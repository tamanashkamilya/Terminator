/*
 * parse.c - understanding what was typed.
 *
 *  1. is_incomplete(): is the input finished, or do we show "> " and read
 *     another line? (open quote, open $( or `, trailing \ | && ||, if
 *     without fi)
 *  2. lex(): split the text into tokens: words and operators
 *     (| || && & ; newline < > >> 2> 2>> 2>&1 &>). A word keeps its quotes;
 *     they are only removed when the word is expanded.
 *  3. expand_word(): turn one word into its final strings: remove quotes,
 *     replace ~ $VAR ${VAR} $? $1 $# $(...) `...`, split unquoted results
 *     on spaces and expand unquoted * ? [ against the tab's directory.
 */
#include "shell.h"
#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Skipping over quoted parts                                           */
/* ------------------------------------------------------------------ */
/* Each skip_ function starts on an opening character and returns the
 * index just after its closing one. If the text ends first, it sets
 * *open = 1 (the command is not complete yet). */

static int skip_paren(const char *s, int i, int *open);

static int skip_escape(const char *s, int i, int *open)   /* at a backslash */
{
    if (!s[i + 1]) {
        *open = 1;
        return i + 1;
    }
    return i + 2;
}

static int skip_single(const char *s, int i, int *open)
{
    i++;
    while (s[i] && s[i] != '\'')
        i++;
    if (!s[i]) {
        *open = 1;
        return i;
    }
    return i + 1;
}

static int skip_back(const char *s, int i, int *open)     /* `...` */
{
    i++;
    while (s[i] && s[i] != '`')
        i = s[i] == '\\' ? skip_escape(s, i, open) : i + 1;
    if (!s[i]) {
        *open = 1;
        return i;
    }
    return i + 1;
}

static int skip_double(const char *s, int i, int *open)
{
    i++;
    while (s[i] && s[i] != '"') {
        if (s[i] == '\\')
            i = skip_escape(s, i, open);
        else if (s[i] == '$' && s[i + 1] == '(')
            i = skip_paren(s, i + 1, open);
        else if (s[i] == '`')
            i = skip_back(s, i, open);
        else
            i++;
    }
    if (!s[i]) {
        *open = 1;
        return i;
    }
    return i + 1;
}

static int skip_paren(const char *s, int i, int *open)    /* at the '(' of $( */
{
    int depth = 0;
    while (s[i]) {
        char c = s[i];
        if (c == '\\')
            i = skip_escape(s, i, open);
        else if (c == '\'')
            i = skip_single(s, i, open);
        else if (c == '"')
            i = skip_double(s, i, open);
        else if (c == '`')
            i = skip_back(s, i, open);
        else {
            if (c == '(')
                depth++;
            if (c == ')' && --depth == 0)
                return i + 1;
            i++;
        }
    }
    *open = 1;
    return i;
}

/* A word ends at an unquoted space, newline or operator character. */
static int word_end(const char *s, int i, int *open)
{
    while (s[i] && !strchr(" \t\n|&;<>", s[i])) {
        char c = s[i];
        if (c == '\\')
            i = skip_escape(s, i, open);
        else if (c == '\'')
            i = skip_single(s, i, open);
        else if (c == '"')
            i = skip_double(s, i, open);
        else if (c == '`')
            i = skip_back(s, i, open);
        else if (c == '$' && s[i + 1] == '(')
            i = skip_paren(s, i + 1, open);
        else
            i++;
    }
    return i;
}

/* ------------------------------------------------------------------ */
/* Tokens                                                               */
/* ------------------------------------------------------------------ */

/* Split s into tokens. The last token is always T_END. Returns the count.
 * *open is set if a quote, $( or ` is still open or the text ends in \ . */
