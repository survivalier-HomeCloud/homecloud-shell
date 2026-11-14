/* HomeCloud Shell - shared declarations (server + client) */
#ifndef HS_H
#define HS_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <pthread.h>

#define HS_VERSION      "1.1.0"
#define HS_DEFAULT_WEB  8680        /* HomeCloud Web: the shell is reached through its port */
#define HC_DEFAULT_PORT 8650
enum { HC_NONE = 0, HC_FULL = 1, HC_LITE = 2 };

/* ---------- util.c ---------- */
void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  mkdir_p(const char *path, mode_t mode);
int  run_cmd(char *const argv[]);
int  command_exists(const char *name);
int  file_exists(const char *path);

/* ---------- ui.c ---------- */
void ui_banner(void);
void ui_title(const char *t);
void ui_ok(const char *fmt, ...)   __attribute__((format(printf, 1, 2)));
void ui_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_err(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
void ui_kv(const char *k, const char *v);
void ui_ask(const char *label, const char *def, char *out, size_t sz);
void ui_ask_secret(const char *label, char *out, size_t sz);
int  ui_confirm(const char *label, int def);
int  ui_wait(const char *label, int (*check)(void *), void *arg, int timeout_ms);
int  ui_interactive(void);

/* ---------- proto.c: framed protocol  [type:1][len:4 BE][payload] ---------- */
enum {
    F_AUTH = 'A',   /* C->S  user\0pass\0cmd\0rows\0cols\0term */
    F_DATA = 'D',   /* both  terminal bytes */
    F_WIN  = 'W',   /* C->S  rows(2) cols(2) big endian */
    F_INFO   = 'K',   /* S->C  info text */
    F_ERR  = 'E',   /* S->C  error text */
    F_EXIT = 'Q',   /* S->C  exit status (int32 BE) */
    F_PEER = 'I',   /* web->S  client address text */
    F_PORT = 'P',   /* web->S  "user\0port": does this user's sandbox listen on it? */
    F_OPEN = 'O'    /* S->C  a service just started listening in the shell: port as decimal text */
};
#define FRAME_MAX 65536
int  io_all(int fd, const void *buf, size_t n);                       /* write all */
int  io_full(int fd, void *buf, size_t n);                            /* read exactly n */
int  frame_send(int fd, int type, const void *p, uint32_t n);
int  frame_recv(int fd, int *type, unsigned char *buf, uint32_t cap, uint32_t *n);   /* 0 ok, -1 closed/error */

/* ---------- config.c ---------- */
typedef struct {
    int  kind;                 /* HC_FULL / HC_LITE */
    char hc_conf[512];
    char data_dir[512];        /* HomeCloud's data directory */
    int  upstream_port;
    int  network;              /* sandbox may use the network (needed to open ports) */
    int  mem_mb;               /* cgroup memory limit, 0 = none */
    int  pids;                 /* cgroup pids limit, 0 = none */
    int  autostart;
    char backend[16];          /* "ns" | "bwrap" */
} scfg_t;
extern scfg_t g_cfg;
extern char   g_dir[512];

int  hc_find(void);            /* installed HomeCloud */
int  scfg_find(void);          /* existing shell.conf */
int  scfg_load(void);
int  scfg_save(void);
void sock_path(char *out, size_t sz);
int  personal_dir(const char *disk_id, char *out, size_t sz);

/* ---------- auth.c ---------- */
/* Verifies the HomeCloud account and returns the personal space directory. 0 = ok. */
int  hc_auth(const char *user, const char *pass, char *home, size_t hsz, int *admin, char *err, size_t esz);
int  hc_active(void);
int  hc_usedisk(const char *admin_user, const char *admin_pass, const char *disk_name, char *disk_user, size_t usz, char *disk_home, size_t hsz, char *err, size_t esz);

/* ---------- auth.c : pont d'administration (/api/shell) ---------- */
int   hc_bridge_key(char *out, size_t sz);
int   hc_bridge_key_ensure(void);                       /* crée /etc/homecloud/shell-bridge.key si besoin */
char *hc_bridge(const char *method, const char *route, const char *params, int *status);
void  hc_urlenc(char *out, size_t sz, const char *in);
int   hc_jstr(const char *j, const char *key, char *out, size_t sz);

/* ---------- admin.c : commandes « homecloud server|listdisk|adduser... » ---------- */
/* Exécute la commande pour le compte `actor` ; écrit le texte à afficher (fins de ligne \r\n). Retourne 0 si réussie. */
int   hc_admin_command(const char *actor, int argc, char **argv, char *out, size_t osz);

/* ---------- identity.c: what the shell says about the machine ---------- */
typedef struct { const char *name, *body; } idfile_t;
extern const idfile_t id_bin[];    /* commands (uname, hostname, whoami...) put first in PATH */
extern const idfile_t id_etc[];    /* generated /etc files: os-release, lsb-release, issue */
extern const idfile_t id_proc[];   /* bound over /proc/version and /proc/cmdline; lists end with { NULL, NULL } */

/* ---------- sandbox.c ---------- */
typedef struct {
    const char *user, *home_src, *cmd, *term;
    int rows, cols;
} spawn_t;
const char *sandbox_detect(void);                          /* "ns", "bwrap", "proot" or NULL */
const char *tmp_base(void);                                /* writable temp dir (/run, $TMPDIR, /tmp) */
int  sandbox_spawn(const spawn_t *o, int *master, pid_t *pid, char *root_out, size_t rsz);
void sandbox_kill(pid_t pid);
void sandbox_cleanup(const char *root);

/* ---------- ports.c ---------- */
void sess_add(const char *user, pid_t pid);
void sess_del(pid_t pid);
int  port_owned(const char *user, int port);
int  sess_count(const char *user);   /* user == NULL: all */
#define WATCH_MAX 256
typedef struct { unsigned long ino[WATCH_MAX]; int port[WATCH_MAX]; char ours[WATCH_MAX]; int n; } lwatch_t;
/* Call about once a second: fills ports[] with the ports a process of the session (root and its descendants) started listening on since the last call. */
int  sess_watch(pid_t root, lwatch_t *w, int *ports, int max);

/* ---------- server.c ---------- */
int  shell_server_run(void);
void shell_server_stop(void);
#endif
