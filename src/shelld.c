/* homecloud-shelld : installer + service management + the daemon itself. */
#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>

#define UNIT_NAME   "homecloud-shell.service"
#define UNIT_PATH   "/etc/systemd/system/" UNIT_NAME
#define DROPIN_DIR  "/etc/systemd/system/homecloud.service.d"
#define DROPIN_FILE DROPIN_DIR "/homecloud-shell.conf"

/* ---------------- service plumbing ---------------- */
static int use_systemd(void) { return geteuid() == 0 && command_exists("systemctl") && access("/run/systemd/system", F_OK) == 0; }
static int unit_installed(void) { return use_systemd() && file_exists(UNIT_PATH); }
static void systemctl(const char *a, const char *b) { char *argv[] = { "systemctl", (char *)a, (char *)b, NULL }; run_cmd(argv); }
static void pid_path(char *o, size_t n) { snprintf(o, n, "%s/shell.pid", g_dir); }
static void log_path(char *o, size_t n) { snprintf(o, n, "%s/shell.log", g_dir); }

static pid_t daemon_pid(void)
{
    char p[700], cl[64], buf[256] = "";
    pid_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    int pid = 0;
    if (!f) return 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 0 || kill(pid, 0) != 0) return 0;
    snprintf(cl, sizeof cl, "/proc/%d/cmdline", pid);
    FILE *c = fopen(cl, "r");
    if (c) { size_t n = fread(buf, 1, sizeof buf - 1, c); buf[n] = 0; fclose(c); if (n && !strstr(buf, "homecloud-shelld")) return 0; }
    return pid;
}

static int is_running(void)
{
    if (unit_installed()) { char *a[] = { "systemctl", "is-active", "--quiet", UNIT_NAME, NULL }; return run_cmd(a) == 0; }
    return daemon_pid() != 0;
}

static void on_sig(int s) { (void)s; shell_server_stop(); }

static int run_core(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    return shell_server_run();
}

static int svc_start(int quiet)
{
    if (unit_installed()) { systemctl("restart", UNIT_NAME); if (!quiet) ui_ok("Service %s démarré", UNIT_NAME); return 0; }
    if (daemon_pid()) { if (!quiet) ui_warn("HomeCloud Shell tourne déjà."); return 0; }
    char lp[700], pp[700];
    log_path(lp, sizeof lp);
    pid_path(pp, sizeof pp);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDONLY), lg = open(lp, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (dn >= 0) dup2(dn, 0);
        if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); }
        FILE *f = fopen(pp, "w");
        if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
        run_core();
        unlink(pp);
        _exit(0);
    }
    usleep(500000);
    int st;
    if (waitpid(pid, &st, WNOHANG) == pid) { ui_err("HomeCloud Shell s'est arrêté au démarrage. Journal : %s", lp); return -1; }
    if (!quiet) ui_ok("HomeCloud Shell démarré en arrière-plan (pid %d)", (int)pid);
    return 0;
}

static int svc_stop(int quiet)
{
    if (unit_installed()) { systemctl("stop", UNIT_NAME); if (!quiet) ui_ok("HomeCloud Shell arrêté."); return 0; }
    pid_t pid = daemon_pid();
    if (!pid) { if (!quiet) ui_warn("HomeCloud Shell n'est pas démarré."); return 0; }
    kill(pid, SIGTERM);
    for (int i = 0; i < 50 && kill(pid, 0) == 0; i++) usleep(100000);
    if (kill(pid, 0) == 0) kill(pid, SIGKILL);
    char pp[700];
    pid_path(pp, sizeof pp);
    unlink(pp);
    if (!quiet) ui_ok("HomeCloud Shell arrêté.");
    return 0;
}

