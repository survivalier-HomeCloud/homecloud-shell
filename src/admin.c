/* Commandes d'administration du shell : homecloud server | listdisk | listuser | adduser | removeuser |
 * permission | adddisk | removedisk | linkdisk.
 * Chaque commande appelle une route « api/shell » de HomeCloud Server. Le compte agissant est celui de la session ;
 * le serveur contrôle lui-même la catégorie ACL (ServerInfo, DiskList, UserManage...). */
#include "shell.h"
#include <ctype.h>
#include <strings.h>
#include <unistd.h>

#define C_OK  "\033[1;32m"
#define C_ERR "\033[1;31m"
#define C_KEY "\033[1;36m"
#define C_DIM "\033[2m"
#define C_OFF "\033[0m"

static const char *const CATS[] = { "ServerInfo", "DiskList", "UseDisk", "UserManage", "PermissionManage", "DiskManage", "DiskLink", NULL };

/* ---- petits utilitaires d'écriture dans le tampon de sortie ---- */
typedef struct { char *p; size_t n, cap; } ob_t;

static void ob_f(ob_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ob_f(ob_t *o, const char *fmt, ...)
{
    if (o->n + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->p + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    o->n += (size_t)w < o->cap - o->n ? (size_t)w : o->cap - o->n - 1;
}

/* ---- lecture JSON minimale (les réponses du serveur sont plates et connues) ---- */
static int jnum(const char *j, const char *key, unsigned long long *v)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    if (!isdigit((unsigned char)*p)) return 0;
    *v = strtoull(p, NULL, 10);
    return 1;
}

static int jbool(const char *j, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":true", key);
    return strstr(j, pat) != NULL;
}

static void human(char *out, size_t sz, unsigned long long b)
{
    const char *u[] = { "o", "Ko", "Mo", "Go", "To" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, sz, "%llu %s", b, u[i]);
    else snprintf(out, sz, "%.1f %s", v, u[i]);
}

/* ---- appel de l'API + gestion uniforme des erreurs ---- */
static char *call(const char *method, const char *route, const char *params, ob_t *o)
{
    int st = 0;
    char *b = hc_bridge(method, route, params, &st);
    if (!b) {
        if (st == 0 && access("/etc/homecloud/shell-bridge.key", R_OK) != 0)
            ob_f(o, C_ERR "HomeCloud : clé du relais absente (/etc/homecloud/shell-bridge.key)." C_OFF "\r\n"
                    "Relancez « homecloud-shelld install » en administrateur.\r\n");
        else
            ob_f(o, C_ERR "HomeCloud : le serveur HomeCloud n'est pas joignable." C_OFF "\r\n");
        return NULL;
    }
    if (st != 200) {
        char e[300] = "";
        if (!hc_jstr(b, "error", e, sizeof e)) snprintf(e, sizeof e, "réponse inattendue du serveur (HTTP %d)", st);
        ob_f(o, C_ERR "HomeCloud : %s" C_OFF "\r\n", e);
        if (st == 403 && strstr(e, "insuffisante"))
            ob_f(o, C_DIM "Un administrateur peut accorder cette autorisation depuis la gestion des comptes de HomeCloud Web." C_OFF "\r\n");
        free(b);
        return NULL;
    }
    return b;
}

static int cat_valid(const char *c)
{
    for (int i = 0; CATS[i]; i++) if (!strcmp(c, CATS[i])) return 1;
    return 0;
}

static void cat_list(ob_t *o)
{
    for (int i = 0; CATS[i]; i++) ob_f(o, "%s%s", i ? ", " : "", CATS[i]);
}

