/* homecloud-shelld: accepts shell sessions on a unix socket (HomeCloud Web relays /shell to it). */
#include "shell.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>

#define MAX_USER_SESSIONS 5
#define MAX_SESSIONS      64

static volatile int running = 1;
static int lfd = -1;

/* ---- brute-force brake: 5 failures per address -> 60 s lock ---- */
typedef struct { char ip[48]; int fails; time_t until, last; } fail_t;
static fail_t fails[128];
static pthread_mutex_t flk = PTHREAD_MUTEX_INITIALIZER;

static int blocked(const char *ip)
{
    int left = 0;
    pthread_mutex_lock(&flk);
    for (int i = 0; i < 128; i++)
        if (!strcmp(fails[i].ip, ip) && fails[i].until > time(NULL)) left = (int)(fails[i].until - time(NULL));
    pthread_mutex_unlock(&flk);
    return left;
}

static void note_fail(const char *ip, int bad)
{
    pthread_mutex_lock(&flk);
    fail_t *f = NULL, *old = &fails[0];
    for (int i = 0; i < 128; i++) {
        if (!strcmp(fails[i].ip, ip)) { f = &fails[i]; break; }
        if (fails[i].last < old->last) old = &fails[i];
    }
    if (!f) { f = old; memset(f, 0, sizeof *f); snprintf(f->ip, sizeof f->ip, "%s", ip); }
    time_t now = time(NULL);
    if (!bad) { f->fails = 0; f->until = 0; }
    else {
        if (now - f->last > 300) f->fails = 0;
        if (++f->fails >= 5) { f->until = now + 60; f->fails = 0; }
    }
    f->last = now;
    pthread_mutex_unlock(&flk);
}

static int valid_user(const char *u)
{
    size_t n = strlen(u);
    if (n < 2 || n > 32) return 0;
    for (; *u; u++)
        if (!((*u >= 'a' && *u <= 'z') || (*u >= 'A' && *u <= 'Z') || (*u >= '0' && *u <= '9') || strchr("._-", *u))) return 0;
    return 1;
}

static void send_err(int fd, const char *m) { frame_send(fd, F_ERR, m, (uint32_t)strlen(m)); }

static void session(int fd, const char *peer, const char *user, const char *home, const char *cmd, const char *term, int rows, int cols, int nested);

#define HC_MARK_START "\036HC_"   /* suivi de USEDISK: ou CMD: */
#define HC_MARK_END   0x1f
#define HC_PENDING_MAX 8192

static int hex_decode(const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);
    if ((n & 1) || n / 2 >= cap) return -1;
    for (size_t i = 0; i < n; i += 2) {
        unsigned a, b;
        char x = in[i], y = in[i + 1];
        if (x >= '0' && x <= '9') a = (unsigned)(x - '0');
        else if (x >= 'a' && x <= 'f') a = (unsigned)(x - 'a' + 10);
        else if (x >= 'A' && x <= 'F') a = (unsigned)(x - 'A' + 10);
        else return -1;
        if (y >= '0' && y <= '9') b = (unsigned)(y - '0');
        else if (y >= 'a' && y <= 'f') b = (unsigned)(y - 'a' + 10);
        else if (y >= 'A' && y <= 'F') b = (unsigned)(y - 'A' + 10);
        else return -1;
        out[i / 2] = (char)((a << 4) | b);
        if (!out[i / 2]) return -1;
    }
    out[n / 2] = 0;
    return 0;
}

