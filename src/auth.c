/* Checks credentials against the real HomeCloud server (its own login), then logs the temporary session out. */
#include "shell.h"
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

static int connect_hc(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_cfg.upstream_port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    return s;
}

/* one request, returns malloc'd body (NUL-terminated) and HTTP status */
static char *http_x(const char *method, const char *path, const char *token, const char *extra, const char *body, int *status)
{
    int s = connect_hc();
    if (s < 0) return NULL;
    char head[2048];
    int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n", method, path);
    if (n < 0 || (size_t)n >= sizeof head) { close(s); return NULL; }
    if (extra) n += snprintf(head + n, sizeof head - (size_t)n, "%s\r\n", extra);
    if (token) n += snprintf(head + n, sizeof head - (size_t)n, "Authorization: Bearer %s\r\n", token);
    if (body) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n", strlen(body));
    n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (io_all(s, head, (size_t)n) < 0 || (body && io_all(s, body, strlen(body)) < 0)) { close(s); return NULL; }
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap + 1);
    ssize_t r;
    while (buf && len < cap && (r = read(s, buf + len, cap - len)) > 0) len += (size_t)r;
    close(s);
    if (!buf) return NULL;
    buf[len] = 0;
    *status = strncmp(buf, "HTTP/1.", 7) ? 0 : atoi(buf + 9);
    char *b = strstr(buf, "\r\n\r\n");
    char *out = strdup(b ? b + 4 : "");
    free(buf);
    return out;
}

static char *http(const char *method, const char *path, const char *token, const char *body, int *status)
{
    return http_x(method, path, token, NULL, body, status);
}

static void urlenc(char *out, size_t sz, const char *in)
{
    size_t o = 0;
    for (; *in && o + 4 < sz; in++) {
        if (isalnum((unsigned char)*in) || strchr("-._~", *in)) out[o++] = *in;
        else o += (size_t)snprintf(out + o, sz - o, "%%%02X", (unsigned char)*in);
    }
    out[o] = 0;
}

static int jstr(const char *j, const char *key, char *out, size_t sz)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < sz) { if (*p == '\\' && p[1]) p++; out[o++] = *p++; }
    out[o] = 0;
    return 1;
}

int hc_active(void)
{
    int st = 0;
    char *b = http("GET", "/api/ping", NULL, NULL, &st);
    int ok = b && st == 200 && strstr(b, "\"app\":\"HomeCloud\"");
    free(b);
    return ok;
}

int hc_auth(const char *user, const char *pass, char *home, size_t hsz, int *admin, char *err, size_t esz)
{
    char eu[256], ep[1024], body[1400], tok[128] = "", id[64] = "";
    int st = 0, rc = -1;
    char *resp = NULL, *disks = NULL;
    urlenc(eu, sizeof eu, user);
    urlenc(ep, sizeof ep, pass);
    snprintf(body, sizeof body, "user=%s&pass=%s", eu, ep);
    resp = http("POST", "/api/login", NULL, body, &st);
    if (!resp) { snprintf(err, esz, "Le serveur HomeCloud n'est pas actif."); return -1; }
    if (st != 200 || !jstr(resp, "token", tok, sizeof tok)) { snprintf(err, esz, "Identifiant ou mot de passe incorrect."); goto out; }
    *admin = strstr(resp, "\"admin\":true") != NULL;
    disks = http("GET", "/api/disks", tok, NULL, &st);
    if (disks && st == 200) {
        /* walk the disk objects; keep the personal one owned by this user */
        char want[96];
        snprintf(want, sizeof want, "\"owner\":\"%.40s\"", user);
        for (char *o = strstr(disks, "{\"id\":\""); o; ) {
            char *next = strstr(o + 1, "{\"id\":\"");
            char saved = 0;
            if (next) { saved = *next; *next = 0; }
            int mine = strstr(o, "\"personal\":true") && strstr(o, want);
            if (mine) jstr(o, "id", id, sizeof id);
            if (next) *next = saved;
            if (mine) break;
            o = next;
        }
    }
    if (!id[0]) { snprintf(err, esz, "Aucun espace personnel trouvé pour ce compte."); goto out; }
    char dir[700];
    struct stat sb;
    if (personal_dir(id, dir, sizeof dir) < 0 || stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        snprintf(err, esz, "Espace personnel introuvable sur le serveur (disque non monté ?).");
        goto out;
    }
    snprintf(home, hsz, "%s", dir);
    rc = 0;
out:
    if (tok[0]) { int s2; free(http("POST", "/api/logout", tok, "", &s2)); }
    free(resp); free(disks);
    return rc;
}


/* Authenticates an administrator and asks HomeCloud Server to resolve/mount a named disk.
 * The server returns only its opaque disk id; the local daemon derives the mount path. */