static int install_unit(int autostart)
{
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return -1;
    exe[n] = 0;
    FILE *f = fopen(UNIT_PATH, "w");
    if (!f) return -1;
    fprintf(f, "[Unit]\nDescription=HomeCloud Shell\nAfter=network-online.target homecloud.service\n\n"
               "[Service]\nType=simple\nExecStart=%s run\nRestart=on-failure\nRestartSec=3\nKillMode=process\n\n"
               "[Install]\nWantedBy=multi-user.target\n", exe);
    fclose(f);
    if (autostart) {
        mkdir_p(DROPIN_DIR, 0755);
        f = fopen(DROPIN_FILE, "w");
        if (!f) return -1;
        fprintf(f, "# HomeCloud Shell : démarre avec HomeCloud\n[Unit]\nWants=" UNIT_NAME "\n");
        fclose(f);
    } else { unlink(DROPIN_FILE); rmdir(DROPIN_DIR); }
    systemctl("daemon-reload", NULL);
    return 0;
}

/* ---------------- helpers ---------------- */
static int web_port(void)
{
    char p[700], line[256];
    snprintf(p, sizeof p, "%s/web.conf", g_dir);
    FILE *f = fopen(p, "r");
    int port = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "port=", 5)) port = atoi(line + 5);
    fclose(f);
    return port;
}

static int sock_answers(void *arg)
{
    (void)arg;
    char path[700];
    struct sockaddr_un a;
    sock_path(path, sizeof path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int ok = s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    if (s >= 0) close(s);
    return ok;
}

/* runs a harmless command in a real sandbox and checks the result */
static int probe_sandbox(void)
{
    char home[200], root[64], out[512] = "";
    snprintf(home, sizeof home, "%s/hcshell-probe-XXXXXX", tmp_base());
    if (!mkdtemp(home)) return 0;
    spawn_t sp = { .user = "probe", .home_src = home, .cmd = "echo probe-ok; ls /home", .term = "dumb", .rows = 24, .cols = 80 };
    int master;
    pid_t pid;
    int ok = 0;
    if (sandbox_spawn(&sp, &master, &pid, root, sizeof root) == 0) {
        size_t len = 0;
        struct pollfd p = { .fd = master, .events = POLLIN };
        while (len < sizeof out - 1 && poll(&p, 1, 3000) > 0) {
            ssize_t r = read(master, out + len, sizeof out - 1 - len);
            if (r <= 0) break;
            len += (size_t)r;
        }
        out[len] = 0;
        close(master);
        sandbox_kill(pid);
        waitpid(pid, NULL, 0);
        sandbox_cleanup(root);
        ok = strstr(out, "probe-ok") != NULL && strstr(out, "probe") != NULL;
    }
    rmdir(home);
    return ok;
}

static int print_addresses(int port)
{
    struct ifaddrs *ifa, *p;
    int shown = 0;
    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK)) continue;
            char ip[INET_ADDRSTRLEN], line[160];
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, ip, sizeof ip);
            snprintf(line, sizeof line, "%s:%d", ip, port);
            ui_kv(p->ifa_name, line);
            shown++;
        }
        freeifaddrs(ifa);
    }
    return shown;
}

/* ---------------- commands ---------------- */
static int load_or_die(void)
{
    if (scfg_find() < 0 || scfg_load() < 0) { fprintf(stderr, "HomeCloud Shell n'est pas installé. Lancez : homecloud-shelld install\n"); return -1; }
    return 0;
}

