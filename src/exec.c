/*
 * exec.c - running command text.
 *
 *   run_text()      a whole line or script: statements separated by ; & or
 *                   newlines, with if / elif / else / fi blocks
 *   run_list()      cmd1 && cmd2 || cmd3
 *   run_pipeline()  cmd1 | cmd2 | cmd3 with redirections
 *   launch()        N commands -> N-1 pipes and N children (fork + execvp)
 *   run_script()    a script file, interpreted by us (not by /bin/bash)
 */
#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct { int type; char *target; } Redir;
typedef struct { List words; Redir redir[16]; int nredir; } Cmd;

/* State of one open if-block while running:
 * parent = the code around the if is running,
 * taken  = a branch has already been chosen,
 * active = the current branch is running. */
typedef struct { int parent, taken, active; } Frame;

static int run_list(Shell *sh, const char *s, Token *t, int i, int e, int bg);

/* ------------------------------------------------------------------ */
/* Statements and if-blocks                                             */
/* ------------------------------------------------------------------ */

static void run_statement(Shell *sh, const char *s, Token *t, int i, int e, int bg,
                          Frame *st, int *depth)
{
    while (i < e) {
        int active = *depth == 0 || st[*depth - 1].active;
        int kw = keyword(s, &t[i]);
        if (kw == K_IF) {
            Frame f = { active, 0, 0 };
            if (active) {
                f.active = run_list(sh, s, t, i + 1, e, 0) == 0;
                f.taken = f.active;
            }
            st[(*depth)++] = f;
            return;
        }
        if (kw == K_ELIF) {
            Frame *f = &st[*depth - 1];
            if (!f->parent || f->taken)
                f->active = 0;
            else {
                f->active = run_list(sh, s, t, i + 1, e, 0) == 0;
                f->taken = f->active;
            }
            return;
        }
        if (kw == K_ELSE) {
            Frame *f = &st[*depth - 1];
            f->active = f->parent && !f->taken;
            f->taken = 1;
            i++;
            continue;                 /* "else cmd" on one line runs cmd */
        }
        if (kw == K_THEN) {
            i++;
            continue;
        }
        if (kw == K_FI) {
            (*depth)--;
            i++;
            continue;
        }
        if (active)
            run_list(sh, s, t, i, e, bg);
        return;
    }
}

int run_text(Shell *sh, const char *text)
{
    Token *t;
    int open;
    lex(text, &t, &open);
    char err[128];
    int depth = open ? -1 : block_depth(text, t, err, sizeof err);
    if (open)
        snprintf(err, sizeof err, "syntax error: unexpected end of file (missing quote?)");
    if (depth != 0) {
        say(sh->err, "terminator: %s\n", depth < 0 ? err : "syntax error: missing 'fi'");
        free(t);
        return sh->status = 2;
    }
    Frame st[64];
    int n = 0;
    for (int i = 0; t[i].type != T_END && !sh->exiting && !sh->interrupted;) {
        int e = stmt_end(t, i);
        run_statement(sh, text, t, i, e, t[e].type == T_BG, st, &n);
        i = t[e].type == T_END ? e : e + 1;
    }
    free(t);
    return sh->status;
}

/* cmd1 && cmd2 || cmd3 */
static int run_pipeline(Shell *sh, const char *s, Token *t, int i, int e, int bg);

static int run_list(Shell *sh, const char *s, Token *t, int i, int e, int bg)
{
    int conn = 0;
    while (i <= e && !sh->exiting && !sh->interrupted) {
        int k = i;
        while (k < e && t[k].type != T_AND && t[k].type != T_OR)
            k++;
        if (conn == 0 || (conn == T_AND && sh->status == 0) || (conn == T_OR && sh->status != 0))
            run_pipeline(sh, s, t, i, k, bg && k == e);
        if (k >= e)
            break;
        conn = t[k].type;
        i = k + 1;
    }
    return sh->status;
}

/* ------------------------------------------------------------------ */
/* Running one command in a child                                       */
/* ------------------------------------------------------------------ */

/* Is this file a shell script we should interpret ourselves? */
static int is_script(Shell *sh, const char *name)
{
    char path[PATH_MAX], head[128];
    struct stat st;
    full_path(sh, name, path);
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, head, sizeof head - 1);
    close(fd);
    head[n > 0 ? n : 0] = 0;
    if (!strncmp(head, "#!", 2)) {
        head[strcspn(head, "\n")] = 0;
        return strstr(head, "bash") || strstr(head, "/sh") || strstr(head, " sh");
    }
    int len = (int)strlen(name);
    return len > 3 && !strcmp(name + len - 3, ".sh");
}

