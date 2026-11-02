/*
 * builtins.c - commands that run inside the shell, and shell variables.
 *
 * cd and exit must be builtins: a child that calls chdir() or exit() only
 * changes itself. jobs/fg/bg need the shell's job table, history the
 * shared history, source runs a file in this shell. echo and test are
 * builtins so that scripts don't need a new process for every line.
 */
#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ------------------------------------------------------------------ */
/* Variables                                                            */
/* ------------------------------------------------------------------ */

const char *get_var(Shell *sh, const char *name)
{
    for (int i = 0; i < sh->nvars; i++)
        if (!strcmp(sh->vars[i].name, name))
            return sh->vars[i].value;
    return NULL;
}

/* export: 1 = export, 0 = don't, -1 = leave as it is.
 * value NULL keeps the current value (used by "export NAME"). */
void set_var(Shell *sh, const char *name, const char *value, int export)
{
    Var *v = NULL;
    for (int i = 0; i < sh->nvars; i++)
        if (!strcmp(sh->vars[i].name, name))
            v = &sh->vars[i];
    if (!v) {
        if (sh->nvars == MAX_VARS || strlen(name) >= sizeof v->name)
            return;
        v = &sh->vars[sh->nvars++];
        snprintf(v->name, sizeof v->name, "%s", name);
        v->value = strdup("");
        v->exported = 0;
    }
    if (value) {
        char *copy = strdup(value);
        free(v->value);
        v->value = copy;
    }
    if (export >= 0)
        v->exported = export;
}

static void unset_var(Shell *sh, const char *name)
{
    for (int i = 0; i < sh->nvars; i++)
        if (!strcmp(sh->vars[i].name, name)) {
            free(sh->vars[i].value);
            sh->vars[i] = sh->vars[--sh->nvars];
            return;
        }
}

/* Every tab keeps its own copy of the environment (threads can't have
 * separate ones), and gives it to its children with make_env(). */
void init_vars(Shell *sh)
{
    for (char **e = environ; *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || eq - *e >= 64)
            continue;
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(eq - *e), *e);
        set_var(sh, name, eq + 1, 1);
    }
    set_var(sh, "PWD", sh->cwd, 1);
    sh->params[0] = "terminator";
    sh->nparams = 1;
}

char **make_env(Shell *sh)
{
    char **env = malloc((sh->nvars + 1) * sizeof(char *));
    int n = 0;
    for (int i = 0; i < sh->nvars; i++)
        if (sh->vars[i].exported) {
            size_t len = strlen(sh->vars[i].name) + strlen(sh->vars[i].value) + 2;
            env[n] = malloc(len);
            snprintf(env[n++], len, "%s=%s", sh->vars[i].name, sh->vars[i].value);
        }
    env[n] = NULL;
    return env;
}

/* ------------------------------------------------------------------ */
/* test / [                                                             */
/* ------------------------------------------------------------------ */

static int number(const char *s, long *v)
{
    char *end;
    *v = strtol(s, &end, 10);
    while (*end == ' ')
        end++;
    return *s && !*end;
}

/* Returns 0 (true), 1 (false) or 2 (error), like the test command. */
static int test_eval(Shell *sh, int n, char **a)
{
    if (n == 0)
        return 1;
    for (int k = 1; k < n - 1; k++)                     /* x -o y */
        if (!strcmp(a[k], "-o")) {
            int l = test_eval(sh, k, a), r = test_eval(sh, n - k - 1, a + k + 1);
            return (l == 2 || r == 2) ? 2 : (l == 0 || r == 0) ? 0 : 1;
        }
    for (int k = 1; k < n - 1; k++)                     /* x -a y */
        if (!strcmp(a[k], "-a")) {
            int l = test_eval(sh, k, a), r = test_eval(sh, n - k - 1, a + k + 1);
            return (l == 2 || r == 2) ? 2 : (l == 0 && r == 0) ? 0 : 1;
        }
    if (!strcmp(a[0], "!") && n > 1) {
        int r = test_eval(sh, n - 1, a + 1);
        return r == 2 ? 2 : !r;
    }
    if (n == 1)
        return a[0][0] ? 0 : 1;
    if (n == 2) {
        const char *op = a[0], *arg = a[1];
        if (!strcmp(op, "-z")) return arg[0] ? 1 : 0;
        if (!strcmp(op, "-n")) return arg[0] ? 0 : 1;
        char path[PATH_MAX];
        struct stat st;
        full_path(sh, arg, path);
        int ok = stat(path, &st) == 0;
        if (!strcmp(op, "-e")) return ok ? 0 : 1;
        if (!strcmp(op, "-f")) return ok && S_ISREG(st.st_mode) ? 0 : 1;
        if (!strcmp(op, "-d")) return ok && S_ISDIR(st.st_mode) ? 0 : 1;
        if (!strcmp(op, "-s")) return ok && st.st_size > 0 ? 0 : 1;
        if (!strcmp(op, "-r")) return access(path, R_OK) == 0 ? 0 : 1;
        if (!strcmp(op, "-w")) return access(path, W_OK) == 0 ? 0 : 1;
        if (!strcmp(op, "-x")) return access(path, X_OK) == 0 ? 0 : 1;
        say(sh->err, "terminator: test: %s: unary operator expected\n", op);
        return 2;
    }
    if (n == 3) {
        const char *x = a[0], *op = a[1], *y = a[2];
        if (!strcmp(op, "=") || !strcmp(op, "==")) return strcmp(x, y) ? 1 : 0;
        if (!strcmp(op, "!=")) return strcmp(x, y) ? 0 : 1;
        long l, r;
        const char *ops[] = { "-eq", "-ne", "-gt", "-ge", "-lt", "-le" };
        for (int k = 0; k < 6; k++)
            if (!strcmp(op, ops[k])) {
                if (!number(x, &l) || !number(y, &r)) {
                    say(sh->err, "terminator: test: integer expression expected\n");
                    return 2;
                }
                int res[] = { l == r, l != r, l > r, l >= r, l < r, l <= r };
                return res[k] ? 0 : 1;
            }
        say(sh->err, "terminator: test: %s: binary operator expected\n", op);
        return 2;
    }
    say(sh->err, "terminator: test: too many arguments\n");
    return 2;
}