/* ---- commandes ---- */
static int cmd_server(const char *u, ob_t *o)
{
    char par[128], v[96], name[96] = "", ver[32] = "";
    unsigned long long port = 0, nu = 0, nd = 0, tot = 0, fr = 0;
    hc_urlenc(v, sizeof v, u);
    snprintf(par, sizeof par, "user=%s", v);
    char *b = call("GET", "/api/shell/server", par, o);
    if (!b) return 1;
    hc_jstr(b, "name", name, sizeof name);
    hc_jstr(b, "version", ver, sizeof ver);
    jnum(b, "port", &port); jnum(b, "users", &nu); jnum(b, "disks", &nd);
    jnum(b, "storageTotal", &tot); jnum(b, "storageFree", &fr);
    char t[40], f[40], used[40];
    human(t, sizeof t, tot); human(f, sizeof f, fr); human(used, sizeof used, tot > fr ? tot - fr : 0);
    ob_f(o, C_KEY "HomeCloud Server" C_OFF "\r\n");
    ob_f(o, "Nom          : %s\r\n", name[0] ? name : "(sans nom)");
    ob_f(o, "Version      : %s\r\n", ver);
    ob_f(o, "Port         : %llu\r\n", port);
    ob_f(o, "Utilisateurs : %llu\r\n", nu);
    ob_f(o, "Disques      : %llu\r\n", nd);
    ob_f(o, "Stockage     : %s utilisés sur %s (%s libres)\r\n", used, t, f);
    free(b);
    return 0;
}

static int cmd_listdisk(const char *u, ob_t *o)
{
    char par[128], v[96];
    hc_urlenc(v, sizeof v, u);
    snprintf(par, sizeof par, "user=%s", v);
    char *b = call("GET", "/api/shell/listdisk", par, o);
    if (!b) return 1;
    int count = 0;
    ob_f(o, C_KEY "%-24s %-12s %-18s %s" C_OFF "\r\n", "NOM", "TAILLE", "PROPRIETAIRE", "TYPE");
    for (char *d = strstr(b, "{\"id\":\""); d; ) {
        char *next = strstr(d + 1, "{\"id\":\"");
        char sv = 0;
        if (next) { sv = *next; *next = 0; }
        char name[96] = "", owner[48] = "", sz[40];
        unsigned long long size = 0;
        hc_jstr(d, "name", name, sizeof name);
        hc_jstr(d, "owner", owner, sizeof owner);
        jnum(d, "size", &size);
        human(sz, sizeof sz, size);
        const char *type = jbool(d, "readOnly") ? "lecture seule" : owner[0] ? "personnel" : "partagé";
        ob_f(o, "%-24.24s %-12s %-18.18s %s\r\n", name, sz, owner[0] ? owner : "-", type);
        count++;
        if (next) *next = sv;
        d = next;
    }
    if (!count) ob_f(o, C_DIM "Aucun disque." C_OFF "\r\n");
    free(b);
    return 0;
}

static int cmd_listuser(const char *u, ob_t *o)
{
    char par[128], v[96];
    hc_urlenc(v, sizeof v, u);
    snprintf(par, sizeof par, "user=%s", v);
    char *b = call("GET", "/api/shell/users", par, o);
    if (!b) return 1;
    int count = 0;
    ob_f(o, C_KEY "%-24s %s" C_OFF "\r\n", "UTILISATEUR", "ROLE");
    for (char *d = strstr(b, "{\"name\":\""); d; ) {
        char *next = strstr(d + 1, "{\"name\":\"");
        char sv = 0;
        if (next) { sv = *next; *next = 0; }
        char name[48] = "";
        hc_jstr(d, "name", name, sizeof name);
        ob_f(o, "%-24.24s %s\r\n", name, jbool(d, "admin") ? "administrateur" : "utilisateur");
        count++;
        if (next) *next = sv;
        d = next;
    }
    if (!count) ob_f(o, C_DIM "Aucun utilisateur." C_OFF "\r\n");
    free(b);
    return 0;
}