static int marker_open_disk(int fd, const char *peer, const char *term, int rows, int cols, char *payload)
{
    char *a = strchr(payload, ':'), *b;
    char admin[64] = "", pass[300] = "", disk[130] = "", disk_user[64] = "", home[700] = "", err[256] = "";
    if (!a) goto malformed;
    *a++ = 0; b = strchr(a, ':'); if (!b) goto malformed;
    *b++ = 0;
    /* The three fields are separated by exactly two colons; disk is the remainder. */
    if (hex_decode(payload, admin, sizeof admin) < 0 || hex_decode(a, pass, sizeof pass) < 0 ||
        hex_decode(b, disk, sizeof disk) < 0 || !valid_user(admin) || !disk[0]) goto malformed;
    if (sess_count(admin) >= MAX_USER_SESSIONS || sess_count(NULL) >= MAX_SESSIONS) {
        const char *msg = "\r\nHomeCloud : limite de sessions atteinte pour cet administrateur.\r\n";
        frame_send(fd, F_DATA, msg, (uint32_t)strlen(msg));
        memset(pass, 0, sizeof pass);
        return 0;
    }
    if (hc_usedisk(admin, pass, disk, disk_user, sizeof disk_user, home, sizeof home, err, sizeof err) < 0) {
        char msg[400]; snprintf(msg, sizeof msg, "\r\n\033[1;31mHomeCloud : %s\033[0m\r\n", err[0] ? err : "accès au disque refusé");
        frame_send(fd, F_DATA, msg, (uint32_t)strlen(msg));
        memset(pass, 0, sizeof pass);
        return 0;
    }
    char msg[400];
    snprintf(msg, sizeof msg, "\r\n\033[1;32mAuthentification réussie. Disque %s monté. Ouverture du shell...\033[0m\r\n", disk);
    frame_send(fd, F_DATA, msg, (uint32_t)strlen(msg));
    logmsg("ouverture du disque '%s' dans une session HomeCloud Shell pour %s depuis %s", disk, admin, peer);
    memset(pass, 0, sizeof pass);
    session(fd, peer, disk_user, home, NULL, term, rows, cols, 1);
    {
        const char *back = "\r\nRetour au shell précédent.\r\n";
        frame_send(fd, F_DATA, back, (uint32_t)strlen(back));
    }
    return 0;
malformed:
    {
        const char *msg = "\r\nHomeCloud : demande usedisk invalide.\r\n";
        frame_send(fd, F_DATA, msg, (uint32_t)strlen(msg));
    }
    memset(pass, 0, sizeof pass);
    return -1;
}

/* Commande d'administration (homecloud server, adduser...) : champs hexadécimaux séparés par « : ».
 * Le compte agissant est celui de la session ; le serveur vérifie l'ACL. Jamais depuis un shell de disque
 * (usedisk) : le compte de ce shell est le propriétaire du disque, pas l'administrateur qui l'a ouvert. */
static void marker_run_cmd(int fd, const char *user, int nested, char *payload)
{
    static const char *bad = "\r\n\033[1;31mHomeCloud : commande invalide.\033[0m\r\n";
    char vals[8][300], *argv[8];
    int argc = 0;
    if (nested) {
        const char *m = "\r\n\033[1;31mHomeCloud : cette commande n'est pas disponible dans un shell de disque.\033[0m\r\n";
        frame_send(fd, F_DATA, m, (uint32_t)strlen(m));
        return;
    }
    for (char *p = payload; argc < 8; ) {
        char *c = strchr(p, ':');
        if (c) *c = 0;
        if (hex_decode(p, vals[argc], sizeof vals[argc]) < 0) { frame_send(fd, F_DATA, bad, (uint32_t)strlen(bad)); goto wipe; }
        argv[argc] = vals[argc];
        argc++;
        if (!c) break;
        p = c + 1;
    }
    if (!argc || !vals[0][0] || !valid_user(user)) { frame_send(fd, F_DATA, bad, (uint32_t)strlen(bad)); goto wipe; }
    {
        char *out = malloc(16384);
        if (!out) goto wipe;
        logmsg("commande d'administration '%s' demandée par %s", argv[0], user);
        hc_admin_command(user, argc, argv, out, 16384);
        size_t len = strlen(out);
        frame_send(fd, F_DATA, "\r\n", 2);
        for (size_t off = 0; off < len; ) {
            size_t chunk = len - off > 8192 ? 8192 : len - off;
            if (frame_send(fd, F_DATA, out + off, (uint32_t)chunk) < 0) break;
            off += chunk;
        }
        free(out);
    }
wipe:
    memset(vals, 0, sizeof vals);
}

/* Strip the private control marker from PTY output. Partial markers are buffered so
 * credentials are never sent to the remote terminal/client. */