int hc_usedisk(const char *admin_user, const char *admin_pass, const char *disk_name,
               char *disk_user, size_t usz, char *disk_home, size_t hsz,
               char *err, size_t esz)
{
    char eu[256], ep[1024], body[1400], tok[128] = "", ed[512], request[600], id[64] = "";
    int st = 0, rc = -1;
    char *resp = NULL, *result = NULL;
    if (!admin_user || !admin_pass || !disk_name || !*disk_name || strlen(admin_user) > 32 ||
        strlen(admin_pass) > 256 || strlen(disk_name) > 64) {
        snprintf(err, esz, "Paramètres invalides."); return -1;
    }
    urlenc(eu, sizeof eu, admin_user);
    urlenc(ep, sizeof ep, admin_pass);
    snprintf(body, sizeof body, "user=%s&pass=%s", eu, ep);
    resp = http("POST", "/api/login", NULL, body, &st);
    if (!resp) { snprintf(err, esz, "Le serveur HomeCloud n'est pas actif."); return -1; }
    if (st != 200 || !jstr(resp, "token", tok, sizeof tok)) {
        snprintf(err, esz, "Identifiant ou mot de passe incorrect."); goto out;
    }
    free(resp); resp = NULL;
    urlenc(ed, sizeof ed, disk_name);
    snprintf(request, sizeof request, "name=%s", ed);
    result = http("POST", "/api/admin/shell_disk", tok, request, &st);
    if (!result) { snprintf(err, esz, "Impossible de contacter le serveur HomeCloud."); goto out; }
    if (st != 200 || !jstr(result, "id", id, sizeof id)) {
        if (!jstr(result, "error", err, esz)) snprintf(err, esz, "Accès au disque refusé.");
        goto out;
    }
    if (personal_dir(id, disk_home, hsz) < 0) { snprintf(err, esz, "Identifiant de disque invalide."); goto out; }
    struct stat sb;
    if (stat(disk_home, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        snprintf(err, esz, "Le disque n'est pas monté ou son dossier est introuvable."); goto out;
    }
    snprintf(disk_user, usz, "%s", admin_user);
    rc = 0;
out:
    if (tok[0]) { int s2; free(http("POST", "/api/logout", tok, "", &s2)); }
    free(resp); free(result);
    memset(ep, 0, sizeof ep); memset(body, 0, sizeof body); memset(tok, 0, sizeof tok);
    return rc;
}

/* ---- Pont d'administration : /api/shell de HomeCloud Server ----
 * Le démon se présente avec la clé du relais (fichier root, 0600) sur la boucle locale ; le compte agissant est
 * celui de la session Shell déjà authentifiée. C'est le serveur qui vérifie la catégorie ACL de ce compte. */
#define BRIDGE_KEY_FILE "/etc/homecloud/shell-bridge.key"

int hc_bridge_key(char *out, size_t sz)
{
    FILE *f = fopen(BRIDGE_KEY_FILE, "r");
    if (!f) return -1;
    if (!fgets(out, (int)sz, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return out[0] ? 0 : -1;
}

/* Crée la clé si elle n'existe pas (installation). 0 = présente ou créée. */
int hc_bridge_key_ensure(void)
{
    char k[160];
    if (hc_bridge_key(k, sizeof k) == 0) return 0;
    unsigned char rnd[32];
    FILE *r = fopen("/dev/urandom", "r");
    if (!r || fread(rnd, 1, sizeof rnd, r) != sizeof rnd) { if (r) fclose(r); return -1; }
    fclose(r);
    for (int i = 0; i < 32; i++) snprintf(k + i * 2, 3, "%02x", rnd[i]);
    mkdir_p("/etc/homecloud", 0755);
    int fd = open(BRIDGE_KEY_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t l = strlen(k);
    int ok = write(fd, k, l) == (ssize_t)l && write(fd, "\n", 1) == 1;
    close(fd);
    chmod(BRIDGE_KEY_FILE, 0600);
    return ok ? 0 : -1;
}

/* params : paire(s) clé=valeur déjà encodées (a=b&c=d). GET : dans l'URL ; POST : dans le corps.
 * Retourne le corps JSON (malloc) ou NULL si le serveur ne répond pas ; *status = code HTTP (0 si aucune clé). */
char *hc_bridge(const char *method, const char *route, const char *params, int *status)
{
    char key[160], hdr[200], path[2048];
    *status = 0;
    if (hc_bridge_key(key, sizeof key) < 0) return NULL;
    snprintf(hdr, sizeof hdr, "X-HomeCloud-Shell-Key: %s", key);
    if (!strcmp(method, "GET")) {
        snprintf(path, sizeof path, "%s?%s", route, params ? params : "");
        char *b = http_x("GET", path, NULL, hdr, NULL, status);
        memset(key, 0, sizeof key); memset(hdr, 0, sizeof hdr);
        return b;
    }
    char *b = http_x("POST", route, NULL, hdr, params ? params : "", status);
    memset(key, 0, sizeof key); memset(hdr, 0, sizeof hdr);
    return b;
}

void hc_urlenc(char *out, size_t sz, const char *in) { urlenc(out, sz, in); }
int  hc_jstr(const char *j, const char *key, char *out, size_t sz) { return jstr(j, key, out, sz); }