static void cmd_install(int argc, char **argv)
{
    int net_arg = -1, auto_arg = -1, quiet = 0, insecure = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) quiet = 1;
        else if (!strcmp(argv[i], "--insecure")) insecure = 1;
        else if (!strcmp(argv[i], "--network")) net_arg = 1;
        else if (!strcmp(argv[i], "--no-network")) net_arg = 0;
        else if (!strcmp(argv[i], "--autostart")) auto_arg = 1;
        else if (!strcmp(argv[i], "--no-autostart")) auto_arg = 0;
    }
    ui_banner();
    ui_title("Détection de HomeCloud");
    if (hc_find() < 0) {
        ui_err("HomeCloud n'est pas installé sur cette machine.");
        exit(1);
    }
    ui_ok("HomeCloud trouvé : %s", g_cfg.kind == HC_FULL ? "édition complète" : "édition lite");
    ui_kv("Configuration", g_cfg.hc_conf);
    if (g_cfg.kind == HC_FULL && geteuid() != 0) { ui_err("Les droits administrateur sont nécessaires."); ui_info("Relancez avec : sudo homecloud-shelld install"); exit(1); }
    if (!g_cfg.data_dir[0]) { ui_err("Dossier de données de HomeCloud introuvable dans sa configuration."); exit(1); }
    if (hc_active()) ui_ok("HomeCloud est actif"); else ui_warn("HomeCloud n'est pas actif : les connexions échoueront tant qu'il est arrêté.");
    int wp = web_port();
    if (wp) { char b[32]; snprintf(b, sizeof b, "%d", wp); ui_ok("HomeCloud Web détecté (port %s) : le client passera par lui", b); }
    else ui_warn("HomeCloud Web n'est pas installé : le client et les routes /port/ passent par lui (homecloud-web install).");

    ui_title("Bac à sable");
    const char *be = sandbox_detect();
    if (!be && command_exists("pkg") && !insecure) {
        ui_info("Sur Termux, le bac à sable sans root utilise « proot ».");
        if (quiet || ui_confirm("Installer proot maintenant (pkg install proot) ?", 1)) {
            char *a[] = { "pkg", "install", "-y", "proot", NULL };
            run_cmd(a);
            be = sandbox_detect();
        }
    }
    if (!be && !insecure) {
        ui_err("Aucun bac à sable disponible.");
        ui_info("Installez proot (Termux : pkg install proot), bubblewrap (apt install bubblewrap) ou installez en root.");
        ui_info("Dernier recours, SANS isolation : homecloud-shelld install --insecure");
        exit(1);
    }
    if (!be && insecure) {
        ui_warn("Mode SANS isolation : les utilisateurs du shell pourront lire toutes les données de HomeCloud");
        ui_warn("et agir avec les droits du compte qui fait tourner HomeCloud. À réserver à un compte unique de confiance.");
        if (!quiet && !ui_confirm("Continuer sans bac à sable ?", 0)) exit(1);
        be = "simple";
    }
    snprintf(g_cfg.backend, sizeof g_cfg.backend, "%s", be);
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s", !strcmp(be, "ns") ? "intégré (espaces de noms Linux, sans capacités)" : !strcmp(be, "proot") ? "proot (sans root)" : !strcmp(be, "simple") ? "AUCUN (non isolé)" : "bubblewrap");
    ui_ok("Moteur : %s", tmp);
    if (!probe_sandbox()) { ui_err("Le test du bac à sable a échoué (conteneur restreint ?). Rien n'est installé."); if (!strcmp(be, "proot")) ui_info("Essayez : export PROOT_NO_SECCOMP=1 puis relancez l'installation."); exit(1); }
    ui_ok("Test réussi : un programme s'exécute dans l'espace personnel");

    ui_title("Options");
    if (net_arg >= 0) g_cfg.network = net_arg;
    else if (quiet) g_cfg.network = 1;
    else g_cfg.network = ui_confirm("Autoriser le réseau dans le bac à sable (internet + ouverture de ports) ?", 1);
    int can_auto = use_systemd();
    if (!can_auto) { g_cfg.autostart = 0; ui_info("systemd est introuvable : démarrez avec « homecloud-shelld start »."); }
    else if (auto_arg >= 0) g_cfg.autostart = auto_arg;
    else if (quiet) g_cfg.autostart = 1;
    else g_cfg.autostart = ui_confirm("Démarrer HomeCloud Shell automatiquement quand HomeCloud démarre ?", 1);

    ui_title("Installation");
    if (is_running()) svc_stop(1);
    if (scfg_save() < 0) { ui_err("Écriture de la configuration impossible."); exit(1); }
    ui_ok("Configuration enregistrée");
    if (hc_bridge_key_ensure() == 0) ui_ok("Clé du relais d'administration prête (/etc/homecloud/shell-bridge.key)");
    else ui_warn("Clé du relais impossible à créer : les commandes d'administration (homecloud server, adduser...) seront indisponibles.");
    if (use_systemd()) { if (install_unit(g_cfg.autostart) < 0) { ui_err("Installation du service impossible."); exit(1); } ui_ok("Service homecloud-shell installé"); if (g_cfg.autostart) ui_ok("Il démarrera avec homecloud.service"); }
    if (svc_start(1) < 0) exit(1);
    if (!ui_wait("HomeCloud Shell est prêt", sock_answers, NULL, 6000)) { ui_err("Le service ne répond pas (journal : %s).", use_systemd() ? "journalctl -u homecloud-shell" : "shell.log"); exit(1); }

    ui_title("Terminé");
    if (wp) {
        printf("  Connectez-vous depuis n'importe quel ordinateur avec le client :\n\n");
        print_addresses(wp);
        printf("\n  \033[1mhomecloud-shell\033[0m <utilisateur>@<adresse>:%d\n\n", wp);
        ui_info("Un service lancé dans le shell sur le port N est visible sur http://<adresse>:%d/port/<utilisateur>/N/", wp);
    } else ui_info("Installez HomeCloud Web pour pouvoir vous connecter.");
    ui_warn("Les fichiers verrouillés ne sont pas protégés dans le shell (c'est votre espace, après connexion).");
    printf("\n");
}

