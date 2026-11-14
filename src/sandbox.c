/* Sandbox for one shell session.
 *
 * What the user sees: a Linux root made of the host's /usr (read-only), a tiny generated /etc,
 * a private /tmp and /dev, a private /proc (only its own processes), and /home/<user> = their personal space.
 * Nothing else of the machine exists for them: not /etc/homecloud, not the other users' disks, not HomeCloud's data.
 *
 * Backends
 *   ns     : built-in, needs root (full edition). New mount / PID / IPC / UTS (and optionally network) namespaces,
 *            pivot_root, then every Linux capability is dropped and no_new_privs is set.
 *   bwrap  : bubblewrap (user namespaces), when installed. Same layout.
 * Network: shared with the host by default, because opening a port is the point (/port/<user>/<port>/...).
 * Resource limits: cgroup v2 (memory + processes) when available. */
#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <sched.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/capability.h>

#ifndef PR_CAPBSET_DROP
#define PR_CAPBSET_DROP 24
#endif
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

static const char *PATH_ENV = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
/* inside ns / bwrap: the identity commands (/run/hc/bin) come first */
static const char *SB_PATH = "/run/hc/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

static void die(const char *m)
{
    dprintf(2, "\r\nhomecloud-shell : bac à sable impossible (%s : %s)\r\n", m, strerror(errno));
    _exit(125);
}

static int mkdirs(const char *path, mode_t mode)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, mode); *p = '/'; }
    return mkdir(tmp, mode) == 0 || errno == EEXIST ? 0 : -1;
}

static void touch(const char *path)
{
    char d[1100];
    snprintf(d, sizeof d, "%s", path);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mkdirs(d, 0755); }
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
}

static int bindm(const char *src, const char *dst, int ro)
{
    struct stat st;
    if (stat(src, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode)) { if (mkdirs(dst, 0755) < 0) return -1; } else touch(dst);
    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) < 0) return -1;
    if (ro && mount(NULL, dst, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID, NULL) < 0) return -1;
    return 0;
}

static void put(const char *dir, const char *name, const char *content)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    touch(p);
    FILE *f = fopen(p, "w");
    if (f) { fputs(content, f); fclose(f); }
}

/* generated /etc files (identical for both backends) */
static void write_etc(const char *dir, const spawn_t *o)
{
    char b[512];
    mkdirs(dir, 0755);
    snprintf(b, sizeof b, "%s:x:0:0:%s:/home/%s:/bin/bash\nnobody:x:65534:65534:nobody:/:/usr/sbin/nologin\n", o->user, o->user, o->user);
    put(dir, "passwd", b);
    put(dir, "group", "root:x:0:\nnogroup:x:65534:\n");
    put(dir, "hosts", "127.0.0.1 localhost homecloud\n::1 localhost\n");
    put(dir, "nsswitch.conf", "passwd: files\ngroup: files\nhosts: files dns\n");
    put(dir, "hostname", "homecloud\n");
    for (const idfile_t *f = id_etc; f->name; f++) put(dir, f->name, f->body);      /* os-release, lsb-release, issue */
    FILE *in = g_cfg.network ? fopen("/etc/resolv.conf", "r") : NULL;
    if (in) {
        char path[1100], line[512];
        snprintf(path, sizeof path, "%s/resolv.conf", dir);
        FILE *out = fopen(path, "w");
        while (out && fgets(line, sizeof line, in)) fputs(line, out);
        if (out) fclose(out);
        fclose(in);
    }
}

