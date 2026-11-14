/* homecloud-shell : styled CLI client.   homecloud-shell [user@]host[:port] [-- command] */
#include "shell.h"
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>

static struct termios saved;
static int raw_on;
static volatile sig_atomic_t winch;

static void raw(int on)
{
    if (!isatty(STDIN_FILENO)) return;
    if (on && !raw_on) {
        struct termios t;
        tcgetattr(STDIN_FILENO, &saved);
        t = saved;
        cfmakeraw(&t);
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        raw_on = 1;
    } else if (!on && raw_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        raw_on = 0;
    }
}

static void on_winch(int s) { (void)s; winch = 1; }

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* full-screen programs (vim, htop...) switch to the alternate screen: never draw over them */
static int alt_screen(int cur, const unsigned char *b, size_t n)
{
    for (size_t i = 0; i + 5 < n; i++) {
        if (b[i] != 0x1b || b[i + 1] != '[' || b[i + 2] != '?') continue;
        size_t j = i + 3;
        int v = 0;
        while (j < n && b[j] >= '0' && b[j] <= '9' && v < 100000) v = v * 10 + (b[j++] - '0');
        if (j < n && (v == 1049 || v == 1047 || v == 47) && (b[j] == 'h' || b[j] == 'l')) cur = (b[j] == 'h');
    }
    return cur;
}

static int vis_len(const char *s)
{
    int w = 0;
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) if ((*q & 0xC0) != 0x80) w++;
    return w;
}

/* a framed line with the address of a service that just started in the shell */
static void show_open(const char *host, int wport, const char *user, int port)
{
    char url[640], text[700], out[4096];
    int v6 = strchr(host, ':') != NULL, n = 0, w;
    struct winsize ws = { 24, 80, 0, 0 };
    snprintf(url, sizeof url, "http://%s%s%s:%d/port/%s/%d/", v6 ? "[" : "", host, v6 ? "]" : "", wport, user, port);
    snprintf(text, sizeof text, "Port %d ouvert : %s", port, url);
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    if (ws.ws_col && vis_len(text) + 4 > ws.ws_col) snprintf(text, sizeof text, "%s", url);   /* narrow terminal: link only */
    w = vis_len(text);
#define ADD(...) do { if (n < (int)sizeof out) n += snprintf(out + n, sizeof out - (size_t)n, __VA_ARGS__); } while (0)
    ADD("\r\n\033[38;5;75m┌");
    for (int i = 0; i < w + 2; i++) ADD("─");
    ADD("┐\033[0m\r\n\033[38;5;75m│\033[0m \033[1m%s\033[0m \033[38;5;75m│\033[0m\r\n\033[38;5;75m└", text);
    for (int i = 0; i < w + 2; i++) ADD("─");
    ADD("┘\033[0m\r\n");
#undef ADD
    if (n > (int)sizeof out) n = (int)sizeof out;
    io_all(STDOUT_FILENO, out, (size_t)n);
}

static void usage(void)
{
    ui_banner();
    printf("  Usage : homecloud-shell [utilisateur@]hôte[:port] [-- commande]\n\n");
    ui_kv("hôte:port", "adresse de HomeCloud Web (port 8680 par défaut)");
    ui_kv("utilisateur", "compte HomeCloud (demandé si absent)");
    ui_kv("-- commande", "exécute une commande puis se déconnecte");
    ui_kv("HOMECLOUD_PASSWORD", "mot de passe (scripts) au lieu de la saisie");
    printf("\n  Pendant la session : touche Entrée, puis ~ puis .  pour se déconnecter.\n\n");
}

static int dial(const char *host, int port)
{
    struct addrinfo hints, *res, *a;
    char ps[16];
    int fd = -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    for (a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) { int one = 1; setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one); setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
    return fd;
}

static void winsize_frame(int fd)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return;
    unsigned char b[4] = { (unsigned char)(ws.ws_row >> 8), (unsigned char)ws.ws_row, (unsigned char)(ws.ws_col >> 8), (unsigned char)ws.ws_col };
    frame_send(fd, F_WIN, b, 4);
}