static void cmd_status(void)
{
    if (load_or_die() < 0) exit(1);
    char b[200];
    ui_title("État de HomeCloud Shell");
    ui_kv("Service", is_running() ? "en marche" : "arrêté");
    ui_kv("Bac à sable", g_cfg.backend);
    ui_kv("Réseau dans le bac", g_cfg.network ? "oui" : "non");
    snprintf(b, sizeof b, "%d Mo, %d processus", g_cfg.mem_mb, g_cfg.pids);
    ui_kv("Limites par session", b);
    ui_kv("HomeCloud", hc_active() ? "actif" : "inactif");
    ui_kv("Démarre avec HomeCloud", file_exists(DROPIN_FILE) ? "oui" : "non");
    printf("\n");
}

static void cmd_uninstall(void)
{
    ui_banner();
    if (load_or_die() < 0) exit(1);
    if (ui_interactive() && !ui_confirm("Désinstaller HomeCloud Shell (HomeCloud n'est pas touché) ?", 0)) exit(0);
    svc_stop(1);
    if (use_systemd()) { systemctl("disable", UNIT_NAME); unlink(UNIT_PATH); unlink(DROPIN_FILE); rmdir(DROPIN_DIR); systemctl("daemon-reload", NULL); }
    char p[700];
    snprintf(p, sizeof p, "%s/shell.conf", g_dir);
    unlink(p);
    ui_ok("HomeCloud Shell a été désinstallé.");
}

static void usage(void)
{
    ui_banner();
    printf("  Usage : homecloud-shelld <commande>\n\n");
    ui_kv("install", "assistant d'installation");
    ui_kv("start | stop | restart", "gère le service");
    ui_kv("status", "état du service");
    ui_kv("run", "premier plan (utilisé par systemd)");
    ui_kv("uninstall", "retire HomeCloud Shell");
    printf("\n  Options d'install : --yes  --network|--no-network  --autostart|--no-autostart  --insecure\n\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 0; }
    if      (!strcmp(argv[1], "install"))   cmd_install(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "run"))       { if (load_or_die() < 0) return 1; return run_core() == 0 ? 0 : 1; }
    else if (!strcmp(argv[1], "status"))    cmd_status();
    else if (!strcmp(argv[1], "uninstall")) cmd_uninstall();
    else if (!strcmp(argv[1], "start"))     { if (load_or_die() < 0) return 1; return svc_start(0) == 0 ? 0 : 1; }
    else if (!strcmp(argv[1], "stop"))      { if (load_or_die() < 0) return 1; return svc_stop(0); }
    else if (!strcmp(argv[1], "restart"))   { if (load_or_die() < 0) return 1; svc_stop(1); return svc_start(0) == 0 ? 0 : 1; }
    else if (!strcmp(argv[1], "version"))   printf("homecloud-shelld %s\n", HS_VERSION);
    else { usage(); return 1; }
    return 0;
}
