/*
 * shell.c - one shell per tab.
 *
 * A tab's shell is the same code in both modes; only how it is started
 * differs (shell_spawn): a thread in thread mode, a forked process in
 * process mode. It talks to the GUI through two pipes:
 *
 *   GUI -> shell : messages (a typed line, Ctrl+C, Ctrl+Z, Ctrl+D)
 *   shell -> GUI : plain text. Commands get this pipe as stdout/stderr,
 *                  so their output goes straight to the tab.
 *
 * Before every prompt the shell also sends "ESC ] 7 ; <dir> BEL" so the
 * GUI knows the tab's directory (for the tab title and Tab completion).
 */
#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Strings and lists                                                    */
/* ------------------------------------------------------------------ */

void str_add(Str *s, char c)
{
    if (s->len + 2 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->buf = realloc(s->buf, s->cap);
    }
    s->buf[s->len++] = c;
    s->buf[s->len] = 0;
}

void str_addn(Str *s, const char *t, int n)
{
    for (int i = 0; i < n; i++)
        str_add(s, t[i]);
}

void str_adds(Str *s, const char *t) { str_addn(s, t, (int)strlen(t)); }

void str_clear(Str *s)
{
    s->len = 0;
    if (s->buf)
        s->buf[0] = 0;
}

const char *str_get(Str *s) { return s->buf ? s->buf : ""; }

void list_add(List *l, char *s)
{
    if (l->n + 2 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, l->cap * sizeof(char *));
    }
    l->v[l->n++] = s;
    l->v[l->n] = NULL;
}

void list_free(List *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

void write_all(int fd, const void *buf, int n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;              /* reader gone (or pipe full and non-blocking) */
        p += w;
        n -= (int)w;
    }
}

/* printf to a file descriptor */
void say(int fd, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1)
        n = sizeof buf - 1;
    if (n > 0)
        write_all(fd, buf, n);
}

/* ------------------------------------------------------------------ */
/* SIGCHLD: wake up the shells                                          */
/* ------------------------------------------------------------------ */

/* Each tab slot has a "wake" pipe. The SIGCHLD handler writes a byte into
 * every one of them (the self-pipe trick). A shell waiting for its job
 * sleeps in poll() on this pipe and on its GUI pipe at the same time, so
 * it notices both "child finished/stopped" and "user pressed Ctrl+C".
 * In thread mode every tab is woken; each one only waitpid()s its own
 * children, so no tab can steal another tab's child. The pipes are never
 * closed, so the handler can never write into a reused descriptor. */
static int wake_r[MAX_TABS], wake_w[MAX_TABS];

static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    for (int i = 0; i < MAX_TABS; i++)
        if (wake_w[i] >= 0)
            (void)!write(wake_w[i], "x", 1);
    errno = saved;
}

static void drain(int fd)
{
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0) {
    }
}

void shell_signals(void)
{
    for (int i = 0; i < MAX_TABS; i++)
        wake_r[i] = wake_w[i] = -1;
    signal(SIGPIPE, SIG_IGN);    /* writing to a closed pipe gives EPIPE instead */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* Starting a tab: thread or process                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int in, out, slot;
    char cwd[PATH_MAX];
} Start;

