/* Styled terminal output (same look as the HomeCloud server installer). */
#include "shell.h"
#include <unistd.h>
#include <ctype.h>
#include <poll.h>
#include <termios.h>
#include <sys/time.h>

static int use_color(void)
{
    static int c = -1;
    if (c < 0) c = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
    return c;
}

int ui_interactive(void) { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

#define C(code) (use_color() ? code : "")
#define RESET  C("\033[0m")
#define BOLD   C("\033[1m")
#define CYAN   C("\033[38;5;45m")
#define BLUE   C("\033[38;5;33m")
#define GREEN  C("\033[38;5;42m")
#define YELLOW C("\033[38;5;214m")
#define RED    C("\033[38;5;203m")
#define GREY   C("\033[38;5;245m")

void ui_banner(void)
{
    printf("\n");
    printf("%s    ╭─────────╮%s\n", CYAN, RESET);
    printf("%s    │  ❯_ ☁   │%s   %s%sHome Cloud Shell%s  %sclient %s%s\n", CYAN, RESET, BOLD, BLUE, RESET, GREY, HS_VERSION, RESET);
    printf("%s    ╰─────────╯%s   %sun terminal dans votre espace personnel%s\n\n", CYAN, RESET, GREY, RESET);
}

void ui_title(const char *t)
{
    printf("\n%s%s▍ %s%s\n", BOLD, BLUE, t, RESET);
    printf("%s  ", GREY);
    for (size_t i = 0; i < 56; i++) printf("─");
    printf("%s\n", RESET);
}

static void line(const char *icon, const char *color, const char *fmt, va_list ap)
{
    printf("  %s%s%s ", color, icon, RESET);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
}

void ui_ok(const char *fmt, ...)   { va_list ap; va_start(ap, fmt); line("✔", GREEN, fmt, ap); va_end(ap); }
void ui_info(const char *fmt, ...) { va_list ap; va_start(ap, fmt); line("●", CYAN, fmt, ap); va_end(ap); }
void ui_warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); line("▲", YELLOW, fmt, ap); va_end(ap); }
void ui_err(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); line("✘", RED, fmt, ap); va_end(ap); }

void ui_kv(const char *k, const char *v)
{
    printf("  %s%-22s%s %s%s%s\n", GREY, k, RESET, BOLD, v, RESET);
}

static void read_line(char *out, size_t sz)
{
    if (!fgets(out, (int)sz, stdin)) { out[0] = 0; return; }
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
}

void ui_ask(const char *label, const char *def, char *out, size_t sz)
{
    if (def && *def) printf("  %s?%s %s %s(%s)%s : ", CYAN, RESET, label, GREY, def, RESET);
    else             printf("  %s?%s %s : ", CYAN, RESET, label);
    fflush(stdout);
    read_line(out, sz);
    if (!*out && def) snprintf(out, sz, "%s", def);
}

int ui_confirm(const char *label, int def)
{
    char buf[16];
    printf("  %s?%s %s %s[%s]%s : ", CYAN, RESET, label, GREY, def ? "O/n" : "o/N", RESET);
    fflush(stdout);
    read_line(buf, sizeof buf);
    if (!*buf) return def;
    return tolower((unsigned char)buf[0]) == 'o' || tolower((unsigned char)buf[0]) == 'y';
}

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* Animated wait: polls check() every 100 ms until it returns non-zero or the timeout expires. */
int ui_wait(const char *label, int (*check)(void *), void *arg, int timeout_ms)
{
    static const char *frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
    long start = now_ms();
    int i = 0, ok = 0;
    while (now_ms() - start < timeout_ms) {
        if (check(arg)) { ok = 1; break; }
        if (use_color()) { printf("\r  %s%s%s %s", CYAN, frames[i++ % 10], RESET, label); fflush(stdout); }
        poll(NULL, 0, 100);
    }
    if (use_color()) printf("\r\033[K");
    if (ok) ui_ok("%s", label); else ui_err("%s", label);
    return ok;
}

/* password prompt without echo */
void ui_ask_secret(const char *label, char *out, size_t sz)
{
    struct termios t, old;
    int tty = isatty(STDIN_FILENO);
    printf("  %s?%s %s : ", CYAN, RESET, label);
    fflush(stdout);
    if (tty) { tcgetattr(STDIN_FILENO, &old); t = old; t.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(STDIN_FILENO, TCSAFLUSH, &t); }
    if (!fgets(out, (int)sz, stdin)) out[0] = 0;
    if (tty) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &old); printf("\n"); }
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
}
