#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

void logmsg(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    flockfile(stderr);
    fprintf(stderr, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    funlockfile(stderr);
}

int mkdir_p(const char *path, mode_t mode)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

int run_cmd(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int command_exists(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = strdup(path), *save = NULL;
    int found = 0;
    for (char *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", d, name);
        if (access(full, X_OK) == 0) found = 1;
    }
    free(copy);
    return found;
}

int file_exists(const char *path) { return access(path, F_OK) == 0; }