static int cmd_adduser(const char *u, int argc, char **argv, ob_t *o)
{
    if (argc != 3 && argc != 4) { ob_f(o, "Usage: homecloud adduser <utilisateur> [motdepasse] [quota_go]\r\n"); return 2; }
    char eu[96], en[200], ep[900], eq[40], par[1400];
    hc_urlenc(eu, sizeof eu, u);
    hc_urlenc(en, sizeof en, argv[1]);
    hc_urlenc(ep, sizeof ep, argv[2]);
    int n = snprintf(par, sizeof par, "user=%s&name=%s&pass=%s", eu, en, ep);
    if (argc == 4) {
        hc_urlenc(eq, sizeof eq, argv[3]);
        snprintf(par + n, sizeof par - (size_t)n, "&quota_gb=%s", eq);
    }
    char *b = call("POST", "/api/shell/adduser", par, o);
    memset(par, 0, sizeof par); memset(ep, 0, sizeof ep);
    if (!b) return 1;
    ob_f(o, C_OK "Utilisateur %s créé." C_OFF "\r\n", argv[1]);
    free(b);
    return 0;
}

static int cmd_removeuser(const char *u, const char *name, ob_t *o)
{
    char eu[96], en[200], par[400];
    hc_urlenc(eu, sizeof eu, u);
    hc_urlenc(en, sizeof en, name);
    snprintf(par, sizeof par, "user=%s&name=%s", eu, en);
    char *b = call("POST", "/api/shell/removeuser", par, o);
    if (!b) return 1;
    ob_f(o, C_OK "Utilisateur %s supprimé." C_OFF "\r\n", name);
    free(b);
    return 0;
}

static int cmd_permission(const char *u, int argc, char **argv, ob_t *o)
{
    if (argc < 2 || argc > 4) {
        ob_f(o, "Usage: homecloud permission <utilisateur> [catégorie [on|off]]\r\nCatégories : ");
        cat_list(o);
        ob_f(o, "\r\n");
        return 2;
    }
    char eu[96], en[200], par[600];
    hc_urlenc(eu, sizeof eu, u);
    hc_urlenc(en, sizeof en, argv[1]);
    if (argc == 2) {                                   /* affichage */
        snprintf(par, sizeof par, "user=%s&name=%s", eu, en);
        char *b = call("GET", "/api/shell/permission", par, o);
        if (!b) return 1;
        ob_f(o, C_KEY "Autorisations de %s" C_OFF "\r\n", argv[1]);
        for (int i = 0; CATS[i]; i++)
            ob_f(o, "  %-18s %s\r\n", CATS[i], jbool(b, CATS[i]) ? C_OK "oui" C_OFF : C_DIM "non" C_OFF);
        free(b);
        return 0;
    }
    if (!cat_valid(argv[2])) {
        ob_f(o, C_ERR "Catégorie invalide." C_OFF "\r\nCatégories : ");
        cat_list(o);
        ob_f(o, "\r\n");
        return 2;
    }
    int on = 1;
    if (argc == 4) {
        if (!strcmp(argv[3], "on") || !strcmp(argv[3], "1")) on = 1;
        else if (!strcmp(argv[3], "off") || !strcmp(argv[3], "0")) on = 0;
        else { ob_f(o, C_ERR "Valeur invalide : utilisez on ou off." C_OFF "\r\n"); return 2; }
    }
    snprintf(par, sizeof par, "user=%s&name=%s&permission=%s&enabled=%d", eu, en, argv[2], on);
    char *b = call("POST", "/api/shell/permission", par, o);
    if (!b) return 1;
    ob_f(o, C_OK "%s : %s %s." C_OFF "\r\n", argv[1], argv[2], on ? "accordée" : "retirée");
    free(b);
    return 0;
}

static int cmd_adddisk(const char *u, const char *name, const char *size, ob_t *o)
{
    char eu[96], en[200], es[40], par[500];
    hc_urlenc(eu, sizeof eu, u);
    hc_urlenc(en, sizeof en, name);
    hc_urlenc(es, sizeof es, size);
    snprintf(par, sizeof par, "user=%s&name=%s&size_gb=%s", eu, en, es);
    char *b = call("POST", "/api/shell/adddisk", par, o);
    if (!b) return 1;
    ob_f(o, C_OK "Disque %s créé (%s Go)." C_OFF "\r\n", name, size);
    free(b);
    return 0;
}