/* ------------------------------------------------------------------ */
/* multiWatch                                                           */
/* ------------------------------------------------------------------ */

#define DASHES "----------------------------------------------------"

typedef struct {
    char *cmd;
    pid_t pid;          /* 0 = not running */
    int fd;             /* read end of its pipe */
    time_t next;        /* when to start it again */
    Str buf;            /* output not printed yet (an unfinished line) */
} Watch;

/* Print the complete lines received so far (everything if all = 1). */
static void watch_print(Shell *sh, Watch *w, int all)
{
    int n = w->buf.len;
    if (!all)
        while (n > 0 && w->buf.buf[n - 1] != '\n')
            n--;
    if (n == 0)
        return;
    Str s = { 0 };
    char head[1024];
    snprintf(head, sizeof head, "\"%s\", %ld:\n", w->cmd, (long)time(NULL));
    str_adds(&s, head);
    str_adds(&s, DASHES "\n");
    str_addn(&s, w->buf.buf, n);
    if (w->buf.buf[n - 1] != '\n')
        str_add(&s, '\n');
    str_adds(&s, DASHES "\n");
    write_all(sh->out, s.buf, s.len);
    free(s.buf);
    memmove(w->buf.buf, w->buf.buf + n, w->buf.len - n);
    w->buf.len -= n;
    w->buf.buf[w->buf.len] = 0;
}

static void watch_start(Shell *sh, Watch *w)
{
    int p[2];
    w->next = time(NULL) + 2;
    if (pipe(p) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int null = open("/dev/null", O_RDONLY);
        sh->err = p[1];                       /* errors go to the pipe too */
        child_setup(sh, null, p[1]);          /* dup2 the pipe onto stdout */
        run_text(sh, w->cmd);
        _exit(sh->status);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return;
    }
    setpgid(pid, pid);
    w->pid = pid;
    w->fd = p[0];
}

/* multiWatch ["cmd1", "cmd2", ...]: run all commands in parallel, one
 * process and one pipe each, and poll() all the pipes. A command is
 * started again 2 s after it finishes. Ctrl+C kills all of them. */