static int relay_pty(int fd, const char *peer, const char *user, int nested, const char *term, int rows, int cols,
                     unsigned char *pending, size_t *plen, const unsigned char *data, size_t n)
{
    static const unsigned char start[] = HC_MARK_START;
    const size_t slen = sizeof start - 1;

    /* A PTY commonly returns one byte per keystroke.  Keeping a fixed
     * (marker_length - 1) tail here delayed terminal echo by ~11 characters.
     * Keep only the suffix that could actually be the beginning of our private
     * marker; ordinary terminal output is forwarded immediately. */
    if (*plen + n >= HC_PENDING_MAX) {
        /* An incomplete control marker is small; a large pending buffer is
         * ordinary output and can safely be flushed. */
        if (*plen && frame_send(fd, F_DATA, pending, (uint32_t)*plen) < 0) return -1;
        *plen = 0;
    }
    if (n > HC_PENDING_MAX - *plen) return -1;
    memcpy(pending + *plen, data, n);
    *plen += n;

    for (;;) {
        unsigned char *hit = NULL;
        if (*plen >= slen) {
            for (size_t i = 0; i + slen <= *plen; i++) {
                if (!memcmp(pending + i, start, slen)) { hit = pending + i; break; }
            }
        }

        if (!hit) {
            /* Preserve only a suffix matching a prefix of HC_MARK_START. */
            size_t keep = *plen < slen - 1 ? *plen : slen - 1;
            while (keep && memcmp(pending + *plen - keep, start, keep) != 0) keep--;
            size_t sendn = *plen - keep;
            if (sendn && frame_send(fd, F_DATA, pending, (uint32_t)sendn) < 0) return -1;
            if (keep) memmove(pending, pending + sendn, keep);
            *plen = keep;
            return 0;
        }

        size_t before = (size_t)(hit - pending);
        if (before && frame_send(fd, F_DATA, pending, (uint32_t)before) < 0) return -1;
        size_t off = before + slen;
        unsigned char *end = memchr(pending + off, HC_MARK_END, *plen - off);
        if (!end) {
            /* Keep the complete marker and its partial payload until the end
             * delimiter arrives; none of it is exposed to the terminal. */
            memmove(pending, pending + before, *plen - before);
            *plen -= before;
            return 0;
        }

        size_t mlen = (size_t)(end - (pending + off));
        char payload[3200];
        if (mlen >= sizeof payload) mlen = sizeof payload - 1;
        memcpy(payload, pending + off, mlen);
        payload[mlen] = 0;
        size_t consumed = (size_t)(end - pending) + 1;
        size_t remain = *plen - consumed;
        memmove(pending, pending + consumed, remain);
        memset(pending + remain, 0, consumed);
        *plen = remain;
        if (!strncmp(payload, "USEDISK:", 8)) marker_open_disk(fd, peer, term, rows, cols, payload + 8);
        else if (!strncmp(payload, "CMD:", 4)) marker_run_cmd(fd, user, nested, payload + 4);
        else {
            const char *m = "\r\nHomeCloud : demande invalide.\r\n";
            frame_send(fd, F_DATA, m, (uint32_t)strlen(m));
        }
        memset(payload, 0, sizeof payload);
        /* Process any output following the marker (normally the outer prompt). */
    }
}

static void session(int fd, const char *peer, const char *user, const char *home, const char *cmd, const char *term, int rows, int cols, int nested)
{
    int master;
    pid_t pid;
    char root[64];
    spawn_t sp = { .user = user, .home_src = home, .cmd = cmd, .term = term, .rows = rows, .cols = cols };
    if (sandbox_spawn(&sp, &master, &pid, root, sizeof root) < 0) {
        const char *m = "Impossible de créer le bac à sable.\r\n";
        if (nested) frame_send(fd, F_DATA, m, (uint32_t)strlen(m)); else send_err(fd, "Impossible de créer le bac à sable.");
        return;
    }
    sess_add(user, pid);
    logmsg("session ouverte : %s depuis %s (pid %d, %s)", user, peer, (int)pid, g_cfg.backend);

    char info[300];
    if (!nested) {
        int n = snprintf(info, sizeof info, "%s\n%s\n/home/%s\n%s", user, g_cfg.backend, user, g_cfg.network ? "réseau" : "sans réseau");
        frame_send(fd, F_INFO, info, (uint32_t)n);
    }

    unsigned char *buf = malloc(FRAME_MAX);
    struct pollfd p[2] = { { .fd = fd, .events = POLLIN }, { .fd = master, .events = POLLIN } };
    int client_gone = 0;
    lwatch_t lw;
    time_t last_scan = 0;
    memset(&lw, 0, sizeof lw);
    unsigned char pending[HC_PENDING_MAX];
    size_t pending_len = 0;
    while (running) {
        if (poll(p, 2, 1000) < 0) { if (errno == EINTR) continue; break; }
        if (g_cfg.network && time(NULL) != last_scan) {      /* a service started in the shell? tell the client */
            int op[8], no = sess_watch(pid, &lw, op, 8);
            last_scan = time(NULL);
            for (int i = 0; i < no; i++) {
                char t[12];
                int l = snprintf(t, sizeof t, "%d", op[i]);
                logmsg("port %d ouvert dans la session de %s", op[i], user);
                if (frame_send(fd, F_OPEN, t, (uint32_t)l) < 0) client_gone = 1;
            }
            if (client_gone) break;
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            int t;
            uint32_t len;
            if (frame_recv(fd, &t, buf, FRAME_MAX, &len) < 0) { client_gone = 1; break; }
            if (t == F_DATA && len) { if (io_all(master, buf, len) < 0) break; }
            else if (t == F_WIN && len == 4) {
                struct winsize ws = { .ws_row = (unsigned short)(buf[0] << 8 | buf[1]), .ws_col = (unsigned short)(buf[2] << 8 | buf[3]) };
                ioctl(master, TIOCSWINSZ, &ws);
            }
        }
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = read(master, buf, 4096);
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }          /* shell ended */
            if (relay_pty(fd, peer, user, nested, term, rows, cols, pending, &pending_len, buf, (size_t)r) < 0) { client_gone = 1; break; }
        }
    }
    if (pending_len && !client_gone) frame_send(fd, F_DATA, pending, (uint32_t)pending_len);
    free(buf);
    int st = 0, code = 0;
    if (client_gone || !running) sandbox_kill(pid);
    close(master);
    sandbox_kill(pid);                                   /* nothing survives the session */
    waitpid(pid, &st, 0);
    code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    sess_del(pid);
    sandbox_cleanup(root);
    if (!client_gone && !nested) {
        unsigned char c[4] = { (unsigned char)(code >> 24), (unsigned char)(code >> 16), (unsigned char)(code >> 8), (unsigned char)code };
        frame_send(fd, F_EXIT, c, 4);
    }
    logmsg("session fermée : %s (code %d)", user, code);
}

