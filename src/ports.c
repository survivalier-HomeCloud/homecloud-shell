/* Which ports does a user's sandbox really listen on?
 * A port is "theirs" only if a LISTEN socket on it is held by a process descended from one of their live sessions.
 * That is what stops /port/<user>/<port>/ from becoming a way to reach HomeCloud, SSH, databases... */
#include "shell.h"
#include <dirent.h>
#include <unistd.h>
#include <strings.h>

typedef struct { char user[40]; pid_t pid; } sess_t;
#define MAX_SESS 256
static sess_t sess[MAX_SESS];
static int nsess;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

void sess_add(const char *user, pid_t pid)
{
    pthread_mutex_lock(&lk);
    if (nsess < MAX_SESS) { snprintf(sess[nsess].user, sizeof sess[nsess].user, "%s", user); sess[nsess++].pid = pid; }
    pthread_mutex_unlock(&lk);
}

void sess_del(pid_t pid)
{
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nsess; i++)
        if (sess[i].pid == pid) { sess[i] = sess[--nsess]; break; }
    pthread_mutex_unlock(&lk);
}

int sess_count(const char *user)
{
    int c = 0;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nsess; i++) if (!user || !strcmp(sess[i].user, user)) c++;
    pthread_mutex_unlock(&lk);
    return c;
}