int multiwatch(Shell *sh, const char *args)
{
    Watch w[32];
    int n = 0;
    for (const char *p = args; *p && n < 32; p++) {     /* the quoted commands */
        if (*p != '"' && *p != '\'')
            continue;
        char q = *p++;
        Str c = { 0 };
        while (*p && *p != q) {
            if (*p == '\\' && p[1] == q)
                p++;
            str_add(&c, *p++);
        }
        memset(&w[n], 0, sizeof w[n]);
        w[n].cmd = c.buf ? c.buf : strdup("");
        w[n].fd = -1;
        n++;
        if (!*p)
            break;
    }
    if (n == 0) {
        say(sh->err, "usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");
        return 2;
    }
    if (!sh->interactive) {
        say(sh->err, "terminator: multiWatch only runs in a tab\n");
        for (int k = 0; k < n; k++)
            free(w[k].cmd);
        return 1;
    }

    int stop = 0;
    while (!stop) {
        time_t now = time(NULL);
        struct pollfd pf[33];
        int who[33], np = 0, timeout = -1;
        pf[np].fd = sh->in;
        pf[np].events = POLLIN;
        who[np++] = -1;
        for (int k = 0; k < n; k++) {
            if (w[k].pid == 0 && now >= w[k].next)
                watch_start(sh, &w[k]);
            if (w[k].pid > 0) {
                pf[np].fd = w[k].fd;
                pf[np].events = POLLIN;
                who[np++] = k;
            } else {
                int ms = (int)(w[k].next - now) * 1000;
                if (ms < 0)
                    ms = 0;
                if (timeout < 0 || ms < timeout)
                    timeout = ms;
            }
        }
        for (int k = 0; k < np; k++)
            pf[k].revents = 0;
        if (poll(pf, np, timeout) < 0)
            continue;                           /* SIGCHLD */
        if (pf[0].revents) {                    /* a key from the GUI */
            char type;
            Str d = { 0 };
            if (!read_msg(sh, &type, &d)) {
                stop = 1;
                sh->exiting = 1;
            } else if (type == MSG_INT || type == MSG_EOF) {
                stop = 1;
            }
            free(d.buf);
        }
        for (int q = 1; q < np && !stop; q++) {
            if (!pf[q].revents)
                continue;
            Watch *x = &w[who[q]];
            char buf[4096];
            ssize_t r = read(x->fd, buf, sizeof buf);
            if (r < 0 && errno == EINTR)
                continue;
            if (r > 0) {
                str_addn(&x->buf, buf, (int)r);
                watch_print(sh, x, 0);
            } else {                            /* finished: print the rest */
                watch_print(sh, x, 1);
                close(x->fd);
                x->fd = -1;
                while (waitpid(x->pid, NULL, 0) < 0 && errno == EINTR) {
                }
                x->pid = 0;
                x->next = time(NULL) + 2;
            }
        }
    }
    for (int k = 0; k < n; k++) {               /* Ctrl+C: kill every child */
        if (w[k].pid > 0) {
            kill(-w[k].pid, SIGKILL);
            while (waitpid(w[k].pid, NULL, 0) < 0 && errno == EINTR) {
            }
        }
        if (w[k].fd >= 0)
            close(w[k].fd);
        free(w[k].cmd);
        free(w[k].buf.buf);
    }
    return 130;
}

/* ------------------------------------------------------------------ */
/* The builtins                                                         */
/* ------------------------------------------------------------------ */

static const char *names[] = {
    "cd", "pwd", "exit", "history", "jobs", "fg", "bg", "source", ".",
    "export", "unset", "echo", "test", "[", "clear", "help", NULL
};

int is_builtin(const char *name)
{
    for (int i = 0; names[i]; i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}

static int bi_cd(Shell *sh, int argc, char **argv, int out)
{
    const char *dir = argc > 1 ? argv[1] : get_var(sh, "HOME");
    if (!dir)
        dir = "/";
    if (!strcmp(dir, "-")) {
        dir = get_var(sh, "OLDPWD");
        if (!dir) {
            say(sh->err, "terminator: cd: OLDPWD not set\n");
            return 1;
        }
        say(out, "%s\n", dir);
    }
    char path[PATH_MAX];
    struct stat st;
    full_path(sh, dir, path);
    normalize_path(path);
    if (stat(path, &st) < 0) {
        say(sh->err, "terminator: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        say(sh->err, "terminator: cd: %s: Not a directory\n", dir);
        return 1;
    }
    if (access(path, X_OK) < 0) {
        say(sh->err, "terminator: cd: %s: Permission denied\n", dir);
        return 1;
    }
    set_var(sh, "OLDPWD", sh->cwd, 1);
    snprintf(sh->cwd, sizeof sh->cwd, "%s", path);   /* only this tab moves */
    set_var(sh, "PWD", sh->cwd, 1);
    return 0;
}

static int bi_history(Shell *sh, int out)
{
    int n = history_count(), from = n > 1000 ? n - 1000 : 0;
    char buf[HIST_LEN];
    for (int i = from; i < n; i++) {
        history_get(i, buf, sizeof buf);
        say(out, "%5d  %s\n", i + 1, buf);
    }
    return 0;
}

static int bi_jobs(Shell *sh, int out)
{
    update_jobs(sh);
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &sh->jobs[i];
        if (!j->used)
            continue;
        const char *state = j->finished ? "Done" : j->stopped ? "Stopped" : "Running";
        say(out, "[%d]  %-8s %s%s\n", i + 1, state, j->text,
            (!j->finished && !j->stopped) ? " &" : "");
        if (j->finished)
            free_job(j);
    }
    return 0;
}

static int bi_fg_bg(Shell *sh, int argc, char **argv, int out)
{
    int fg = argv[0][0] == 'f';
    if (!sh->interactive) {
        say(sh->err, "terminator: %s: no job control here\n", argv[0]);
        return 1;
    }
    Job *j = find_job(sh, argc > 1 ? argv[1] : NULL);
    if (!j) {
        say(sh->err, "terminator: %s: %s: no such job\n", argv[0], argc > 1 ? argv[1] : "current");
        return 1;
    }
    j->stopped = 0;
    if (fg) {
        say(out, "%s\n", j->text);
        j->background = 0;
        kill(-j->pgid, SIGCONT);
        return wait_job(sh, j);
    }
    j->background = 1;
    kill(-j->pgid, SIGCONT);
    say(out, "[%d]+ %s &\n", job_number(sh, j), j->text);
    return 0;
}

static int bi_export(Shell *sh, int argc, char **argv, int out)
{
    if (argc == 1)
        for (int i = 0; i < sh->nvars; i++)
            if (sh->vars[i].exported)
                say(out, "export %s=\"%s\"\n", sh->vars[i].name, sh->vars[i].value);
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = 0;
            set_var(sh, argv[i], eq + 1, 1);
            *eq = '=';
        } else {
            set_var(sh, argv[i], NULL, 1);
        }
    }
    return 0;
}