int lex(const char *s, Token **toks, int *open)
{
    int n = 0, cap = 16, i = 0;
    Token *t = malloc(cap * sizeof *t);
    *open = 0;
    for (;;) {
        /* skip blanks and "\<newline>" between words */
        while (s[i] == ' ' || s[i] == '\t' || (s[i] == '\\' && s[i + 1] == '\n'))
            i += s[i] == '\\' ? 2 : 1;
        if (s[i] == '#') {                           /* comment */
            while (s[i] && s[i] != '\n')
                i++;
            continue;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            t = realloc(t, cap * sizeof *t);
        }
        Token *k = &t[n++];
        k->start = i;
        char c = s[i];
        if (!c) {
            k->type = T_END;
            k->end = i;
            break;
        }
        if (c == '\n') {
            k->type = T_NL; i++;
        } else if (c == '|') {
            if (s[i + 1] == '|') { k->type = T_OR; i += 2; }
            else { k->type = T_PIPE; i++; }
        } else if (c == '&') {
            if (s[i + 1] == '&') { k->type = T_AND; i += 2; }
            else if (s[i + 1] == '>') { k->type = T_ALL_OUT; i += 2; }
            else { k->type = T_BG; i++; }
        } else if (c == ';') {
            k->type = T_SEMI; i++;
        } else if (c == '<') {
            k->type = T_IN; i++;
        } else if (c == '>') {
            if (s[i + 1] == '>') { k->type = T_APPEND; i += 2; }
            else { k->type = T_OUT; i++; }
        } else if (c == '2' && s[i + 1] == '>') {
            if (!strncmp(s + i, "2>&1", 4)) { k->type = T_ERR_TO_OUT; i += 4; }
            else if (s[i + 2] == '>') { k->type = T_ERR_APPEND; i += 3; }
            else { k->type = T_ERR; i += 2; }
        } else {
            k->type = T_WORD;
            i = word_end(s, i, open);
        }
        k->end = i;
    }
    *toks = t;
    return n;
}

int token_is(const char *s, Token *t, const char *word)
{
    int len = t->end - t->start;
    return t->type == T_WORD && len == (int)strlen(word) && !strncmp(s + t->start, word, len);
}

int keyword(const char *s, Token *t)
{
    static const char *names[] = { "if", "then", "elif", "else", "fi" };
    for (int k = 0; k < 5; k++)
        if (token_is(s, t, names[k]))
            return k + 1;               /* K_IF ... K_FI */
    return K_NONE;
}

/* A statement ends at ; & newline or the end. */
int stmt_end(Token *t, int i)
{
    while (t[i].type != T_END && t[i].type != T_SEMI &&
           t[i].type != T_NL && t[i].type != T_BG)
        i++;
    return i;
}

/* Check if/then/elif/else/fi. Returns how many "if" are still open
 * (0 = all closed), or -1 with a message in err for a syntax error. */
int block_depth(const char *s, Token *t, char *err, int size)
{
    int depth = 0, need_then[64], seen_else[64];
    for (int i = 0; t[i].type != T_END;) {
        int e = stmt_end(t, i);
        for (int k = i; k < e;) {
            int kw = keyword(s, &t[k]);
            if (kw == K_IF) {
                if (depth == 64 || k + 1 == e) {
                    snprintf(err, size, "syntax error near 'if'");
                    return -1;
                }
                need_then[depth] = 1;
                seen_else[depth] = 0;
                depth++;
                break;                          /* the rest is the condition */
            } else if (kw == K_ELIF) {
                if (!depth || seen_else[depth - 1] || need_then[depth - 1] || k + 1 == e) {
                    snprintf(err, size, "syntax error near unexpected 'elif'");
                    return -1;
                }
                need_then[depth - 1] = 1;
                break;
            } else if (kw == K_THEN) {
                if (!depth || !need_then[depth - 1]) {
                    snprintf(err, size, "syntax error near unexpected 'then'");
                    return -1;
                }
                need_then[depth - 1] = 0;
                k++;
            } else if (kw == K_ELSE) {
                if (!depth || need_then[depth - 1] || seen_else[depth - 1]) {
                    snprintf(err, size, "syntax error near unexpected 'else'");
                    return -1;
                }
                seen_else[depth - 1] = 1;
                k++;
            } else if (kw == K_FI) {
                if (!depth || need_then[depth - 1]) {
                    snprintf(err, size, "syntax error near unexpected 'fi'");
                    return -1;
                }
                depth--;
                k++;
            } else {
                break;
            }
        }
        i = t[e].type == T_END ? e : e + 1;
    }
    return depth;
}

int is_incomplete(const char *s)
{
    Token *t;
    int open;
    int n = lex(s, &t, &open);
    int result = open;
    if (!result) {
        int k = n - 2;                          /* last token before T_END */
        while (k >= 0 && t[k].type == T_NL)
            k--;
        if (k >= 0 && (t[k].type == T_PIPE || t[k].type == T_AND || t[k].type == T_OR))
            result = 1;
        else {
            char err[128];
            result = block_depth(s, t, err, sizeof err) > 0;
        }
    }
    free(t);
    return result;
}