static int cmd_removedisk(const char *u, const char *name, ob_t *o)
{
    char eu[96], en[200], par[400];
    hc_urlenc(eu, sizeof eu, u);
    hc_urlenc(en, sizeof en, name);
    snprintf(par, sizeof par, "user=%s&name=%s", eu, en);
    char *b = call("POST", "/api/shell/removedisk", par, o);
    if (!b) return 1;
    ob_f(o, C_OK "Disque %s supprimé." C_OFF "\r\n", name);
    free(b);
    return 0;
}

static int cmd_linkdisk(const char *u, const char *disk, const char *user, const char *perm, ob_t *o)
{
    const char *p = NULL;
    if (!strcasecmp(perm, "read") || !strcmp(perm, "r")) p = "r";
    else if (!strcasecmp(perm, "write") || !strcmp(perm, "rw")) p = "rw";
    else if (!strcasecmp(perm, "none")) p = "none";
    if (!p) { ob_f(o, C_ERR "Permission invalide (Read, Write ou None)." C_OFF "\r\n"); return 2; }
    char eu[96], ed[200], eun[200], par[700];
    hc_urlenc(eu, sizeof eu, u);
    hc_urlenc(ed, sizeof ed, disk);
    hc_urlenc(eun, sizeof eun, user);
    snprintf(par, sizeof par, "user=%s&disk=%s&username=%s&perm=%s", eu, ed, eun, p);
    char *b = call("POST", "/api/shell/linkdisk", par, o);
    if (!b) return 1;
    ob_f(o, C_OK "Disque %s : accès de %s = %s." C_OFF "\r\n", disk, user,
         !strcmp(p, "r") ? "lecture" : !strcmp(p, "rw") ? "lecture/écriture" : "aucun");
    free(b);
    return 0;
}

int hc_admin_command(const char *actor, int argc, char **argv, char *out, size_t osz)
{
    ob_t o = { out, 0, osz };
    out[0] = 0;
    if (argc < 1) { ob_f(&o, "HomeCloud : commande vide.\r\n"); return 2; }
    const char *c = argv[0];
    int rc;
#define NEED(n, usage) if (argc != (n)) { ob_f(&o, "Usage: homecloud " usage "\r\n"); return 2; }
    if      (!strcmp(c, "server"))     { NEED(1, "server");     rc = cmd_server(actor, &o); }
    else if (!strcmp(c, "listdisk"))   { NEED(1, "listdisk");   rc = cmd_listdisk(actor, &o); }
    else if (!strcmp(c, "listuser"))   { NEED(1, "listuser");   rc = cmd_listuser(actor, &o); }
    else if (!strcmp(c, "adduser"))    rc = cmd_adduser(actor, argc, argv, &o);
    else if (!strcmp(c, "removeuser")) { NEED(2, "removeuser <utilisateur>"); rc = cmd_removeuser(actor, argv[1], &o); }
    else if (!strcmp(c, "permission")) rc = cmd_permission(actor, argc, argv, &o);
    else if (!strcmp(c, "adddisk"))    { NEED(3, "adddisk <nom> <taille_go>"); rc = cmd_adddisk(actor, argv[1], argv[2], &o); }
    else if (!strcmp(c, "removedisk")) { NEED(2, "removedisk <nom>"); rc = cmd_removedisk(actor, argv[1], &o); }
    else if (!strcmp(c, "linkdisk"))   { NEED(4, "linkdisk <disque> <utilisateur> <Read|Write|None>"); rc = cmd_linkdisk(actor, argv[1], argv[2], argv[3], &o); }
    else { ob_f(&o, "homecloud: commande inconnue '%s' (essayez: homecloud help)\r\n", c); rc = 2; }
#undef NEED
    return rc;
}