static int bi_echo(int argc, char **argv, int out)
{
    int i = 1, newline = 1;
    if (argc > 1 && !strcmp(argv[1], "-n")) {
        newline = 0;
        i = 2;
    }
    Str s = { 0 };
    for (; i < argc; i++) {
        str_adds(&s, argv[i]);
        if (i < argc - 1)
            str_add(&s, ' ');
    }
    if (newline)
        str_add(&s, '\n');
    write_all(out, str_get(&s), s.len);
    free(s.buf);
    return 0;
}

static int bi_help(int out)
{
    say(out,
        "Terminator builtins:\n"
        "  cd [dir|-]        change this tab's directory\n"
        "  pwd               print it\n"
        "  exit [n]          close this tab\n"
        "  history           the last 1000 commands (shared by all tabs)\n"
        "  jobs, fg [%%n], bg [%%n]   job control\n"
        "  source FILE [args] (or . FILE)   run a script in this shell\n"
        "  export NAME[=value], unset NAME\n"
        "  echo [-n] ..., test ... / [ ... ], clear\n"
        "  multiWatch [\"cmd1\", \"cmd2\", ...]   run commands in parallel every 2 s\n"
        "Keys: Ctrl+R search history, Tab complete file names,\n"
        "      Ctrl+T new tab, Ctrl+Shift+W close tab (change them in ~/.terminatorrc)\n");
    return 0;
}

int run_builtin(Shell *sh, int argc, char **argv, int out)
{
    const char *c = argv[0];
    if (!strcmp(c, "cd"))
        return bi_cd(sh, argc, argv, out);
    if (!strcmp(c, "pwd")) {
        say(out, "%s\n", sh->cwd);
        return 0;
    }
    if (!strcmp(c, "exit")) {
        sh->status = argc > 1 ? atoi(argv[1]) & 255 : sh->status;
        sh->exiting = 1;
        return sh->status;
    }
    if (!strcmp(c, "history"))
        return bi_history(sh, out);
    if (!strcmp(c, "jobs"))
        return bi_jobs(sh, out);
    if (!strcmp(c, "fg") || !strcmp(c, "bg"))
        return bi_fg_bg(sh, argc, argv, out);
    if (!strcmp(c, "source") || !strcmp(c, ".")) {
        if (argc < 2) {
            say(sh->err, "terminator: source: filename argument required\n");
            return 2;
        }
        return run_script(sh, argv[1], argc - 1, argv + 1);
    }
    if (!strcmp(c, "export"))
        return bi_export(sh, argc, argv, out);
    if (!strcmp(c, "unset")) {
        for (int i = 1; i < argc; i++)
            unset_var(sh, argv[i]);
        return 0;
    }
    if (!strcmp(c, "echo"))
        return bi_echo(argc, argv, out);
    if (!strcmp(c, "test") || !strcmp(c, "[")) {
        if (c[0] == '[') {
            if (strcmp(argv[argc - 1], "]")) {
                say(sh->err, "terminator: [: missing ']'\n");
                return 2;
            }
            argc--;
        }
        return test_eval(sh, argc - 1, argv + 1);
    }
    if (!strcmp(c, "clear")) {
        say(out, "\033[2J");      /* the GUI clears the tab when it sees this */
        return 0;
    }
    if (!strcmp(c, "help"))
        return bi_help(out);
    return 127;
}