/* commands and /proc files that describe the machine, generated into <dir>: bin/<commands>, version, cmdline */
static void write_identity(const char *dir)
{
    char bd[1100], p[1200];
    mkdirs(dir, 0755);
    snprintf(bd, sizeof bd, "%s/bin", dir);
    mkdirs(bd, 0755);
    for (const idfile_t *f = id_bin; f->name; f++) {
        put(bd, f->name, f->body);
        snprintf(p, sizeof p, "%s/%s", bd, f->name);
        chmod(p, 0755);
    }
    for (const idfile_t *f = id_proc; f->name; f++) put(dir, f->name, f->body);

    /* Interactive Bash completion for HomeCloud's built-in command. */
    const char *rc =
        "shopt -s histappend\n"
        "_hc_complete() {\n"
        "  local cur prev words\n"
        "  cur=\"${COMP_WORDS[COMP_CWORD]}\"; prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
        "  words='help info server listdisk listuser usedisk adduser removeuser permission adddisk removedisk linkdisk'\n"
        "  case \"${COMP_CWORD}\" in\n"
        "    1) COMPREPLY=( $(compgen -W \"$words\" -- \"$cur\") );;\n"
        "    *) case \"${COMP_WORDS[1]}:$prev\" in\n"
        "         permission:*) COMPREPLY=( $(compgen -W 'ServerInfo DiskList UseDisk UserManage PermissionManage DiskManage DiskLink on off' -- \"$cur\") );;\n"
        "         linkdisk:*) COMPREPLY=( $(compgen -W 'Read Write None' -- \"$cur\") );;\n"
        "         *) COMPREPLY=();;\n"
        "       esac;;\n"
        "  esac\n"
        "}\n"
        "complete -o nospace -F _hc_complete homecloud\n";
    put(dir, "bashrc", rc);
}

