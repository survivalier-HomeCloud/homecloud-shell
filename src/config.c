/* Locating HomeCloud and the shell configuration (shell.conf, next to homecloud.conf). */
#include "shell.h"
#include <unistd.h>
#include <sys/stat.h>

scfg_t g_cfg;
char   g_dir[512];
#define FULL_DIR "/etc/homecloud"

static void defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.kind = HC_LITE;
    g_cfg.upstream_port = HC_DEFAULT_PORT;
    g_cfg.network = 1;
    g_cfg.mem_mb = 1024;
    g_cfg.pids = 512;
    snprintf(g_cfg.backend, sizeof g_cfg.backend, "ns");
}

static int candidates(char out[3][520])
{
    int n = 0;
    const char *env = getenv("HC_HOME"), *home = getenv("HOME");
    if (env && *env) snprintf(out[n++], 520, "%.500s", env);
    snprintf(out[n++], 520, "%s", FULL_DIR);
    if (home && *home) snprintf(out[n++], 520, "%.500s/.homecloud", home);
    return n;
}

static int has(const char *dir, const char *name)
{
    char p[700];
    snprintf(p, sizeof p, "%.600s/%s", dir, name);
    return access(p, F_OK) == 0;
}

/* reads key=value lines of homecloud.conf */
static void read_hc_conf(void)
{
    FILE *f = fopen(g_cfg.hc_conf, "r");
    if (!f) return;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!strncmp(line, "port=", 5)) { int p = atoi(line + 5); if (p > 0 && p < 65536) g_cfg.upstream_port = p; }
        else if (!strncmp(line, "data_dir=", 9)) snprintf(g_cfg.data_dir, sizeof g_cfg.data_dir, "%s", line + 9);
    }
    fclose(f);
}

int hc_find(void)
{
    char c[3][520];
    int n = candidates(c);
    defaults();
    for (int i = 0; i < n; i++) {
        if (!has(c[i], "homecloud.conf")) continue;
        snprintf(g_dir, sizeof g_dir, "%.510s", c[i]);
        g_cfg.kind = strcmp(c[i], FULL_DIR) ? HC_LITE : HC_FULL;
        snprintf(g_cfg.hc_conf, sizeof g_cfg.hc_conf, "%.480s/homecloud.conf", c[i]);
        read_hc_conf();
        return 0;
    }
    return -1;
}

int scfg_find(void)
{
    char c[3][520];
    int n = candidates(c);
    for (int i = 0; i < n; i++)
        if (has(c[i], "shell.conf")) { snprintf(g_dir, sizeof g_dir, "%.510s", c[i]); return 0; }
    return -1;
}

int scfg_load(void)
{
    char path[700];
    defaults();
    snprintf(path, sizeof path, "%s/shell.conf", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if      (!strcmp(k, "kind"))      g_cfg.kind = !strcmp(v, "full") ? HC_FULL : HC_LITE;
        else if (!strcmp(k, "hc_conf"))   snprintf(g_cfg.hc_conf, sizeof g_cfg.hc_conf, "%s", v);
        else if (!strcmp(k, "network"))   g_cfg.network = atoi(v);
        else if (!strcmp(k, "mem_mb"))    g_cfg.mem_mb = atoi(v);
        else if (!strcmp(k, "pids"))      g_cfg.pids = atoi(v);
        else if (!strcmp(k, "autostart")) g_cfg.autostart = atoi(v);
        else if (!strcmp(k, "backend"))   snprintf(g_cfg.backend, sizeof g_cfg.backend, "%.15s", v);
    }
    fclose(f);
    read_hc_conf();          /* ports and data directory always come from HomeCloud itself */
    return 0;
}

int scfg_save(void)
{
    char path[700];
    mkdir_p(g_dir, 0700);
    snprintf(path, sizeof path, "%s/shell.conf", g_dir);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# HomeCloud Shell configuration\nkind=%s\nhc_conf=%s\nbackend=%s\nnetwork=%d\nmem_mb=%d\npids=%d\nautostart=%d\n",
            g_cfg.kind == HC_FULL ? "full" : "lite", g_cfg.hc_conf, g_cfg.backend, g_cfg.network, g_cfg.mem_mb, g_cfg.pids, g_cfg.autostart);
    fclose(f);
    chmod(path, 0600);
    return 0;
}

void sock_path(char *out, size_t sz) { snprintf(out, sz, "%s/shell.sock", g_dir); }

/* directory holding a disk's files: lite = <data>/disks/<id>, full = <data>/mnt/<id> (mounted LUKS volume) */
int personal_dir(const char *id, char *out, size_t sz)
{
    for (const char *p = id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return -1;
    if (!*id) return -1;
    snprintf(out, sz, "%.480s/%s/%s", g_cfg.data_dir, g_cfg.kind == HC_FULL ? "mnt" : "disks", id);
    return 0;
}
