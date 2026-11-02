/*
 * testdriver.c - "./terminator --test [useProcess=True]"
 *
 * Runs the real tab shells (threads or processes, exactly as the GUI does)
 * but without a window, so the shell can be tested automatically. Every
 * line read from stdin is "typed" into the current tab. Special lines:
 *
 *   @C  @Z  @D     press Ctrl+C / Ctrl+Z / Ctrl+D
 *   @wait N        wait N milliseconds for output
 *   @new           open a new tab (it becomes the current one)
 *   @tab N         switch to tab N (1, 2, ...)
 *
 * Everything the tabs print is copied to stdout.
 */
#include "terminator.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int to_tab[MAX_TABS], from_tab[MAX_TABS], ntabs, cur, process_mode;
static pid_t pids[MAX_TABS];
static int esc;          /* inside "ESC ] ... BEL" (the shell's directory) */

static void print_output(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (esc == 2) {
            if (c == 7)
                esc = 0;
        } else if (esc == 1) {
            esc = c == ']' ? 2 : 0;
            if (!esc)
                putchar(c);
        } else if (c == 27) {
            esc = 1;
        } else {
            putchar(c);
        }
    }
    fflush(stdout);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* Copy output to stdout until nothing arrives for quiet ms, or max ms passed. */
static void pump(int quiet, int max)
{
    long end = now_ms() + max;
    for (;;) {
        long left = end - now_ms();
        if (left <= 0)
            return;
        int ms = left < quiet ? (int)left : quiet;
        struct pollfd p[MAX_TABS];
        for (int i = 0; i < ntabs; i++) {
            p[i].fd = from_tab[i];
            p[i].events = POLLIN;
            p[i].revents = 0;
        }
        int r = poll(p, ntabs, ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return;
        for (int i = 0; i < ntabs; i++) {
            if (!p[i].revents)
                continue;
            char buf[4096];
            ssize_t n = read(from_tab[i], buf, sizeof buf);
            if (n > 0)
                print_output(buf, (int)n);
            else if (n == 0 || errno != EINTR) {
                close(from_tab[i]);
                from_tab[i] = -1;
                printf("<<tab %d closed>>\n", i + 1);
            }
        }
    }
}

static void open_tab(const char *cwd)
{
    int fds[2 * MAX_TABS], n = 0;
    for (int i = 0; i < ntabs; i++) {
        if (to_tab[i] >= 0) fds[n++] = to_tab[i];
        if (from_tab[i] >= 0) fds[n++] = from_tab[i];
    }
    pids[ntabs] = shell_spawn(ntabs, cwd, process_mode, &to_tab[ntabs], &from_tab[ntabs], fds, n);
    cur = ntabs++;
}

static void send_to_tab(char type, const char *data)
{
    int len = (int)strlen(data);
    unsigned char h[5] = { (unsigned char)type, len & 255, (len >> 8) & 255,
                           (len >> 16) & 255, (len >> 24) & 255 };
    write_all(to_tab[cur], h, 5);
    write_all(to_tab[cur], data, len);
}

int test_run(int pm)
{
    process_mode = pm;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, "/");
    open_tab(cwd);
    pump(300, 3000);
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strcmp(line, "@C")) {
            printf("^C\n");
            send_to_tab(MSG_INT, "");
        } else if (!strcmp(line, "@Z")) {
            printf("^Z\n");
            send_to_tab(MSG_STOP, "");
        } else if (!strcmp(line, "@D")) {
            send_to_tab(MSG_EOF, "");
        } else if (!strncmp(line, "@wait ", 6)) {
            pump(atoi(line + 6), atoi(line + 6));
            continue;
        } else if (!strcmp(line, "@new")) {
            open_tab(cwd);
            printf("<<tab %d>>\n", cur + 1);
        } else if (!strncmp(line, "@tab ", 5)) {
            cur = atoi(line + 5) - 1;
            printf("<<tab %d>>\n", cur + 1);
        } else {
            printf("%s\n", line);
            send_to_tab(MSG_LINE, line);
        }
        fflush(stdout);
        pump(300, 3000);
    }
    for (int i = 0; i < ntabs; i++)
        close(to_tab[i]);
    pump(500, 3000);
    if (pm)
        for (int i = 0; i < ntabs; i++)
            if (pids[i] > 0)
                waitpid(pids[i], NULL, 0);
    return 0;
}