static void set_env(const spawn_t *o)
{
    char b[512];
    clearenv();
    setenv("PATH", SB_PATH, 1);
    snprintf(b, sizeof b, "/home/%s", o->user);
    setenv("HOME", b, 1);
    setenv("USER", o->user, 1);
    setenv("LOGNAME", o->user, 1);
    setenv("SHELL", "/bin/bash", 1);
    setenv("HOSTNAME", "homecloud", 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("TERM", o->term && *o->term ? o->term : "xterm-256color", 1);
    snprintf(b, sizeof b, "\\[\\e[1;36m\\]%s@homecloud\\[\\e[0m\\]:\\[\\e[1;34m\\]\\w\\[\\e[0m\\]\\$ ", o->user);
    setenv("PS1", b, 1);
}

static void drop_privileges(void)
{
    for (int c = 0; c <= 63; c++) prctl(PR_CAPBSET_DROP, c, 0, 0, 0);
    struct __user_cap_header_struct h = { .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0 };
    struct __user_cap_data_struct d[2];
    memset(d, 0, sizeof d);
    syscall(SYS_capset, &h, d);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct rlimit rl = { 0, 0 };
    setrlimit(RLIMIT_CORE, &rl);
}

static void close_fds(void)
{
    long mx = sysconf(_SC_OPEN_MAX);
    if (mx < 0 || mx > 8192) mx = 8192;
    for (int fd = 3; fd < mx; fd++) close(fd);
}

static void exec_shell(const spawn_t *o)
{
    const char *bash = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
    if (o->cmd && *o->cmd) execl(bash, "bash", "-c", o->cmd, (char *)NULL);
    else execl(bash, "bash", "--noprofile", "--rcfile", "/run/hc/bashrc", "-i", (char *)NULL);
    die("exec shell");
}

/* ---------------- built-in backend ---------------- */
static void ns_setup_and_exec(const spawn_t *o, const char *root)
{
    char p[1100], q[1100];
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) die("mount private");
    if (mount("tmpfs", root, "tmpfs", MS_NOSUID | MS_NODEV, "mode=755,size=32m") < 0) die("mount root");

    /* system directories, read-only (usr-merge symlinks are recreated as symlinks) */
    static const char *sys[] = { "usr", "bin", "sbin", "lib", "lib32", "lib64", "libx32" };
    for (size_t i = 0; i < sizeof sys / sizeof *sys; i++) {
        struct stat st;
        snprintf(p, sizeof p, "/%s", sys[i]);
        if (lstat(p, &st) < 0) continue;
        snprintf(q, sizeof q, "%s/%s", root, sys[i]);
        if (S_ISLNK(st.st_mode)) {
            char tgt[512];
            ssize_t n = readlink(p, tgt, sizeof tgt - 1);
            if (n > 0) { tgt[n] = 0; if (symlink(tgt, q) < 0) die("symlink"); }
        } else if (bindm(p, q, 1) < 0) die("bind system dir");
    }

    /* /etc: generated files + a few harmless read-only host files */
    snprintf(p, sizeof p, "%s/etc", root);
    write_etc(p, o);
    static const char *hostetc[] = { "ld.so.cache", "localtime", "ssl", "ca-certificates", "alternatives", "terminfo" };
    for (size_t i = 0; i < sizeof hostetc / sizeof *hostetc; i++) {
        snprintf(p, sizeof p, "/etc/%s", hostetc[i]);
        snprintf(q, sizeof q, "%s/etc/%s", root, hostetc[i]);
        bindm(p, q, 1);
    }

    /* /dev */
    snprintf(p, sizeof p, "%s/dev", root);
    mkdirs(p, 0755);
    if (mount("tmpfs", p, "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=755,size=1m") < 0) die("mount dev");
    static const char *devs[] = { "null", "zero", "full", "random", "urandom", "tty" };
    for (size_t i = 0; i < sizeof devs / sizeof *devs; i++) {
        snprintf(p, sizeof p, "/dev/%s", devs[i]);
        snprintf(q, sizeof q, "%s/dev/%s", root, devs[i]);
        bindm(p, q, 0);
    }
    snprintf(q, sizeof q, "%s/dev/shm", root); mkdirs(q, 0755);
    mount("tmpfs", q, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=1777,size=64m");
    snprintf(q, sizeof q, "%s/dev/fd", root);     symlink("/proc/self/fd", q);
    snprintf(q, sizeof q, "%s/dev/stdin", root);  symlink("/proc/self/fd/0", q);
    snprintf(q, sizeof q, "%s/dev/stdout", root); symlink("/proc/self/fd/1", q);
    snprintf(q, sizeof q, "%s/dev/stderr", root); symlink("/proc/self/fd/2", q);

    /* /proc of the new PID namespace, with the dangerous parts masked */
    snprintf(p, sizeof p, "%s/proc", root);
    mkdirs(p, 0555);
    if (mount("proc", p, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) die("mount proc");
    static const char *ro[] = { "sys", "sysrq-trigger", "irq", "bus", "fs" };
    for (size_t i = 0; i < sizeof ro / sizeof *ro; i++) {
        snprintf(p, sizeof p, "%s/proc/%s", root, ro[i]);
        bindm(p, p, 1);
    }
    static const char *mask[] = { "kcore", "keys", "timer_list", "sched_debug", "kmsg" };
    snprintf(q, sizeof q, "%s/dev/null", root);
    for (size_t i = 0; i < sizeof mask / sizeof *mask; i++) {
        snprintf(p, sizeof p, "%s/proc/%s", root, mask[i]);
        mount(q, p, NULL, MS_BIND, NULL);
    }

    /* scratch space + the personal space */
    snprintf(p, sizeof p, "%s/tmp", root); mkdirs(p, 01777);
    if (mount("tmpfs", p, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=256m") < 0) die("mount tmp");
    snprintf(p, sizeof p, "%s/run", root); mkdirs(p, 0755);
    mount("tmpfs", p, "tmpfs", MS_NOSUID | MS_NODEV, "mode=755,size=16m");
    {   /* what the shell says about the machine: commands first in PATH, /proc/version, /usr/lib/os-release */
        char hc[1100];
        snprintf(hc, sizeof hc, "%s/run/hc", root);
        write_identity(hc);
        for (const idfile_t *f = id_proc; f->name; f++) {
            snprintf(p, sizeof p, "%s/%s", hc, f->name);
            snprintf(q, sizeof q, "%s/proc/%s", root, f->name);
            bindm(p, q, 1);
        }
        if (access("/usr/lib/os-release", F_OK) == 0) {
            snprintf(p, sizeof p, "%s/etc/os-release", root);
            snprintf(q, sizeof q, "%s/usr/lib/os-release", root);
            bindm(p, q, 1);
        }
        bindm(hc, hc, 1);
    }
    snprintf(q, sizeof q, "%s/home/%s", root, o->user);
    if (bindm(o->home_src, q, 0) < 0) die("bind espace personnel");
    if (mount(NULL, q, NULL, MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV, NULL) < 0) die("remount home");

    /* switch root */
    if (chdir(root) < 0) die("chdir root");
    mkdir(".old", 0700);
    if (syscall(SYS_pivot_root, ".", ".old") < 0) die("pivot_root");
    if (chdir("/") < 0) die("chdir /");
    if (umount2("/.old", MNT_DETACH) < 0) die("umount old root");
    rmdir("/.old");
    sethostname("homecloud", 9);
    if (!g_cfg.network) {                            /* private network namespace: just bring lo up */
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "lo");
        if (s >= 0 && ioctl(s, SIOCGIFFLAGS, &ifr) == 0) { ifr.ifr_flags |= IFF_UP; ioctl(s, SIOCSIFFLAGS, &ifr); }
        if (s >= 0) close(s);
    }

    drop_privileges();
    set_env(o);
    snprintf(p, sizeof p, "/home/%s", o->user);
    if (chdir(p) < 0) chdir("/");

    /* tiny init: runs the shell, reaps orphans, leaves with the shell's status (the kernel then ends the namespace) */
    pid_t sh = fork();
    if (sh < 0) die("fork");
    if (sh == 0) exec_shell(o);
    int code = 0;
    for (;;) {
        int st;
        pid_t w = waitpid(-1, &st, 0);
        if (w < 0) { if (errno == EINTR) continue; break; }
        if (w == sh) { code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); break; }
    }
    _exit(code);
}

static void ns_enter(const spawn_t *o, const char *root)
{
    int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | (g_cfg.network ? 0 : CLONE_NEWNET);
    if (unshare(flags) < 0) die("unshare");
    pid_t init = fork();                              /* first child of the new PID namespace = pid 1 */
    if (init < 0) die("fork");
    if (init == 0) ns_setup_and_exec(o, root);
    int st;
    while (waitpid(init, &st, 0) < 0 && errno == EINTR) {}
    _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
}

/* ---------------- bubblewrap backend ---------------- */
static void bwrap_exec(const spawn_t *o, const char *root)
{
    char *argv[140], home[700], etc[700], hc[760];
    int n = 0;
    char *bw = NULL;
    const char *path = getenv("PATH");
    char *copy = strdup(path ? path : PATH_ENV), *save = NULL;
    for (char *d = strtok_r(copy, ":", &save); d && !bw; d = strtok_r(NULL, ":", &save)) {
        char full[1100];
        snprintf(full, sizeof full, "%s/bwrap", d);
        if (access(full, X_OK) == 0) bw = strdup(full);
    }
    free(copy);
    if (!bw) die("bwrap introuvable");
    snprintf(etc, sizeof etc, "%s/etc", root);
    write_etc(etc, o);
    snprintf(hc, sizeof hc, "%s/hc", root);
    write_identity(hc);
    snprintf(home, sizeof home, "/home/%s", o->user);
    const char *files[] = { "passwd", "group", "hosts", "nsswitch.conf", "resolv.conf", "os-release", "lsb-release", "issue" };
    argv[n++] = bw;
    argv[n++] = "--die-with-parent"; argv[n++] = "--unshare-pid"; argv[n++] = "--unshare-ipc"; argv[n++] = "--unshare-uts";
    if (!g_cfg.network) argv[n++] = "--unshare-net";
    argv[n++] = "--cap-drop"; argv[n++] = "ALL";
    argv[n++] = "--hostname"; argv[n++] = "homecloud";
    static const char *sys[] = { "usr", "bin", "sbin", "lib", "lib32", "lib64", "libx32" };
    static char sysp[7][16];
    for (size_t i = 0; i < 7; i++) {
        struct stat st;
        snprintf(sysp[i], sizeof sysp[i], "/%s", sys[i]);
        if (lstat(sysp[i], &st) < 0) continue;
        if (S_ISLNK(st.st_mode)) {
            static char tgt[7][128];
            ssize_t k = readlink(sysp[i], tgt[i], sizeof tgt[i] - 1);
            if (k > 0) { tgt[i][k] = 0; argv[n++] = "--symlink"; argv[n++] = tgt[i]; argv[n++] = sysp[i]; }
        } else { argv[n++] = "--ro-bind"; argv[n++] = sysp[i]; argv[n++] = sysp[i]; }
    }
    argv[n++] = "--proc"; argv[n++] = "/proc"; argv[n++] = "--dev"; argv[n++] = "/dev";
    argv[n++] = "--tmpfs"; argv[n++] = "/tmp"; argv[n++] = "--tmpfs"; argv[n++] = "/run";
    static char src[8][700], dst[8][64];
    for (int i = 0; i < 8; i++) {
        snprintf(src[i], sizeof src[i], "%s/%s", etc, files[i]);
        if (access(src[i], F_OK) != 0) continue;
        snprintf(dst[i], sizeof dst[i], "/etc/%s", files[i]);
        argv[n++] = "--ro-bind"; argv[n++] = src[i]; argv[n++] = dst[i];
    }
    static const char *hostetc[] = { "/etc/ld.so.cache", "/etc/localtime", "/etc/ssl", "/etc/alternatives" };
    for (size_t i = 0; i < sizeof hostetc / sizeof *hostetc; i++)
        if (access(hostetc[i], F_OK) == 0) { argv[n++] = "--ro-bind"; argv[n++] = (char *)hostetc[i]; argv[n++] = (char *)hostetc[i]; }
    /* what the shell says about the machine */
    argv[n++] = "--ro-bind"; argv[n++] = hc; argv[n++] = "/run/hc";
    static char pj[4][800], pd[4][64], osl[800];
    int ip = 0;
    for (const idfile_t *f = id_proc; f->name && ip < 4; f++, ip++) {
        snprintf(pj[ip], sizeof pj[ip], "%s/%s", hc, f->name);
        snprintf(pd[ip], sizeof pd[ip], "/proc/%s", f->name);
        argv[n++] = "--ro-bind"; argv[n++] = pj[ip]; argv[n++] = pd[ip];
    }
    if (access("/usr/lib/os-release", F_OK) == 0) {
        snprintf(osl, sizeof osl, "%s/os-release", etc);
        argv[n++] = "--ro-bind"; argv[n++] = osl; argv[n++] = "/usr/lib/os-release";
    }
    argv[n++] = "--bind"; argv[n++] = (char *)o->home_src; argv[n++] = home;
    argv[n++] = "--chdir"; argv[n++] = home;
    set_env(o);
    argv[n++] = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
    if (o->cmd && *o->cmd) { argv[n++] = "-c"; argv[n++] = (char *)o->cmd; }
    else { argv[n++] = "--noprofile"; argv[n++] = "--rcfile"; argv[n++] = "/run/hc/bashrc"; argv[n++] = "-i"; }
    argv[n] = NULL;
    execv(bw, argv);
    die("exec bwrap");
}

/* ---------------- cgroup v2 limits (best effort) ---------------- */
/* ---------------- proot backend (Termux, no root) ---------------- */
static char *find_cmd(const char *name)
{
    const char *path = getenv("PATH");
    char *copy = strdup(path ? path : PATH_ENV), *save = NULL, *res = NULL;
    for (char *d = strtok_r(copy, ":", &save); d && !res; d = strtok_r(NULL, ":", &save)) {
        char full[1100];
        snprintf(full, sizeof full, "%s/%s", d, name);
        if (access(full, X_OK) == 0) res = strdup(full);
    }
    free(copy);
    return res;
}

/* Termux layout: <PREFIX>/bin/proot, binaries and libraries live under PREFIX */
static void prefix_of(const char *proot, char *out, size_t sz)
{
    char real[4096];
    if (!realpath(proot, real)) snprintf(real, sizeof real, "%s", proot);
    char *sl = strrchr(real, '/');
    if (sl) *sl = 0;
    sl = strrchr(real, '/');
    if (sl) *sl = 0;
    snprintf(out, sz, "%s", real[0] ? real : "/usr");
}

static void proot_exec(const spawn_t *o, const char *root)
{
    char *argv[48], home[700], prefix[600], tmpd[700], tdst[700], bpre[700], shell[700];
    int n = 0;
    char *pr = find_cmd("proot");
    if (!pr) die("proot introuvable");
    prefix_of(pr, prefix, sizeof prefix);
    snprintf(home, sizeof home, "/home/%s", o->user);
    snprintf(tmpd, sizeof tmpd, "%s/tmp", root);
    mkdir(tmpd, 0700);
    snprintf(tdst, sizeof tdst, "%s/tmp", prefix);
    snprintf(bpre, sizeof bpre, "%s", prefix);
    snprintf(shell, sizeof shell, "%s/bin/bash", prefix);
    if (access(shell, X_OK) != 0) snprintf(shell, sizeof shell, "%s/bin/sh", prefix);
    char homebind[1500];
    snprintf(homebind, sizeof homebind, "%s:%s", o->home_src, home);
    char tmpbind[1500];
    snprintf(tmpbind, sizeof tmpbind, "%s:%s", tmpd, tdst);
    const int no_seccomp = getenv("PROOT_NO_SECCOMP") != NULL;
    char path[700];
    snprintf(path, sizeof path, "%s/bin", prefix);
    clearenv();
    setenv("PATH", path, 1);
    setenv("PREFIX", prefix, 1);
    setenv("TMPDIR", tdst, 1);
    setenv("PROOT_TMP_DIR", root, 1);
    if (no_seccomp) setenv("PROOT_NO_SECCOMP", "1", 1);
    setenv("HOME", home, 1);
    setenv("USER", o->user, 1);
    setenv("LOGNAME", o->user, 1);
    setenv("SHELL", shell, 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("TERM", o->term && *o->term ? o->term : "xterm-256color", 1);
    char ps[512];
    snprintf(ps, sizeof ps, "\\[\\e[1;36m\\]%s@homecloud\\[\\e[0m\\]:\\[\\e[1;34m\\]\\w\\[\\e[0m\\]\\$ ", o->user);
    setenv("PS1", ps, 1);
    argv[n++] = pr;
    argv[n++] = "--kill-on-exit";
    argv[n++] = "-r"; argv[n++] = (char *)root;
    argv[n++] = "-b"; argv[n++] = bpre;
    argv[n++] = "-b"; argv[n++] = tmpbind;
    argv[n++] = "-b"; argv[n++] = homebind;
    static const char *host[] = { "/system", "/apex", "/vendor", "/dev", "/proc", NULL };
    for (int i = 0; host[i]; i++)
        if (access(host[i], F_OK) == 0) { argv[n++] = "-b"; argv[n++] = (char *)host[i]; }
    argv[n++] = "-w"; argv[n++] = home;
    argv[n++] = shell;
    if (o->cmd && *o->cmd) { argv[n++] = "-c"; argv[n++] = (char *)o->cmd; }
    else { argv[n++] = "--noprofile"; argv[n++] = "--rcfile"; argv[n++] = "/run/hc/bashrc"; argv[n++] = "-i"; }
    argv[n] = NULL;
    execv(pr, argv);
    die("exec proot");
}

/* ---------------- "simple" backend: no isolation (explicit opt-in only) ---------------- */
static void simple_exec(const spawn_t *o)
{
    const char *path = getenv("PATH"), *pre = getenv("PREFIX");
    char p[1200], home[700], sh[300];
    snprintf(p, sizeof p, "%s", path ? path : PATH_ENV);
    snprintf(home, sizeof home, "%s", o->home_src);
    char *bash = find_cmd("bash");
    snprintf(sh, sizeof sh, "%s", bash ? bash : "/bin/sh");
    clearenv();
    setenv("PATH", p, 1);
    if (pre) setenv("PREFIX", pre, 1);
    setenv("HOME", home, 1);
    setenv("USER", o->user, 1);
    setenv("LOGNAME", o->user, 1);
    setenv("SHELL", sh, 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("TERM", o->term && *o->term ? o->term : "xterm-256color", 1);
    char ps[512];
    snprintf(ps, sizeof ps, "\\[\\e[1;36m\\]%s@homecloud\\[\\e[0m\\]:\\[\\e[1;34m\\]\\w\\[\\e[0m\\]\\$ ", o->user);
    setenv("PS1", ps, 1);
    if (chdir(home) != 0) die("chdir");
    if (o->cmd && *o->cmd) execl(sh, "bash", "-c", o->cmd, (char *)NULL);
    else execl(sh, "bash", "--norc", "--noprofile", "-i", (char *)NULL);
    die("exec shell");
}

static void wfile(const char *p, const char *v)
{
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, v, strlen(v)) < 0) { /* best effort */ }
    close(fd);
}

static void cg_path(const char *root, char *out, size_t sz)
{
    const char *b = strrchr(root, '/');
    snprintf(out, sz, "/sys/fs/cgroup/hcshell/%s", b ? b + 1 : root);
}

static void cg_join(const char *root, pid_t pid)
{
    if (access("/sys/fs/cgroup/cgroup.controllers", F_OK) != 0 || geteuid() != 0) return;
    char d[300], f[360], v[32];
    wfile("/sys/fs/cgroup/cgroup.subtree_control", "+memory +pids");
    mkdir("/sys/fs/cgroup/hcshell", 0755);
    wfile("/sys/fs/cgroup/hcshell/cgroup.subtree_control", "+memory +pids");
    cg_path(root, d, sizeof d);
    if (mkdir(d, 0755) < 0) return;
    if (g_cfg.mem_mb > 0) { snprintf(f, sizeof f, "%s/memory.max", d); snprintf(v, sizeof v, "%lld", (long long)g_cfg.mem_mb << 20); wfile(f, v);
                            snprintf(f, sizeof f, "%s/memory.swap.max", d); wfile(f, "0"); }
    if (g_cfg.pids > 0)   { snprintf(f, sizeof f, "%s/pids.max", d); snprintf(v, sizeof v, "%d", g_cfg.pids); wfile(f, v); }
    snprintf(f, sizeof f, "%s/cgroup.procs", d);
    snprintf(v, sizeof v, "%d", (int)pid);
    wfile(f, v);
}

/* ---------------- public API ---------------- */
const char *sandbox_detect(void)
{
    if (geteuid() == 0) {
        /* does the namespace machinery work here? (containers sometimes forbid it) */
        pid_t p = fork();
        if (p == 0) {
            int fl = CLONE_NEWNS | CLONE_NEWPID;
            _exit(unshare(fl) == 0 ? 0 : 1);
        }
        int st = 1;
        if (p > 0) waitpid(p, &st, 0);
        if (p > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) return "ns";
    }
    if (command_exists("bwrap")) return "bwrap";
    if (command_exists("proot")) return "proot";
    return NULL;
}

/* writable temp directory: /run (Linux), $TMPDIR (Termux), /tmp */
const char *tmp_base(void)
{
    static char b[200];
    const char *c[] = { "/run", getenv("TMPDIR"), "/tmp", NULL };
    for (int i = 0; c[i]; i++)
        if (*c[i] && strlen(c[i]) < 120 && access(c[i], W_OK) == 0) { snprintf(b, sizeof b, "%s", c[i]); return b; }
    return "/tmp";
}

int sandbox_spawn(const spawn_t *o, int *master, pid_t *pid, char *root_out, size_t rsz)
{
    char root[64];
    snprintf(root, sizeof root, "%s/hcshell-XXXXXX", tmp_base());
    if (!mkdtemp(root)) return -1;
    snprintf(root_out, rsz, "%s", root);
    int gate[2];
    if (pipe(gate) < 0) { rmdir(root); return -1; }
    struct winsize ws = { .ws_row = (unsigned short)(o->rows > 0 ? o->rows : 24), .ws_col = (unsigned short)(o->cols > 0 ? o->cols : 80) };
    pid_t p = forkpty(master, NULL, NULL, &ws);
    if (p < 0) { close(gate[0]); close(gate[1]); rmdir(root); return -1; }
    if (p == 0) {
        char c;
        if (read(gate[0], &c, 1) != 1) _exit(125);   /* wait until the parent has set the cgroup up */
        close_fds();
        signal(SIGPIPE, SIG_DFL);
        if (!strcmp(g_cfg.backend, "bwrap")) bwrap_exec(o, root);
        if (!strcmp(g_cfg.backend, "proot")) proot_exec(o, root);
        if (!strcmp(g_cfg.backend, "simple")) simple_exec(o);
        ns_enter(o, root);
        _exit(125);
    }
    cg_join(root, p);
    if (write(gate[1], "g", 1) < 0) { /* child will exit */ }
    close(gate[0]); close(gate[1]);
    *pid = p;
    return 0;
}

/* SIGKILL the whole process tree below pid (and pid itself) */
void sandbox_kill(pid_t pid)
{
    pid_t list[4096];
    int n = 0;
    list[n++] = pid;
    for (int round = 0; round < 8; round++) {          /* a few passes catch freshly forked children */
        DIR *d = opendir("/proc");
        struct dirent *e;
        if (!d) break;
        while ((e = readdir(d))) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            char f[64], buf[512];
            snprintf(f, sizeof f, "/proc/%s/stat", e->d_name);
            FILE *fp = fopen(f, "r");
            if (!fp) continue;
            size_t k = fread(buf, 1, sizeof buf - 1, fp);
            fclose(fp);
            buf[k] = 0;
            char *rp = strrchr(buf, ')');
            int pp = 0;
            if (!rp || sscanf(rp + 2, "%*c %d", &pp) != 1) continue;
            pid_t me = (pid_t)atoi(e->d_name);
            for (int i = 0; i < n; i++)
                if (list[i] == pp) {
                    int known = 0;
                    for (int j = 0; j < n; j++) if (list[j] == me) known = 1;
                    if (!known && n < 4096) list[n++] = me;
                    break;
                }
        }
        closedir(d);
        for (int i = n - 1; i >= 0; i--) kill(list[i], SIGKILL);
    }
}

void sandbox_cleanup(const char *root)
{
    char d[300];
    if (!root || !*root) return;
    cg_path(root, d, sizeof d);
    rmdir(d);
    /* bwrap backend keeps generated /etc files in the temp directory */
    char etc[400];
    snprintf(etc, sizeof etc, "%s/etc", root);
    static const char *files[] = { "passwd", "group", "hosts", "nsswitch.conf", "hostname", "resolv.conf", "os-release", "lsb-release", "issue" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) { char f[460]; snprintf(f, sizeof f, "%s/%s", etc, files[i]); unlink(f); }
    rmdir(etc);
    {   /* bwrap backend: generated identity commands and /proc files */
        char hc[400], f[460];
        snprintf(hc, sizeof hc, "%s/hc", root);
        for (const idfile_t *x = id_bin; x->name; x++) { snprintf(f, sizeof f, "%s/bin/%s", hc, x->name); unlink(f); }
        snprintf(f, sizeof f, "%s/bin", hc); rmdir(f);
        for (const idfile_t *x = id_proc; x->name; x++) { snprintf(f, sizeof f, "%s/%s", hc, x->name); unlink(f); }
        rmdir(hc);
    }
    snprintf(etc, sizeof etc, "%s/tmp", root);
    rmdir(etc);
    rmdir(root);
}