int main(int argc, char **argv)
{
    char user[64] = "", host[256] = "", pass[300] = "", cmd[1024] = "";
    int port = HS_DEFAULT_WEB, i;
    const char *target = NULL;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            for (i++; i < argc; i++) { if (cmd[0]) strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1); strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1); }
            break;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        if (!strcmp(argv[i], "--version")) { printf("homecloud-shell %s\n", HS_VERSION); return 0; }
        if (!target) target = argv[i]; else { usage(); return 2; }
    }
    if (!target) { usage(); return 2; }

    const char *at = strchr(target, '@');
    const char *hp = target;
    if (at) { snprintf(user, sizeof user, "%.*s", (int)(at - target), target); hp = at + 1; }
    snprintf(host, sizeof host, "%s", hp);
    char *colon = strrchr(host, ':');
    if (colon && !strchr(colon, ']')) { *colon = 0; port = atoi(colon + 1); }
    if (!host[0] || port < 1 || port > 65535) { fprintf(stderr, "Adresse invalide : %s\n", target); return 2; }

    int interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (interactive) ui_banner();
    if (!user[0]) {
        if (!interactive) { fprintf(stderr, "Utilisateur manquant (utilisateur@hôte).\n"); return 2; }
        ui_ask("Utilisateur", NULL, user, sizeof user);
    }
    const char *envpw = getenv("HOMECLOUD_PASSWORD");
    if (envpw) snprintf(pass, sizeof pass, "%s", envpw);
    else if (interactive) { char label[160]; snprintf(label, sizeof label, "Mot de passe de %s", user); ui_ask_secret(label, pass, sizeof pass); }
    else { if (!fgets(pass, sizeof pass, stdin)) pass[0] = 0; pass[strcspn(pass, "\r\n")] = 0; }

    int fd = dial(host, port);
    if (fd < 0) { ui_err("Impossible de joindre %s:%d.", host, port); return 1; }
    char rq[512];
    int n = snprintf(rq, sizeof rq, "GET /shell HTTP/1.1\r\nHost: %s:%d\r\nConnection: Upgrade\r\nUpgrade: hcshell\r\nUser-Agent: homecloud-shell/%s\r\n\r\n", host, port, HS_VERSION);
    io_all(fd, rq, (size_t)n);

    /* HTTP answer: 101 means HomeCloud Shell is behind HomeCloud Web */
    char head[2048];
    size_t hl = 0;
    while (hl < sizeof head - 1) {
        if (read(fd, head + hl, 1) != 1) break;
        hl++;
        head[hl] = 0;
        if (hl >= 4 && !memcmp(head + hl - 4, "\r\n\r\n", 4)) break;
    }
    if (strncmp(head, "HTTP/1.1 101", 12)) {
        if (strstr(head, " 404 ")) ui_err("HomeCloud Shell n'est pas installé ou pas démarré sur ce serveur.");
        else if (strstr(head, " 503 ")) ui_err("Le serveur HomeCloud n'est pas actif.");
        else ui_err("%s:%d ne répond pas comme HomeCloud Web.", host, port);
        return 1;
    }

    struct winsize ws = { 24, 80, 0, 0 };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    const char *term = getenv("TERM");
    unsigned char *buf = malloc(FRAME_MAX + 1);
    char auth[2200];
    size_t al = 0;
    al += (size_t)snprintf(auth + al, sizeof auth - al, "%s", user) + 1;
    al += (size_t)snprintf(auth + al, sizeof auth - al, "%s", pass) + 1;
    al += (size_t)snprintf(auth + al, sizeof auth - al, "%s", cmd) + 1;
    al += (size_t)snprintf(auth + al, sizeof auth - al, "%d", ws.ws_row) + 1;
    al += (size_t)snprintf(auth + al, sizeof auth - al, "%d", ws.ws_col) + 1;
    al += (size_t)snprintf(auth + al, sizeof auth - al, "%s", term ? term : "xterm-256color") + 1;
    frame_send(fd, F_AUTH, auth, (uint32_t)al);
    memset(pass, 0, sizeof pass);
    memset(auth, 0, sizeof auth);

    int t;
    uint32_t len;
    if (frame_recv(fd, &t, buf, FRAME_MAX, &len) < 0) { ui_err("Connexion interrompue."); return 1; }
    buf[len] = 0;
    if (t == F_ERR) { ui_err("%s", (char *)buf); return 1; }
    if (t != F_INFO) { ui_err("Réponse inattendue du serveur."); return 1; }
    if (interactive) {
        char *nm = (char *)buf, *be = strchr(nm, '\n'), *home = NULL, *net = NULL;
        if (be) { *be++ = 0; home = strchr(be, '\n'); }
        if (home) { *home++ = 0; net = strchr(home, '\n'); }
        if (net) *net++ = 0;
        ui_ok("Connecté à %s:%d en tant que %s", host, port, nm);
        ui_kv("Espace personnel", home ? home : "?");
        char b2[100];
        snprintf(b2, sizeof b2, "%s · %s", be ? be : "?", net ? net : "?");
        ui_kv("Bac à sable", b2);
        printf("  \033[38;5;245mEntrée puis ~. pour quitter\033[0m\n\n");
    }

    signal(SIGWINCH, on_winch);
    signal(SIGPIPE, SIG_IGN);
    raw(1);
    int code = 0, bol = 1, tilde = 0, stdin_open = 1, alt = 0, npend = 0, pend[16];
    long long last_data = now_ms();
    struct pollfd p[2] = { { .fd = STDIN_FILENO, .events = POLLIN }, { .fd = fd, .events = POLLIN } };
    for (;;) {
        if (winch) { winch = 0; winsize_frame(fd); }
        p[0].fd = stdin_open ? STDIN_FILENO : -1;
        if (poll(p, 2, npend ? 100 : 500) < 0) { if (errno == EINTR) continue; break; }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(STDIN_FILENO, buf, 4096);
            if (r <= 0) { stdin_open = 0; }
            else {
                /* "~." right after a newline disconnects, like ssh */
                for (ssize_t k = 0; k < r; k++) {
                    unsigned char c = buf[k];
                    if (tilde && c == '.') { raw(0); printf("\r\nDéconnecté.\r\n"); return 0; }
                    tilde = bol && c == '~';
                    bol = (c == '\r' || c == '\n');
                }
                if (frame_send(fd, F_DATA, buf, (uint32_t)r) < 0) break;
            }
        }
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (frame_recv(fd, &t, buf, FRAME_MAX, &len) < 0) { code = 255; raw(0); fprintf(stderr, "\r\nConnexion perdue.\r\n"); return code; }
            if (t == F_DATA) { if (io_all(STDOUT_FILENO, buf, len) < 0) break; alt = alt_screen(alt, buf, len); last_data = now_ms(); }
            else if (t == F_OPEN && interactive && len > 0 && len < 8) {
                int pp;
                buf[len] = 0;
                pp = atoi((char *)buf);
                if (pp > 0 && pp <= 65535 && npend < 16) pend[npend++] = pp;
            }
            else if (t == F_EXIT && len == 4) { code = (int)((uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3]); break; }
            else if (t == F_ERR) { buf[len] = 0; raw(0); fprintf(stderr, "\r\n%s\r\n", (char *)buf); return 1; }
        }
        if (npend && !alt && now_ms() - last_data >= 250) {      /* terminal is quiet: show the framed address(es) */
            for (int k = 0; k < npend; k++) show_open(host, port, user, pend[k]);
            npend = 0;
        }
    }
    raw(0);
    if (interactive) printf("\n\033[38;5;245mConnexion fermée.\033[0m\n");
    return code & 255;
}
