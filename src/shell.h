/*
 * shell.h - shared by the shell files (shell.c, parse.c, exec.c, builtins.c).
 * The GUI never includes this file: it only talks to a shell through pipes.
 */
#ifndef SHELL_H
#define SHELL_H

#include "terminator.h"
#include <limits.h>
#include <sys/types.h>

#define MAX_VARS   256
#define MAX_PARAMS 64      /* $0 $1 ... */
#define MAX_JOBS   32
#define MAX_PIPE   32      /* commands in one pipeline */

/* A growable string. Start with: Str s = {0}; */
typedef struct { char *buf; int len, cap; } Str;
void str_add(Str *s, char c);
void str_addn(Str *s, const char *t, int n);
void str_adds(Str *s, const char *t);
void str_clear(Str *s);
const char *str_get(Str *s);

/* A growable, NULL-terminated list of strings (usable as argv). */
typedef struct { char **v; int n, cap; } List;
void list_add(List *l, char *s);
void list_free(List *l);

typedef struct { char name[64]; char *value; int exported; } Var;

/* A job is one pipeline started by the shell (its own process group). */
typedef struct {
    int used;
    int stopped, finished, background;
    pid_t pgid;
    int npids;
    pid_t pids[MAX_PIPE];
    int done[MAX_PIPE];
    int status;          /* exit status of the last command of the pipeline */
    int input;           /* write end of the job's stdin pipe, or -1 */
    char text[256];      /* the command line, for messages */
} Job;

typedef struct {
    int interactive;     /* 1 = a tab's own shell, 0 = a forked subshell */
    int in;              /* messages from the GUI (interactive only) */
    int wake;            /* "a child changed state" pipe (interactive only) */
    int out, err;        /* where output and error messages go */
    char cwd[PATH_MAX];  /* this tab's directory (threads can't have their own) */
    Var vars[MAX_VARS];
    int nvars;
    char *params[MAX_PARAMS];
    int nparams;
    int status;          /* $? */
    Job jobs[MAX_JOBS];
    int interrupted;     /* Ctrl+C: stop the rest of this line or script */
    int exiting;         /* the exit builtin ran, or the tab was closed */
} Shell;

/* Tokens made by lex() */
enum { T_WORD, T_PIPE, T_OR, T_AND, T_BG, T_SEMI, T_NL,
       T_IN, T_OUT, T_APPEND, T_ERR, T_ERR_APPEND, T_ERR_TO_OUT, T_ALL_OUT,
       T_END };
typedef struct { int type, start, end; } Token;   /* text is s[start..end) */

/* if/then/elif/else/fi */
enum { K_NONE, K_IF, K_THEN, K_ELIF, K_ELSE, K_FI };

/* parse.c */
int   lex(const char *s, Token **toks, int *open);
int   is_incomplete(const char *s);
int   token_is(const char *s, Token *t, const char *word);
int   keyword(const char *s, Token *t);
int   stmt_end(Token *t, int i);
int   block_depth(const char *s, Token *t, char *err, int size);
void  expand_word(Shell *sh, const char *word, List *out, int split);
char *expand_one(Shell *sh, const char *word);
void  full_path(Shell *sh, const char *name, char *out);
void  normalize_path(char *path);

/* shell.c */
void *shell_main(void *arg);
void  say(int fd, const char *fmt, ...);
int   read_msg(Shell *sh, char *type, Str *data);
void  child_setup(Shell *sh, int in, int out);
char *capture(Shell *sh, const char *cmd);
Job  *new_job(Shell *sh);
void  free_job(Job *j);
int   wait_job(Shell *sh, Job *j);
void  update_jobs(Shell *sh);
Job  *find_job(Shell *sh, const char *spec);
int   job_number(Shell *sh, Job *j);

/* exec.c */
int run_text(Shell *sh, const char *text);
int run_script(Shell *sh, const char *path, int argc, char **argv);

/* builtins.c */
int  is_builtin(const char *name);
int  run_builtin(Shell *sh, int argc, char **argv, int out);
int  multiwatch(Shell *sh, const char *args);
void init_vars(Shell *sh);
const char *get_var(Shell *sh, const char *name);
void set_var(Shell *sh, const char *name, const char *value, int export);
char **make_env(Shell *sh);

#endif