/* inodes of LISTEN sockets on this port (tcp + tcp6) */
static int listen_inodes(int port, unsigned long *out, int max)
{
    int n = 0;
    const char *files[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    for (int f = 0; f < 2; f++) {
        FILE *fp = fopen(files[f], "r");
        char line[512];
        if (!fp) continue;
        if (!fgets(line, sizeof line, fp)) { fclose(fp); continue; }
        while (fgets(line, sizeof line, fp)) {
            char la[64], ra[64];
            unsigned st, lp = 0;
            unsigned long inode = 0;
            int sl;
            /* sl local rem st tx:rx tr:when retrnsmt uid timeout inode */
            if (sscanf(line, " %d: %63[0-9A-Fa-f]:%x %63s %x %*s %*s %*s %*s %*s %lu", &sl, la, &lp, ra, &st, &inode) < 6) continue;
            if (st == 0x0A && (int)lp == port && inode && n < max) out[n++] = inode;
        }
        fclose(fp);
    }
    return n;
}

static int ppid_of(pid_t pid)
{
    char f[64], buf[512];
    snprintf(f, sizeof f, "/proc/%d/stat", (int)pid);
    FILE *fp = fopen(f, "r");
    if (!fp) return -1;
    size_t k = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[k] = 0;
    char *rp = strrchr(buf, ')');
    int pp = -1;
    if (!rp || sscanf(rp + 2, "%*c %d", &pp) != 1) return -1;
    return pp;
}

static int holds_socket(pid_t pid, const unsigned long *inodes, int ni)
{
    char d[64], l[300], t[64];
    snprintf(d, sizeof d, "/proc/%d/fd", (int)pid);
    DIR *dir = opendir(d);
    struct dirent *e;
    int found = 0;
    if (!dir) return 0;
    while (!found && (e = readdir(dir))) {
        snprintf(l, sizeof l, "/proc/%d/fd/%s", (int)pid, e->d_name);
        ssize_t k = readlink(l, t, sizeof t - 1);
        if (k <= 0) continue;
        t[k] = 0;
        unsigned long ino;
        if (sscanf(t, "socket:[%lu]", &ino) == 1)
            for (int i = 0; i < ni; i++) if (inodes[i] == ino) { found = 1; break; }
    }
    closedir(dir);
    return found;
}

int port_owned(const char *user, int port)
{
    unsigned long inodes[16];
    int ni = listen_inodes(port, inodes, 16);
    if (!ni) return 0;
    pid_t roots[MAX_SESS];
    int nr = 0;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nsess; i++) if (!strcmp(sess[i].user, user)) roots[nr++] = sess[i].pid;
    pthread_mutex_unlock(&lk);
    if (!nr) return 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    int ok = 0;
    if (!d) return 0;
    while (!ok && (e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        pid_t p = (pid_t)atoi(e->d_name);
        if (!holds_socket(p, inodes, ni)) continue;
        /* is p a descendant of one of the user's sessions? */
        pid_t cur = p;
        for (int depth = 0; depth < 64 && cur > 1; depth++) {
            for (int i = 0; i < nr; i++) if (roots[i] == cur) ok = 1;
            if (ok) break;
            cur = ppid_of(cur);
            if (cur < 0) break;
        }
    }
    closedir(d);
    return ok;
}

/* ---------------- new listeners: announce services as they open ---------------- */

/* HomeCloud Web reaches the service on 127.0.0.1: only sockets bound to "any" or loopback can be proxied */
static int local_reachable(const char *a)
{
    return !strcasecmp(a, "00000000") || !strcasecmp(a, "0100007F") ||
           !strcasecmp(a, "00000000000000000000000000000000") ||
           !strcasecmp(a, "00000000000000000000000001000000") ||
           !strcasecmp(a, "0000000000000000FFFF000000000000") ||
           !strcasecmp(a, "0000000000000000FFFF00000100007F");
}

static int list_listeners(unsigned long *ino, int *port, int max)
{
    int n = 0;
    const char *files[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    for (int f = 0; f < 2; f++) {
        FILE *fp = fopen(files[f], "r");
        char line[512];
        if (!fp) continue;
        if (!fgets(line, sizeof line, fp)) { fclose(fp); continue; }
        while (fgets(line, sizeof line, fp)) {
            char la[64], ra[64];
            unsigned st, lp = 0;
            unsigned long inode = 0;
            int sl;
            if (sscanf(line, " %d: %63[0-9A-Fa-f]:%x %63s %x %*s %*s %*s %*s %*s %lu", &sl, la, &lp, ra, &st, &inode) < 6) continue;
            if (st == 0x0A && inode && local_reachable(la) && n < max) { ino[n] = inode; port[n] = (int)lp; n++; }
        }
        fclose(fp);
    }
    return n;
}

static int descends(pid_t p, pid_t root)
{
    pid_t cur = p;
    for (int depth = 0; depth < 64 && cur > 1; depth++) {
        if (cur == root) return 1;
        cur = ppid_of(cur);
        if (cur < 0) break;
    }
    return 0;
}

/* sets held[i] for every ino[i] that pid has open; returns how many */
static int mark_held(pid_t pid, const unsigned long *ino, int ni, char *held)
{
    char d[64], l[300], t[64];
    int found = 0;
    snprintf(d, sizeof d, "/proc/%d/fd", (int)pid);
    DIR *dir = opendir(d);
    struct dirent *e;
    if (!dir) return 0;
    while ((e = readdir(dir))) {
        snprintf(l, sizeof l, "/proc/%d/fd/%s", (int)pid, e->d_name);
        ssize_t k = readlink(l, t, sizeof t - 1);
        if (k <= 0) continue;
        t[k] = 0;
        unsigned long x;
        if (sscanf(t, "socket:[%lu]", &x) == 1)
            for (int i = 0; i < ni; i++) if (ino[i] == x && !held[i]) { held[i] = 1; found++; }
    }
    closedir(dir);
    return found;
}

int sess_watch(pid_t root, lwatch_t *w, int *ports, int max)
{
    unsigned long cur[WATCH_MAX], fresh[WATCH_MAX];
    int cp[WATCH_MAX], fport[WATCH_MAX], nc = list_listeners(cur, cp, WATCH_MAX), nf = 0, k = 0, np = 0;

    for (int i = 0; i < w->n; i++) {                      /* forget sockets that are closed */
        int alive = 0;
        for (int j = 0; j < nc; j++) if (cur[j] == w->ino[i]) { alive = 1; break; }
        if (alive) { w->ino[k] = w->ino[i]; w->port[k] = w->port[i]; w->ours[k] = w->ours[i]; k++; }
    }
    w->n = k;
    for (int j = 0; j < nc; j++) {                        /* sockets we have not classified yet */
        int known = 0;
        for (int i = 0; i < w->n; i++) if (w->ino[i] == cur[j]) { known = 1; break; }
        if (!known) { fresh[nf] = cur[j]; fport[nf] = cp[j]; nf++; }
    }
    if (!nf) return 0;

    char held[WATCH_MAX];
    memset(held, 0, sizeof held);
    DIR *d = opendir("/proc");
    struct dirent *e;
    if (d) {
        while ((e = readdir(d))) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            pid_t p = (pid_t)atoi(e->d_name);
            char h[WATCH_MAX];
            memset(h, 0, sizeof h);
            if (!mark_held(p, fresh, nf, h) || !descends(p, root)) continue;
            for (int i = 0; i < nf; i++) if (h[i]) held[i] = 1;
        }
        closedir(d);
    }
    for (int i = 0; i < nf && w->n < WATCH_MAX; i++) {
        int dup = 0;                                      /* same port on IPv4 + IPv6: announce once */
        if (held[i]) for (int j = 0; j < w->n; j++) if (w->ours[j] && w->port[j] == fport[i]) { dup = 1; break; }
        w->ino[w->n] = fresh[i]; w->port[w->n] = fport[i]; w->ours[w->n] = held[i]; w->n++;
        if (held[i] && !dup && np < max) ports[np++] = fport[i];
    }
    return np;
}