/* ------------------------------------------------------------------ */
/* Paths                                                                */
/* ------------------------------------------------------------------ */

/* Relative names are relative to the tab's directory, not the process's:
 * in thread mode all tabs share one process. */
void full_path(Shell *sh, const char *name, char *out)
{
    if (name[0] == '/')
        snprintf(out, PATH_MAX, "%s", name);
    else
        snprintf(out, PATH_MAX, "%s/%s", sh->cwd, name);
}

/* Remove "." and "dir/.." parts and double slashes, in place. */
void normalize_path(char *path)
{
    char copy[PATH_MAX], *parts[PATH_MAX / 2], *save;
    int n = 0;
    snprintf(copy, sizeof copy, "%s", path);
    for (char *p = strtok_r(copy, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (!strcmp(p, "."))
            continue;
        if (!strcmp(p, "..")) {
            if (n)
                n--;
            continue;
        }
        parts[n++] = p;
    }
    int len = 0;
    path[0] = 0;
    for (int i = 0; i < n; i++)
        len += snprintf(path + len, PATH_MAX - len, "/%s", parts[i]);
    if (!n)
        strcpy(path, "/");
}

/* ------------------------------------------------------------------ */
/* Expansion                                                            */
/* ------------------------------------------------------------------ */

/* Append the value of $name to out. */
static void add_param(Shell *sh, const char *name, Str *out)
{
    char num[32];
    if (!strcmp(name, "?")) {
        snprintf(num, sizeof num, "%d", sh->status);
        str_adds(out, num);
    } else if (!strcmp(name, "$")) {
        snprintf(num, sizeof num, "%d", (int)getpid());
        str_adds(out, num);
    } else if (!strcmp(name, "#")) {
        snprintf(num, sizeof num, "%d", sh->nparams > 0 ? sh->nparams - 1 : 0);
        str_adds(out, num);
    } else if (!strcmp(name, "@") || !strcmp(name, "*")) {
        for (int i = 1; i < sh->nparams; i++) {
            if (i > 1)
                str_add(out, ' ');
            str_adds(out, sh->params[i]);
        }
    } else if (isdigit((unsigned char)name[0])) {
        int k = atoi(name);
        if (k < sh->nparams)
            str_adds(out, sh->params[k]);
    } else {
        const char *v = get_var(sh, name);
        if (v)
            str_adds(out, v);
    }
}

/* w[i] is '$' or '`'. Put the value into out; return the index after it. */
static int expand_dollar(Shell *sh, const char *w, int i, Str *out)
{
    if (w[i] == '`') {                               /* `cmd` */
        Str cmd = { 0 };
        i++;
        while (w[i] && w[i] != '`') {
            if (w[i] == '\\' && w[i + 1] && strchr("`\\$", w[i + 1]))
                i++;
            str_add(&cmd, w[i++]);
        }
        if (w[i])
            i++;
        char *r = capture(sh, str_get(&cmd));
        str_adds(out, r);
        free(r);
        free(cmd.buf);
        return i;
    }
    char n = w[i + 1];
    if (n == '(') {                                  /* $(cmd) */
        int open = 0;
        int e = skip_paren(w, i + 1, &open);
        int end = open ? e : e - 1;                  /* index of the ')' */
        char *cmd = strndup(w + i + 2, end - (i + 2));
        char *r = capture(sh, cmd);
        str_adds(out, r);
        free(r);
        free(cmd);
        return e;
    }
    if (n == '{') {                                  /* ${NAME} */
        const char *close = strchr(w + i + 2, '}');
        if (!close) {
            str_add(out, '$');
            return i + 1;
        }
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(close - (w + i + 2)), w + i + 2);
        add_param(sh, name, out);
        return (int)(close - w) + 1;
    }
    if (n && strchr("?$#@*", n)) {                   /* $? $$ $# $@ $* */
        char name[2] = { n, 0 };
        add_param(sh, name, out);
        return i + 2;
    }
    if (isdigit((unsigned char)n)) {                 /* $1 */
        char name[2] = { n, 0 };
        add_param(sh, name, out);
        return i + 2;
    }
    if (isalpha((unsigned char)n) || n == '_') {     /* $NAME */
        int j = i + 1;
        while (isalnum((unsigned char)w[j]) || w[j] == '_')
            j++;
        char name[64];
        snprintf(name, sizeof name, "%.*s", j - i - 1, w + i + 1);
        add_param(sh, name, out);
        return j;
    }
    str_add(out, '$');                               /* a lone $ */
    return i + 1;
}