/* In the child: redirect, then run a builtin, a script or a program. */
static void run_child(Shell *sh, Cmd *c, List *argv, List *assigns)
{
    for (int k = 0; k < c->nredir; k++) {
        Redir *r = &c->redir[k];
        int fd = -1, to = 1;
        switch (r->type) {
        case T_IN:         fd = open(r->target, O_RDONLY); to = 0; break;
        case T_OUT:        fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0644); break;
        case T_APPEND:     fd = open(r->target, O_WRONLY | O_CREAT | O_APPEND, 0644); break;
        case T_ERR:        fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0644); to = 2; break;
        case T_ERR_APPEND: fd = open(r->target, O_WRONLY | O_CREAT | O_APPEND, 0644); to = 2; break;
        case T_ALL_OUT:    fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0644); break;
        case T_ERR_TO_OUT: dup2(1, 2); continue;
        }
        if (fd < 0) {          /* e.g. missing input file: report, don't run */
            say(2, "terminator: %s: %s\n", r->target, strerror(errno));
            _exit(1);
        }
        dup2(fd, to);
        if (r->type == T_ALL_OUT)
            dup2(fd, 2);
        close(fd);
    }
    for (int k = 0; k < assigns->n; k++) {          /* NAME=value cmd */
        char *eq = strchr(assigns->v[k], '=');
        *eq = 0;
        set_var(sh, assigns->v[k], eq + 1, 1);
    }
    if (argv->n == 0)
        _exit(0);
    char *name = argv->v[0];
    if (is_builtin(name))
        _exit(run_builtin(sh, argv->n, argv->v, 1));
    if ((!strcmp(name, "bash") || !strcmp(name, "sh")) && argv->n > 1) {
        if (!strcmp(argv->v[1], "-c") && argv->n > 2) {
            run_text(sh, argv->v[2]);
            _exit(sh->status);
        }
        if (argv->v[1][0] != '-')
            _exit(run_script(sh, argv->v[1], argv->n - 1, argv->v + 1));
    }
    if (strchr(name, '/') && is_script(sh, name))
        _exit(run_script(sh, name, argv->n, argv->v));
    environ = make_env(sh);
    execvp(name, argv->v);
    if (errno == ENOENT && !strchr(name, '/')) {
        say(2, "terminator: %s: command not found\n", name);
        _exit(127);
    }
    say(2, "terminator: %s: %s\n", name, strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

/* Start every command of a pipeline: N commands, N-1 pipes, N children,
 * all in one process group (the job) so one signal reaches all of them. */
static void launch(Shell *sh, Cmd *cmds, List *argv, List *assigns, int nc,
                   int bg, const char *label)
{
    int first_in = -1, job_input = -1;
    if (bg) {
        first_in = open("/dev/null", O_RDONLY);
    } else if (sh->interactive) {
        /* typed lines are passed to the job through this pipe */
        int p[2];
        if (pipe(p) == 0) {
            first_in = p[0];
            job_input = p[1];
            fcntl(job_input, F_SETFL, O_NONBLOCK);
        }
    }
    pid_t pids[MAX_PIPE], pgid = 0;
    int n = 0, prev = first_in;
    for (int c = 0; c < nc; c++) {
        int p[2] = { -1, -1 };
        if (c < nc - 1 && pipe(p) < 0) {
            say(sh->err, "terminator: pipe: %s\n", strerror(errno));
            break;
        }
        pid_t pid = fork();
        if (pid == 0) {
            if (sh->interactive)
                setpgid(0, pgid);           /* first child: new group */
            child_setup(sh, prev, c < nc - 1 ? p[1] : sh->out);
            run_child(sh, &cmds[c], &argv[c], &assigns[c]);
        }
        if (pid < 0) {
            say(sh->err, "terminator: fork: %s\n", strerror(errno));
            if (p[0] >= 0) { close(p[0]); close(p[1]); }
            break;
        }
        if (sh->interactive) {
            if (!pgid)
                pgid = pid;
            setpgid(pid, pgid);             /* also in the parent: no race */
        }
        pids[n++] = pid;
        if (prev >= 0)
            close(prev);                    /* close unused ends, or no EOF */
        if (p[1] >= 0)
            close(p[1]);
        prev = p[0];
    }
    if (prev >= 0)
        close(prev);
    if (n == 0) {
        if (job_input >= 0)
            close(job_input);
        sh->status = 1;
        return;
    }

    Job *j = sh->interactive ? new_job(sh) : NULL;
    if (!j) {                               /* subshell (or no free job slot) */
        if (job_input >= 0)
            close(job_input);
        if (bg) {
            sh->status = 0;
            return;
        }
        int st = 0;
        for (int k = 0; k < n; k++)
            while (waitpid(pids[k], &st, 0) < 0 && errno == EINTR) {
            }
        sh->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        return;
    }
    j->pgid = pgid;
    j->npids = n;
    memcpy(j->pids, pids, n * sizeof(pid_t));
    j->background = bg;
    j->input = job_input;
    snprintf(j->text, sizeof j->text, "%s", label);
    if (bg) {
        say(sh->out, "[%d] %d\n", job_number(sh, j), (int)pids[n - 1]);
        sh->status = 0;
        return;
    }
    sh->status = wait_job(sh, j);
}

/* ------------------------------------------------------------------ */
/* One pipeline                                                         */
/* ------------------------------------------------------------------ */

static int is_assignment(const char *w)
{
    if (!(w[0] == '_' || (w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z')))
        return 0;
    for (int i = 1; w[i]; i++) {
        if (w[i] == '=')
            return 1;
        if (!(w[i] == '_' || (w[i] >= 'A' && w[i] <= 'Z') || (w[i] >= 'a' && w[i] <= 'z') ||
              (w[i] >= '0' && w[i] <= '9')))
            return 0;
    }
    return 0;
}

/* A builtin that runs inside the shell can't dup2() over the shell's own
 * descriptors (in thread mode they belong to every tab), so its output is
 * simply written to the opened file instead. */
static int builtin_output(Shell *sh, Cmd *c, int *out)
{
    for (int k = 0; k < c->nredir; k++) {
        Redir *r = &c->redir[k];
        int fd = -1;
        char path[PATH_MAX];
        full_path(sh, r->target ? r->target : "", path);
        if (r->type == T_IN)
            fd = open(path, O_RDONLY);
        else if (r->type == T_OUT || r->type == T_ALL_OUT || r->type == T_ERR)
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        else if (r->type == T_APPEND || r->type == T_ERR_APPEND)
            fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        else
            continue;
        if (fd < 0) {
            say(sh->err, "terminator: %s: %s\n", r->target, strerror(errno));
            return 0;
        }
        if (r->type == T_OUT || r->type == T_APPEND || r->type == T_ALL_OUT) {
            if (*out != sh->out)
                close(*out);
            *out = fd;
        } else {
            close(fd);
        }
    }
    return 1;
}

static int run_pipeline(Shell *sh, const char *s, Token *t, int i, int e, int bg)
{
    if (i >= e) {
        say(sh->err, "terminator: syntax error near unexpected token\n");
        return sh->status = 2;
    }
    /* multiWatch takes the raw text: its argument is a list of commands */
    if (token_is(s, &t[i], "multiWatch")) {
        char *rest = strndup(s + t[i].end, t[e - 1].end - t[i].end);
        sh->status = multiwatch(sh, rest);
        free(rest);
        return sh->status;
    }

    static const Cmd empty;
    Cmd cmds[MAX_PIPE];
    List argv[MAX_PIPE], assigns[MAX_PIPE];
    int nc = 1, ok = 1;
    cmds[0] = empty;
    for (int k = i; k < e && ok; k++) {
        Cmd *c = &cmds[nc - 1];
        int ty = t[k].type;
        if (ty == T_WORD) {
            list_add(&c->words, strndup(s + t[k].start, t[k].end - t[k].start));
        } else if (ty == T_PIPE) {
            if (nc == MAX_PIPE || (!c->words.n && !c->nredir))
                ok = 0;
            else
                cmds[nc++] = empty;
        } else if (ty == T_ERR_TO_OUT && c->nredir < 16) {
            c->redir[c->nredir].type = ty;
            c->redir[c->nredir++].target = NULL;
        } else if (ty >= T_IN && ty <= T_ALL_OUT && c->nredir < 16 &&
                   k + 1 < e && t[k + 1].type == T_WORD) {
            c->redir[c->nredir].type = ty;
            c->redir[c->nredir++].target = strndup(s + t[k + 1].start, t[k + 1].end - t[k + 1].start);
            k++;
        } else {
            ok = 0;
        }
    }
    if (ok && !cmds[nc - 1].words.n && !cmds[nc - 1].nredir)
        ok = 0;
    if (!ok) {
        say(sh->err, "terminator: syntax error near unexpected token\n");
        for (int c = 0; c < nc; c++) {
            list_free(&cmds[c].words);
            for (int k = 0; k < cmds[c].nredir; k++)
                free(cmds[c].redir[k].target);
        }
        return sh->status = 2;
    }

    /* expand words now (not earlier), so "cd x; echo $(pwd)" sees the new dir */
    for (int c = 0; c < nc; c++) {
        argv[c] = (List){ 0 };
        assigns[c] = (List){ 0 };
        int k = 0;
        for (; k < cmds[c].words.n && is_assignment(cmds[c].words.v[k]); k++) {
            char *w = cmds[c].words.v[k], *eq = strchr(w, '=');
            char *value = expand_one(sh, eq + 1);
            Str a = { 0 };
            str_addn(&a, w, (int)(eq - w) + 1);
            str_adds(&a, value);
            list_add(&assigns[c], a.buf);
            free(value);
        }
        for (; k < cmds[c].words.n; k++)
            expand_word(sh, cmds[c].words.v[k], &argv[c], 1);
        for (int r = 0; r < cmds[c].nredir; r++)
            if (cmds[c].redir[r].target) {
                char *x = expand_one(sh, cmds[c].redir[r].target);
                free(cmds[c].redir[r].target);
                cmds[c].redir[r].target = x;
            }
    }

    if (nc == 1 && argv[0].n == 0 && !cmds[0].nredir) {
        /* only NAME=value: set shell variables */
        for (int k = 0; k < assigns[0].n; k++) {
            char *eq = strchr(assigns[0].v[k], '=');
            *eq = 0;
            set_var(sh, assigns[0].v[k], eq + 1, -1);
        }
        sh->status = 0;
    } else if (nc == 1 && !bg && argv[0].n && is_builtin(argv[0].v[0])) {
        /* a lone builtin runs in the shell itself: cd, exit, fg ... */
        int out = sh->out;
        if (builtin_output(sh, &cmds[0], &out))
            sh->status = run_builtin(sh, argv[0].n, argv[0].v, out);
        else
            sh->status = 1;
        if (out != sh->out)
            close(out);
    } else {
        char label[256];
        int len = t[e - 1].end - t[i].start;
        snprintf(label, sizeof label, "%.*s", len, s + t[i].start);
        for (char *p = label; *p; p++)
            if (*p == '\n')
                *p = ' ';
        launch(sh, cmds, argv, assigns, nc, bg, label);
    }

    for (int c = 0; c < nc; c++) {
        list_free(&cmds[c].words);
        list_free(&argv[c]);
        list_free(&assigns[c]);
        for (int k = 0; k < cmds[c].nredir; k++)
            free(cmds[c].redir[k].target);
    }
    return sh->status;
}

/* ------------------------------------------------------------------ */
/* Scripts                                                              */
/* ------------------------------------------------------------------ */

/* Read the file and run it with our own interpreter. argv[0] becomes $0,
 * argv[1] $1 and so on. "exit" inside the script only ends the script. */
int run_script(Shell *sh, const char *path, int argc, char **argv)
{
    char full[PATH_MAX];
    full_path(sh, path, full);
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        say(sh->err, "terminator: %s: %s\n", path, strerror(errno));
        return sh->status = 127;
    }
    Str text = { 0 };
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        str_addn(&text, buf, (int)n);
    close(fd);

    char *saved[MAX_PARAMS];
    int nsaved = sh->nparams, was_exiting = sh->exiting;
    memcpy(saved, sh->params, sizeof saved);
    sh->nparams = 0;
    for (int i = 0; i < argc && i < MAX_PARAMS; i++)
        sh->params[sh->nparams++] = argv[i];

    run_text(sh, str_get(&text));

    memcpy(sh->params, saved, sizeof saved);
    sh->nparams = nsaved;
    if (sh->interactive)
        sh->exiting = was_exiting;   /* "exit" in a sourced script keeps the tab */
    free(text.buf);
    return sh->status;
}