pid_t shell_spawn(int slot, const char *cwd, int process_mode,
                  int *to_shell, int *from_shell,
                  const int *close_in_child, int nclose)
{
    int a[2], b[2];
    if (pipe(a) < 0)
        return -1;
    if (pipe(b) < 0) {
        close(a[0]);
        close(a[1]);
        return -1;
    }
    if (wake_r[slot] < 0) {
        int w[2];
        if (pipe(w) == 0) {
            fcntl(w[0], F_SETFL, O_NONBLOCK);
            fcntl(w[1], F_SETFL, O_NONBLOCK);
            wake_r[slot] = w[0];
            wake_w[slot] = w[1];
        }
    }
    drain(wake_r[slot]);

    Start *st = malloc(sizeof *st);
    st->in = a[0];
    st->out = b[1];
    st->slot = slot;
    snprintf(st->cwd, sizeof st->cwd, "%s", cwd);
    *to_shell = a[1];
    *from_shell = b[0];

    if (process_mode) {
        pid_t pid = fork();
        if (pid == 0) {
            /* The tab process keeps only its own pipes: not the X
             * connection and not the other tabs' pipes. */
            for (int i = 0; i < nclose; i++)
                close(close_in_child[i]);
            close(a[1]);
            close(b[0]);
            for (int i = 0; i < MAX_TABS; i++)
                if (i != slot && wake_r[i] >= 0) {
                    int r = wake_r[i], w = wake_w[i];
                    wake_r[i] = wake_w[i] = -1;
                    close(r);
                    close(w);
                }
            shell_main(st);
            _exit(0);
        }
        close(a[0]);
        close(b[1]);
        free(st);
        if (pid < 0) {
            close(a[1]);
            close(b[0]);
        }
        return pid;
    }

    /* Thread mode. macOS gives threads a 512 KB stack; ask for 8 MB. */
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int err = pthread_create(&th, &attr, shell_main, st);
    pthread_attr_destroy(&attr);
    if (err) {
        free(st);
        close(a[0]);
        close(a[1]);
        close(b[0]);
        close(b[1]);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Messages from the GUI                                                */
/* ------------------------------------------------------------------ */

static int read_full(int fd, void *buf, int n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 0;
        p += r;
        n -= (int)r;
    }
    return 1;
}

/* Read one message. Returns 0 when the GUI has closed the tab. */
int read_msg(Shell *sh, char *type, Str *data)
{
    unsigned char h[5];
    if (!read_full(sh->in, h, 5))
        return 0;
    int len = h[1] | h[2] << 8 | h[3] << 16 | h[4] << 24;
    str_clear(data);
    while (len > 0) {
        char buf[1024];
        int k = len < (int)sizeof buf ? len : (int)sizeof buf;
        if (!read_full(sh->in, buf, k))
            return 0;
        str_addn(data, buf, k);
        len -= k;
    }
    *type = (char)h[0];
    return 1;
}

/* Wait for the next message while the shell is idle at its prompt. */
static int wait_msg(Shell *sh, char *type, Str *data)
{
    for (;;) {
        struct pollfd p[2] = { { sh->in, POLLIN, 0 }, { sh->wake, POLLIN, 0 } };
        if (poll(p, 2, -1) < 0)
            continue;                         /* interrupted by SIGCHLD */
        if (p[1].revents) {
            drain(sh->wake);
            update_jobs(sh);                  /* reap finished background jobs */
        }
        if (p[0].revents)
            return read_msg(sh, type, data);
    }
}

/* ------------------------------------------------------------------ */
/* Children                                                             */
/* ------------------------------------------------------------------ */

/* Run in every forked child: restore signals, wire up stdin/stdout/stderr,
 * close every other descriptor, and move into the tab's directory. After
 * this the child is a plain subshell that writes to fd 1 and 2. */
void child_setup(Shell *sh, int in, int out)
{
    int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE, SIGHUP };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        signal(sigs[i], SIG_DFL);
    if (in >= 0 && in != 0)
        dup2(in, 0);
    if (out != 1)
        dup2(out, 1);
    if (sh->err != 2)
        dup2(sh->err, 2);
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 4096)
        max = 4096;
    for (int fd = 3; fd < max; fd++)
        close(fd);
    if (chdir(sh->cwd) < 0) {
        /* directory vanished: stay where we are */
    }
    sh->interactive = 0;
    sh->in = sh->wake = -1;
    sh->out = 1;
    sh->err = 2;
    memset(sh->jobs, 0, sizeof sh->jobs);
}

/* $(cmd) and `cmd`: run cmd in a subshell and return what it printed,
 * without trailing newlines. The inner $(...) of a nested one runs first,
 * because the subshell expands its own words before running them. */