/* The field being built. "pat" is the same text for glob(), with quoted
 * * ? [ escaped so that only unquoted ones are wildcards. */
typedef struct { Str text, pat; int has, wild; } Field;

static void quoted_char(Field *f, char c)
{
    str_add(&f->text, c);
    if (strchr("*?[]\\", c))
        str_add(&f->pat, '\\');
    str_add(&f->pat, c);
    f->has = 1;
}

static void plain_char(Field *f, char c)
{
    str_add(&f->text, c);
    str_add(&f->pat, c);
    f->has = 1;
}

static void finish(Shell *sh, Field *f, List *out, int split)
{
    if (!f->has)
        return;
    int added = 0;
    if (split && f->wild) {
        const char *p = str_get(&f->pat);
        char pattern[PATH_MAX * 2];
        int prefix = 0;
        if (p[0] == '/')
            snprintf(pattern, sizeof pattern, "%s", p);
        else {
            snprintf(pattern, sizeof pattern, "%s/%s", sh->cwd, p);
            prefix = (int)strlen(sh->cwd) + 1;       /* removed from the results */
        }
        glob_t g;
        memset(&g, 0, sizeof g);
        if (glob(pattern, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++)
                list_add(out, strdup(g.gl_pathv[k] + prefix));
            added = 1;
        }
        globfree(&g);
    }
    if (!added)                                      /* no match: keep the word */
        list_add(out, strdup(str_get(&f->text)));
    str_clear(&f->text);
    str_clear(&f->pat);
    f->has = f->wild = 0;
}

/* Expand one word and add the resulting strings to out.
 * split = 1 for command words (split on spaces, expand wildcards),
 * split = 0 for redirection targets and NAME=value. */
void expand_word(Shell *sh, const char *w, List *out, int split)
{
    Field f;
    memset(&f, 0, sizeof f);
    int i = 0;
    if (w[0] == '~' && (w[1] == '/' || w[1] == 0)) {
        const char *home = get_var(sh, "HOME");
        for (const char *p = home ? home : ""; *p; p++)
            quoted_char(&f, *p);
        f.has = 1;
        i = 1;
    }
    while (w[i]) {
        char c = w[i];
        if (c == '\'') {                             /* '...': all literal */
            f.has = 1;
            i++;
            while (w[i] && w[i] != '\'')
                quoted_char(&f, w[i++]);
            if (w[i])
                i++;
        } else if (c == '"') {                       /* "...": $ and ` still work */
            f.has = 1;
            i++;
            while (w[i] && w[i] != '"') {
                if (w[i] == '\\' && w[i + 1] && strchr("$`\"\\\n", w[i + 1])) {
                    /* \<newline> inside quotes keeps the line break, as the
                     * assignment's first example shows (bash would join). */
                    quoted_char(&f, w[i + 1]);
                    i += 2;
                } else if (w[i] == '$' || w[i] == '`') {
                    Str v = { 0 };
                    i = expand_dollar(sh, w, i, &v);
                    for (int k = 0; k < v.len; k++)
                        quoted_char(&f, v.buf[k]);
                    free(v.buf);
                } else {
                    quoted_char(&f, w[i++]);
                }
            }
            if (w[i])
                i++;
        } else if (c == '\\') {
            if (w[i + 1] == '\n')
                i += 2;                              /* line continuation */
            else if (w[i + 1]) {
                quoted_char(&f, w[i + 1]);
                i += 2;
            } else
                i++;
        } else if (c == '$' || c == '`') {           /* unquoted: split on spaces */
            Str v = { 0 };
            i = expand_dollar(sh, w, i, &v);
            for (int k = 0; k < v.len; k++) {
                char d = v.buf[k];
                if (split && (d == ' ' || d == '\t' || d == '\n'))
                    finish(sh, &f, out, split);
                else
                    plain_char(&f, d);
            }
            free(v.buf);
        } else {
            if (c == '*' || c == '?' || c == '[')
                f.wild = 1;
            plain_char(&f, c);
            i++;
        }
    }
    finish(sh, &f, out, split);
    free(f.text.buf);
    free(f.pat.buf);
}

/* Expand a word to exactly one string (no splitting, no wildcards). */
char *expand_one(Shell *sh, const char *word)
{
    List l = { 0 };
    expand_word(sh, word, &l, 0);
    char *r = strdup(l.n ? l.v[0] : "");
    list_free(&l);
    return r;
}