static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char peer[48] = "local";
    unsigned char *buf = malloc(FRAME_MAX + 1);
    struct timeval tv = { .tv_sec = 30 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);       /* authentication must come quickly */
    for (int guard = 0; buf && guard < 4; guard++) {
        int t;
        uint32_t n;
        if (frame_recv(fd, &t, buf, FRAME_MAX, &n) < 0) break;
        buf[n] = 0;
        if (t == F_PEER) { snprintf(peer, sizeof peer, "%.46s", (char *)buf); continue; }
        if (t == F_PORT) {                                           /* ownership question from HomeCloud Web */
            const char *user = (char *)buf, *port = user + strlen(user) + 1;
            int ok = (port < (const char *)buf + n) && valid_user(user) && port_owned(user, atoi(port));
            if (ok) frame_send(fd, F_INFO, "ok", 2); else send_err(fd, "aucun service");
            break;
        }
        if (t != F_AUTH) { send_err(fd, "protocole invalide"); break; }

        /* user\0pass\0cmd\0rows\0cols\0term */
        char *f[6] = { "", "", "", "0", "0", "" };
        char *p = (char *)buf, *end = (char *)buf + n;
        for (int i = 0; i < 6 && p <= end; i++) { f[i] = p; p += strlen(p) + 1; }
        if (!valid_user(f[0]) || strlen(f[1]) > 256 || strlen(f[2]) > 4000) { send_err(fd, "Identifiant ou mot de passe incorrect."); note_fail(peer, 1); break; }
        int left = blocked(peer);
        if (left) { char m[120]; snprintf(m, sizeof m, "Trop d'échecs de connexion : réessayez dans %d s.", left); send_err(fd, m); break; }
        if (sess_count(f[0]) >= MAX_USER_SESSIONS || sess_count(NULL) >= MAX_SESSIONS) { send_err(fd, "Trop de sessions ouvertes."); break; }

        char home[700], err[200] = "";
        int admin = 0;
        if (hc_auth(f[0], f[1], home, sizeof home, &admin, err, sizeof err) < 0) {
            logmsg("connexion refusée : %s depuis %s", f[0], peer);
            if (!strstr(err, "pas actif")) note_fail(peer, 1);
            send_err(fd, err);
            break;
        }
        note_fail(peer, 0);
        tv.tv_sec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char term[64];
        snprintf(term, sizeof term, "%.40s", f[5]);
        for (char *c = term; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) *c = 'x';
        session(fd, peer, f[0], home, f[2], term, atoi(f[3]), atoi(f[4]), 0);
        break;
    }
    free(buf);
    close(fd);
    return NULL;
}

int shell_server_run(void)
{
    char path[700];
    struct sockaddr_un a;
    signal(SIGPIPE, SIG_IGN);
    sock_path(path, sizeof path);
    lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { logmsg("chemin de socket trop long : %s", path); return -1; }
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    unlink(path);
    mode_t old = umask(0177);
    int rc = bind(lfd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (rc < 0 || listen(lfd, 32) < 0) { logmsg("socket %s : %s", path, strerror(errno)); return -1; }
    chmod(path, 0600);
    logmsg("HomeCloud Shell %s prêt (socket %s, bac à sable : %s, réseau : %s)", HS_VERSION, path, g_cfg.backend, g_cfg.network ? "oui" : "non");
    while (running) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (!running) break; continue; }
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, conn_thread, (void *)(intptr_t)fd) != 0) close(fd);
        pthread_attr_destroy(&at);
    }
    unlink(path);
    return 0;
}

void shell_server_stop(void)
{
    running = 0;
    if (lfd >= 0) shutdown(lfd, SHUT_RDWR);
}