char *capture(Shell *sh, const char *cmd)
{
    int p[2];
    if (pipe(p) < 0)
        return strdup("");
    pid_t pid = fork();
    if (pid == 0) {
        if (sh->interactive)
            setpgid(0, 0);           /* own group, so Ctrl+C can reach it */
        int in = sh->interactive ? open("/dev/null", O_RDONLY) : -1;
        child_setup(sh, in, p[1]);
        run_text(sh, cmd);
        _exit(sh->status);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return strdup("");
    }
    if (sh->interactive)
        setpgid(pid, pid);

    Str out = { 0 };
    int gui = sh->interactive ? sh->in : -1;
    for (;;) {
        struct pollfd f[2] = { { p[0], POLLIN, 0 }, { gui, POLLIN, 0 } };
        if (poll(f, 2, -1) < 0)
            continue;
        if (f[1].revents) {                   /* a key while we wait */
            char type;
            Str d = { 0 };
            if (!read_msg(sh, &type, &d)) {
                kill(-pid, SIGKILL);
                sh->exiting = 1;
                gui = -1;
            } else if (type == MSG_INT) {
                kill(-pid, SIGINT);
                sh->interrupted = 1;
            }
            free(d.buf);
        }
        if (!f[0].revents)
            continue;
        char buf[4096];
        ssize_t n = read(p[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        str_addn(&out, buf, (int)n);
    }
    close(p[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    sh->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    while (out.len > 0 && out.buf[out.len - 1] == '\n')
        out.buf[--out.len] = 0;
    return out.buf ? out.buf : strdup("");
}

/* ------------------------------------------------------------------ */
/* Jobs                                                                 */
/* ------------------------------------------------------------------ */

Job *new_job(Shell *sh)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (!sh->jobs[i].used) {
            Job *j = &sh->jobs[i];
            memset(j, 0, sizeof *j);
            j->used = 1;
            j->input = -1;
            return j;
        }
    return NULL;
}

void free_job(Job *j)
{
    if (j->input >= 0)
        close(j->input);
    j->input = -1;
    j->used = 0;
}

int job_number(Shell *sh, Job *j) { return (int)(j - sh->jobs) + 1; }

/* "%2", "2" or nothing (= the newest job) */
Job *find_job(Shell *sh, const char *spec)
{
    update_jobs(sh);
    if (!spec) {
        for (int i = MAX_JOBS - 1; i >= 0; i--)
            if (sh->jobs[i].used && !sh->jobs[i].finished)
                return &sh->jobs[i];
        return NULL;
    }
    if (*spec == '%')
        spec++;
    int n = atoi(spec);
    if (n < 1 || n > MAX_JOBS || !sh->jobs[n - 1].used || sh->jobs[n - 1].finished)
        return NULL;
    return &sh->jobs[n - 1];
}

/* Collect the state of this shell's own children, without blocking. */
void update_jobs(Shell *sh)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &sh->jobs[i];
        if (!j->used || j->finished)
            continue;
        int left = 0;
        for (int k = 0; k < j->npids; k++) {
            if (j->done[k])
                continue;
            int st;
            pid_t r = waitpid(j->pids[k], &st, WNOHANG | WUNTRACED | WCONTINUED);
            if (r == 0) {
                left++;
            } else if (r < 0) {
                j->done[k] = 1;              /* already gone */
            } else if (WIFSTOPPED(st)) {
                j->stopped = 1;
                left++;
            } else if (WIFCONTINUED(st)) {
                j->stopped = 0;
                left++;
            } else {
                j->done[k] = 1;
                if (k == j->npids - 1)
                    j->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            }
        }
        if (!left) {
            j->finished = 1;
            j->stopped = 0;
        }
    }
}

/* Wait for a foreground job. While waiting, keys from the GUI still arrive:
 * Ctrl+C and Ctrl+Z become signals to the job's process group, typed lines
 * become the job's input, Ctrl+D closes its input. */
int wait_job(Shell *sh, Job *j)
{
    for (;;) {
        update_jobs(sh);
        if (j->finished || j->stopped)
            break;
        struct pollfd p[2] = { { sh->in, POLLIN, 0 }, { sh->wake, POLLIN, 0 } };
        if (poll(p, 2, -1) < 0)
            continue;
        if (p[1].revents)
            drain(sh->wake);
        if (!p[0].revents)
            continue;
        char type;
        Str data = { 0 };
        if (!read_msg(sh, &type, &data)) {    /* tab closed */
            kill(-j->pgid, SIGHUP);
            kill(-j->pgid, SIGCONT);
            sh->exiting = 1;
            free(data.buf);
            break;
        }
        if (type == MSG_INT) {
            kill(-j->pgid, SIGINT);
            sh->interrupted = 1;
        } else if (type == MSG_STOP) {
            kill(-j->pgid, SIGTSTP);
        } else if (type == MSG_LINE && j->input >= 0) {
            str_add(&data, '\n');
            write_all(j->input, data.buf, data.len);
        } else if (type == MSG_EOF && j->input >= 0) {
            close(j->input);
            j->input = -1;
        }
        free(data.buf);
    }
    if (j->stopped) {
        j->background = 1;
        say(sh->out, "[%d]+ Stopped %s\n", job_number(sh, j), j->text);
        return 128 + SIGTSTP;
    }
    if (!j->finished)            /* tab was closed while it ran */
        return 129;
    int status = j->status;
    free_job(j);
    return status;
}

/* Print "Done" for background jobs that finished (before the prompt). */
static void report_jobs(Shell *sh)
{
    update_jobs(sh);
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &sh->jobs[i];
        if (j->used && j->finished) {
            say(sh->out, "[%d]+ Done %s\n", i + 1, j->text);
            free_job(j);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The tab's main loop                                                  */
/* ------------------------------------------------------------------ */

static int blank(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n')
            return 0;
    return 1;
}

void *shell_main(void *arg)
{
    Start *st = arg;
    Shell *sh = calloc(1, sizeof *sh);
    sh->interactive = 1;
    sh->in = st->in;
    sh->out = sh->err = st->out;
    sh->wake = wake_r[st->slot];
    snprintf(sh->cwd, sizeof sh->cwd, "%s", st->cwd);
    free(st);
    init_vars(sh);
    const char *user = get_var(sh, "USER");
    char name[64];
    snprintf(name, sizeof name, "%s", user && *user ? user : "user");

    Str line = { 0 }, cmd = { 0 };      /* cmd collects lines until complete */
    int prompt = 1;
    while (!sh->exiting) {
        if (prompt) {
            report_jobs(sh);
            say(sh->out, "\033]7;%s\007%s@terminator> ", sh->cwd, name);
            prompt = 0;
        }
        char type;
        if (!wait_msg(sh, &type, &line))
            break;                              /* the GUI closed this tab */
        if (type == MSG_LINE) {
            if (cmd.len)
                str_add(&cmd, '\n');
            str_adds(&cmd, str_get(&line));
            if (is_incomplete(str_get(&cmd))) {  /* open quote, trailing \ ... */
                say(sh->out, "> ");
                continue;
            }
            if (!blank(str_get(&cmd))) {
                history_add(str_get(&cmd));
                sh->interrupted = 0;
                run_text(sh, str_get(&cmd));
            }
            str_clear(&cmd);
            prompt = 1;
        } else if (type == MSG_EOF) {
            if (!cmd.len)
                break;                          /* Ctrl+D at an empty prompt */
            say(sh->err, "terminator: unexpected end of input\n");
            str_clear(&cmd);
            prompt = 1;
        } else {                                /* Ctrl+C / Ctrl+Z at the prompt */
            str_clear(&cmd);
            prompt = 1;
        }
    }

    /* Leaving: hang up this tab's jobs (like bash) and close the pipes. */
    for (int i = 0; i < MAX_JOBS; i++)
        if (sh->jobs[i].used) {
            kill(-sh->jobs[i].pgid, SIGHUP);
            kill(-sh->jobs[i].pgid, SIGCONT);
            free_job(&sh->jobs[i]);
        }
    close(sh->in);
    close(sh->out);
    for (int i = 0; i < sh->nvars; i++)
        free(sh->vars[i].value);
    free(line.buf);
    free(cmd.buf);
    free(sh);
    return NULL;
}
