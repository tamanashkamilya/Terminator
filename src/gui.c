/*
 * gui.c - the X11 window.
 *
 * The GUI owns everything you see: the tab bar, one text buffer per tab
 * and the line being typed (like a terminal's "cooked" line editing).
 * Enter sends the line to the tab's shell through a pipe; whatever the
 * shell and its programs print comes back through another pipe and is
 * added to that tab's buffer - also while the tab is hidden.
 *
 * The event loop sleeps in poll() on the X connection and on every tab's
 * output pipe at once, so it never busy-waits. Only this file uses Xlib.
 */
#include "terminator.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SCROLLBACK 5000     /* rows kept per tab */
#define MAXCOL     400      /* widest row */
#define INPUT_MAX  4096     /* longest line you can type */
#define TAIL_MAX   300      /* rows used to show the prompt and typed line */
#define PAD        6        /* pixels left of the text */

enum { MODE_NORMAL, MODE_SEARCH, MODE_PICK };

typedef struct {
    int used, closing;          /* closing: closed by the user, shell still exiting */
    int to_shell, from_shell;   /* our ends of the two pipes */
    pid_t pid;                  /* process mode: the tab's process */

    char (*rows)[MAXCOL + 1];   /* the text; the last row is the current line */
    int nrows, col;
    int esc, esc_len;           /* escape sequence being read */
    char esc_buf[PATH_MAX + 16];
    char cwd[PATH_MAX];         /* sent by the shell before each prompt */
    int scroll;                 /* rows scrolled back */

    char input[INPUT_MAX];      /* the line being typed */
    int len, cur;
    char draft[INPUT_MAX];      /* what was typed before pressing Up */
    int hist_pos;               /* -1 = not browsing history */

    int mode;                   /* normal, Ctrl+R search, or picking a completion */
    char mode_buf[256];
    int mode_len;
    char saved_prompt[MAXCOL + 1];
    char **matches;             /* completion choices */
    int nmatches, prefix_len;
} Tab;

static Display *dpy;
static Window win;
static GC gc;
static Pixmap pix;
static XFontStruct *font;
static int W = 920, H = 560, cw, ch, ascent, bar_h, text_top, cols = 80, rows_vis = 24;
static unsigned long c_bg, c_fg, c_bar, c_bar_text, c_active, c_cursor, c_sel;
static Atom a_clipboard, a_utf8, a_targets, a_prop, a_delete;

static Tab tabs[MAX_TABS];
static int active = -1, process_mode, dirty = 1;
static int tab_x0[MAX_TABS], tab_x1[MAX_TABS], plus_x0, plus_x1;

static char *clip_text, *primary_text;            /* what we offer to paste */
static int sel_on, sel_drag, sel_r1, sel_c1, sel_r2, sel_c2;
static int draw_first;                            /* first row on screen */

/* The prompt line + typed text, cut into screen rows (see layout_tail) */
static char tail[INPUT_MAX + MAXCOL + 64];
static int tail_start[TAIL_MAX], tail_len[TAIL_MAX], ntail, cur_row, cur_col;

static void open_tab(const char *cwd);

/* ------------------------------------------------------------------ */
/* The text buffer of a tab                                             */
/* ------------------------------------------------------------------ */

static void new_row(Tab *t)
{
    if (t->nrows == SCROLLBACK) {               /* forget the oldest rows */
        int drop = SCROLLBACK / 5;
        memmove(t->rows, t->rows + drop, (size_t)(SCROLLBACK - drop) * sizeof *t->rows);
        t->nrows -= drop;
        sel_on = 0;
    }
    t->rows[t->nrows++][0] = 0;
    t->col = 0;
}

static void put_char(Tab *t, char c)
{
    if (c == '\n') { new_row(t); return; }
    if (c == '\r') { t->col = 0; return; }
    if (c == '\b') { if (t->col > 0) t->col--; return; }
    if (c == '\t') {
        do put_char(t, ' '); while (t->col % 8);
        return;
    }
    if ((unsigned char)c < 32)
        return;
    if (t->col >= cols)                         /* wrap long lines */
        new_row(t);
    char *row = t->rows[t->nrows - 1];
    int len = (int)strlen(row);
    while (len < t->col)
        row[len++] = ' ';
    row[t->col] = c;
    if (t->col == len)
        row[len + 1] = 0;
    t->col++;
}

static void clear_tab(Tab *t, int keep_line)
{
    if (keep_line)
        memmove(t->rows[0], t->rows[t->nrows - 1], MAXCOL + 1);
    else {
        t->rows[0][0] = 0;
        t->col = 0;
    }
    t->nrows = 1;
    t->scroll = 0;
    sel_on = 0;
    dirty = 1;
}

/* Add output to a tab. Understands \n \r \b \t, "ESC [ 2 J" (clear) and
 * "ESC ] 7 ; dir BEL" (the shell's directory); other escapes (colours)
 * are skipped. */
static void feed(Tab *t, const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        switch (t->esc) {
        case 0:
            if (b == 27)
                t->esc = 1;
            else if (b >= 0x80) {
                if ((b & 0xC0) != 0x80)          /* one ? per UTF-8 character */
                    put_char(t, '?');
            } else
                put_char(t, (char)b);
            break;
        case 1:
            t->esc_len = 0;
            t->esc = b == '[' ? 2 : b == ']' ? 3 : 0;
            break;
        case 2:                                  /* ESC [ ... letter */
            if (b >= 0x40 && b <= 0x7e) {
                t->esc_buf[t->esc_len] = 0;
                if (b == 'J' && (!strcmp(t->esc_buf, "2") || !strcmp(t->esc_buf, "3")))
                    clear_tab(t, 0);
                t->esc = 0;
            } else if (t->esc_len < (int)sizeof t->esc_buf - 1)
                t->esc_buf[t->esc_len++] = (char)b;
            break;
        case 3:                                  /* ESC ] ... BEL */
            if (b == 7 || b == 27) {
                t->esc_buf[t->esc_len] = 0;
                if (!strncmp(t->esc_buf, "7;", 2))
                    snprintf(t->cwd, sizeof t->cwd, "%s", t->esc_buf + 2);
                t->esc = b == 27 ? 1 : 0;
            } else if (t->esc_len < (int)sizeof t->esc_buf - 1)
                t->esc_buf[t->esc_len++] = (char)b;
            break;
        }
    }
    dirty = 1;
}

static void show(Tab *t, const char *s) { feed(t, s, (int)strlen(s)); }

/* ------------------------------------------------------------------ */
/* Tabs                                                                 */
/* ------------------------------------------------------------------ */

static int visible(int i) { return tabs[i].used && !tabs[i].closing; }

static void send_msg(Tab *t, char type, const char *data, int len)
{
    if (t->to_shell < 0)
        return;
    unsigned char h[5] = { (unsigned char)type, len & 255, (len >> 8) & 255,
                           (len >> 16) & 255, (len >> 24) & 255 };
    write_all(t->to_shell, h, 5);
    write_all(t->to_shell, data, len);
}

static void quit(void)
{
    for (int i = 0; i < MAX_TABS; i++)       /* each shell hangs up its jobs */
        if (tabs[i].used && tabs[i].to_shell >= 0)
            close(tabs[i].to_shell);
    usleep(200 * 1000);
    history_cleanup();
    XCloseDisplay(dpy);
    exit(0);
}

static void set_active(int i)
{
    active = i;
    sel_on = sel_drag = 0;
    dirty = 1;
}

/* Choose another tab to show after tab i goes away. */
static void pick_active(int i)
{
    for (int k = 1; k <= MAX_TABS; k++) {
        int j = (i + MAX_TABS - k) % MAX_TABS;     /* prefer the one on the left */
        if (visible(j)) {
            set_active(j);
            return;
        }
    }
    quit();                                       /* that was the last tab */
}

static void close_tab(int i)
{
    if (tabs[i].to_shell >= 0) {      /* the shell sees EOF and exits */
        close(tabs[i].to_shell);
        tabs[i].to_shell = -1;
    }
    tabs[i].closing = 1;
    if (active == i)
        pick_active(i);
    dirty = 1;
}

static void free_matches(Tab *t)
{
    for (int k = 0; k < t->nmatches; k++)
        free(t->matches[k]);
    free(t->matches);
    t->matches = NULL;
    t->nmatches = 0;
}

/* The tab's shell exited (exit, Ctrl+D, closed, or crashed). */
static void tab_gone(int i)
{
    Tab *t = &tabs[i];
    close(t->from_shell);
    if (t->to_shell >= 0)
        close(t->to_shell);
    if (process_mode && t->pid > 0)
        waitpid(t->pid, NULL, 0);
    free(t->rows);
    free_matches(t);
    int was_visible = !t->closing;
    t->used = t->closing = 0;
    if (was_visible && active == i)
        pick_active(i);
    dirty = 1;
}

static void open_tab(const char *cwd)
{
    int i = 0;
    while (i < MAX_TABS && tabs[i].used)
        i++;
    if (i == MAX_TABS)
        return;
    /* descriptors the new tab process must not keep */
    int fds[2 * MAX_TABS + 1], n = 0;
    fds[n++] = ConnectionNumber(dpy);
    for (int k = 0; k < MAX_TABS; k++)
        if (tabs[k].used) {
            if (tabs[k].to_shell >= 0)
                fds[n++] = tabs[k].to_shell;
            fds[n++] = tabs[k].from_shell;
        }
    Tab *t = &tabs[i];
    memset(t, 0, sizeof *t);
    t->rows = calloc(SCROLLBACK, sizeof *t->rows);
    t->nrows = 1;
    t->hist_pos = -1;
    snprintf(t->cwd, sizeof t->cwd, "%s", cwd);
    t->pid = shell_spawn(i, cwd, process_mode, &t->to_shell, &t->from_shell, fds, n);
    if (t->pid < 0) {
        free(t->rows);
        return;
    }
    t->used = 1;
    set_active(i);
}

static void switch_tab(int dir)
{
    for (int k = 1; k <= MAX_TABS; k++) {
        int j = (active + dir * k + MAX_TABS * k) % MAX_TABS;
        if (visible(j)) {
            set_active(j);
            return;
        }
    }
}

static void goto_tab(int n)          /* n-th visible tab, from 0 */
{
    for (int i = 0; i < MAX_TABS; i++)
        if (visible(i) && n-- == 0) {
            set_active(i);
            return;
        }
}

/* ------------------------------------------------------------------ */
/* Editing the typed line                                               */
/* ------------------------------------------------------------------ */

static void insert_text(Tab *t, const char *s, int n)
{
    for (int k = 0; k < n; k++) {
        char c = s[k];
        if (c == '\r')
            continue;
        if (c == '\t')
            c = ' ';
        if ((unsigned char)c < 32 && c != '\n')
            continue;
        if (t->len >= INPUT_MAX - 1)
            break;
        memmove(t->input + t->cur + 1, t->input + t->cur, t->len - t->cur);
        t->input[t->cur++] = c;
        t->len++;
    }
    t->input[t->len] = 0;
}

static void delete_range(Tab *t, int from, int to)
{
    if (from < 0)
        from = 0;
    if (to > t->len)
        to = t->len;
    if (from >= to)
        return;
    memmove(t->input + from, t->input + to, t->len - to);
    t->len -= to - from;
    t->cur = from;
    t->input[t->len] = 0;
}

static void set_input(Tab *t, const char *s)
{
    snprintf(t->input, INPUT_MAX, "%s", s);
    t->len = t->cur = (int)strlen(t->input);
}

/* Up / Down: walk through the shared history. */
static void history_move(Tab *t, int dir)
{
    int count = history_count();
    int pos = t->hist_pos < 0 ? count : t->hist_pos;
    if (dir < 0) {
        if (pos == 0)
            return;
        if (t->hist_pos < 0)
            memcpy(t->draft, t->input, t->len + 1);
        pos--;
    } else {
        if (t->hist_pos < 0)
            return;
        pos++;
    }
    if (pos >= count) {
        set_input(t, t->draft);
        t->hist_pos = -1;
    } else {
        char buf[HIST_LEN];
        history_get(pos, buf, sizeof buf);
        set_input(t, buf);
        t->hist_pos = pos;
    }
}

/* Enter: show the line in the buffer and send it to the shell. */
static void submit(Tab *t)
{
    feed(t, t->input, t->len);
    feed(t, "\n", 1);
    send_msg(t, MSG_LINE, t->input, t->len);
    set_input(t, "");
    t->hist_pos = -1;
}

/* Ctrl+C / Ctrl+Z: echo ^C, forget the line, tell the shell. */
static void send_key(Tab *t, const char *echo, char type)
{
    feed(t, t->input, t->len);
    show(t, echo);
    set_input(t, "");
    t->hist_pos = -1;
    send_msg(t, type, "", 0);
}

/* Ctrl+R and the completion list print something below the prompt and then
 * show the prompt again. The prompt is whatever the current row holds. */
static void begin_mode(Tab *t, int mode)
{
    snprintf(t->saved_prompt, sizeof t->saved_prompt, "%s", t->rows[t->nrows - 1]);
    feed(t, t->input, t->len);
    show(t, "\n");
    t->mode = mode;
    t->mode_len = 0;
    t->mode_buf[0] = 0;
}

static void end_mode(Tab *t)
{
    show(t, t->saved_prompt);
    t->mode = MODE_NORMAL;
    free_matches(t);
}

/* ------------------------------------------------------------------ */
/* Tab completion                                                       */
/* ------------------------------------------------------------------ */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Complete the word before the cursor with file names from the tab's
 * directory (or the directory written in the word, like "src/ma"). */
static void complete(Tab *t)
{
    int ws = t->cur;
    while (ws > 0 && t->input[ws - 1] != ' ')
        ws--;
    char word[PATH_MAX], dir[PATH_MAX];
    snprintf(word, sizeof word, "%.*s", t->cur - ws, t->input + ws);
    char *slash = strrchr(word, '/');
    const char *prefix = word;
    const char *home = getenv("HOME");
    if (slash) {
        *slash = 0;
        prefix = slash + 1;
        if (word[0] == 0)
            snprintf(dir, sizeof dir, "/");
        else if (word[0] == '/')
            snprintf(dir, sizeof dir, "%s", word);
        else if (word[0] == '~')
            snprintf(dir, sizeof dir, "%s%s", home ? home : "", word + 1);
        else
            snprintf(dir, sizeof dir, "%s/%s", t->cwd, word);
    } else {
        snprintf(dir, sizeof dir, "%s", t->cwd);
    }

    DIR *d = opendir(dir);
    if (!d)
        return;
    char **m = NULL;
    int n = 0, plen = (int)strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (e->d_name[0] == '.' && prefix[0] != '.')
            continue;                               /* hidden files */
        if (strncmp(e->d_name, prefix, plen))
            continue;
        char full[PATH_MAX * 2];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        m = realloc(m, (n + 1) * sizeof *m);
        m[n] = malloc(strlen(e->d_name) + 2);
        sprintf(m[n++], "%s%s", e->d_name, is_dir ? "/" : "");
    }
    closedir(d);
    if (n == 0)
        return;                                     /* no match: do nothing */
    qsort(m, n, sizeof *m, cmp_str);

    int common = (int)strlen(m[0]);                 /* longest common prefix */
    for (int k = 1; k < n; k++) {
        int j = 0;
        while (j < common && m[k][j] == m[0][j])
            j++;
        common = j;
    }
    if (n == 1 || common > plen) {
        insert_text(t, m[0] + plen, (n == 1 ? (int)strlen(m[0]) : common) - plen);
        for (int k = 0; k < n; k++)
            free(m[k]);
        free(m);
        return;
    }
    /* still ambiguous: show a numbered list and let the user pick */
    begin_mode(t, MODE_PICK);
    for (int k = 0; k < n; k++) {
        char item[PATH_MAX + 16];
        snprintf(item, sizeof item, "%d. %s%s", k + 1, m[k], k < n - 1 ? "   " : "\n");
        show(t, item);
    }
    t->matches = m;
    t->nmatches = n;
    t->prefix_len = plen;
}

static void pick_key(Tab *t, int act, KeySym ks, const char *text, int n)
{
    if (ks == XK_Escape || act == A_INTERRUPT) {
        end_mode(t);
        return;
    }
    if (ks == XK_BackSpace) {
        if (t->mode_len)
            t->mode_buf[--t->mode_len] = 0;
        return;
    }
    int enter = ks == XK_Return || ks == XK_KP_Enter;
    if (n == 1 && text[0] >= '0' && text[0] <= '9' && t->mode_len < 6) {
        t->mode_buf[t->mode_len++] = text[0];
        t->mode_buf[t->mode_len] = 0;
    } else if (!enter)
        return;
    int k = atoi(t->mode_buf);
    if (k >= 1 && k <= t->nmatches && (enter || k * 10 > t->nmatches)) {
        char choice[PATH_MAX];
        snprintf(choice, sizeof choice, "%s", t->matches[k - 1] + t->prefix_len);
        end_mode(t);                     /* shows the prompt again */
        insert_text(t, choice, (int)strlen(choice));
        return;
    }
    if (enter || k > t->nmatches) {      /* not a valid number: start over */
        t->mode_len = 0;
        t->mode_buf[0] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Ctrl+R                                                               */
/* ------------------------------------------------------------------ */

static void search_key(Tab *t, int act, KeySym ks, const char *text, int n)
{
    if (ks == XK_Escape || act == A_INTERRUPT) {
        show(t, "Enter search term: ");
        show(t, t->mode_buf);
        show(t, "^C\n");
        end_mode(t);
        return;
    }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        char *result = malloc(1 << 16);
        history_search(t->mode_buf, result, 1 << 16);
        show(t, "Enter search term: ");
        show(t, t->mode_buf);
        show(t, "\n");
        show(t, result);
        free(result);
        end_mode(t);
        return;
    }
    if (ks == XK_BackSpace) {
        if (t->mode_len)
            t->mode_buf[--t->mode_len] = 0;
        return;
    }
    for (int k = 0; k < n; k++)
        if ((unsigned char)text[k] >= 32 && t->mode_len < (int)sizeof t->mode_buf - 1) {
            t->mode_buf[t->mode_len++] = text[k];
            t->mode_buf[t->mode_len] = 0;
        }
}

/* ------------------------------------------------------------------ */
/* Selection, copy and paste (X11 selections)                           */
/* ------------------------------------------------------------------ */

static void layout_tail(Tab *t);

/* Text of screen row v (rows of the buffer, then the prompt/input rows). */
static const char *vrow(Tab *t, int v, int *len)
{
    if (v < t->nrows - 1) {
        *len = (int)strlen(t->rows[v]);
        return t->rows[v];
    }
    int k = v - (t->nrows - 1);
    if (k < 0 || k >= ntail) {
        *len = 0;
        return "";
    }
    *len = tail_len[k];
    return tail + tail_start[k];
}

static char *selected_text(Tab *t)
{
    layout_tail(t);
    int r1 = sel_r1, c1 = sel_c1, r2 = sel_r2, c2 = sel_c2;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        r1 = sel_r2; c1 = sel_c2; r2 = sel_r1; c2 = sel_c1;
    }
    char *out = malloc((size_t)(r2 - r1 + 1) * (MAXCOL + 2) + 1);
    int n = 0;
    for (int v = r1; v <= r2; v++) {
        int len;
        const char *s = vrow(t, v, &len);
        int from = v == r1 ? c1 : 0, to = v == r2 ? c2 : len;
        if (to > len)
            to = len;
        if (from < to) {
            memcpy(out + n, s + from, to - from);
            n += to - from;
        }
        if (v < r2)
            out[n++] = '\n';
    }
    out[n] = 0;
    return out;
}

static void copy(void)
{
    if (active < 0)
        return;
    Tab *t = &tabs[active];
    char *text = sel_on ? selected_text(t) : strndup(t->input, t->len);
    free(clip_text);
    clip_text = text;
    XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
}

static void request_paste(Atom which)
{
    XConvertSelection(dpy, which, a_utf8, a_prop, win, CurrentTime);
}

/* Another program (or we ourselves) wants our selection. */
static void on_selection_request(XSelectionRequestEvent *rq)
{
    const char *text = rq->selection == a_clipboard ? clip_text : primary_text;
    Atom prop = rq->property != None ? rq->property : rq->target;
    XSelectionEvent reply;
    memset(&reply, 0, sizeof reply);
    reply.type = SelectionNotify;
    reply.display = dpy;
    reply.requestor = rq->requestor;
    reply.selection = rq->selection;
    reply.target = rq->target;
    reply.time = rq->time;
    reply.property = None;
    if (text && rq->target == a_targets) {
        Atom list[3] = { a_targets, a_utf8, XA_STRING };
        XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)list, 3);
        reply.property = prop;
    } else if (text && (rq->target == a_utf8 || rq->target == XA_STRING)) {
        XChangeProperty(dpy, rq->requestor, prop, rq->target, 8, PropModeReplace,
                        (const unsigned char *)text, (int)strlen(text));
        reply.property = prop;
    }
    XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&reply);
}

/* The text we asked for (paste) has arrived. */
static void on_selection_notify(XSelectionEvent *e)
{
    if (e->property == None) {
        if (e->target == a_utf8)               /* try plain STRING instead */
            XConvertSelection(dpy, e->selection, XA_STRING, a_prop, win, CurrentTime);
        return;
    }
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, a_prop, 0, 1 << 20, True, AnyPropertyType,
                           &type, &format, &n, &after, &data) == Success && data) {
        if (active >= 0 && format == 8 && tabs[active].mode == MODE_NORMAL)
            insert_text(&tabs[active], (char *)data, (int)n);
        XFree(data);
    }
    dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
/* ------------------------------------------------------------------ */

/* Cut "current row + typed text" into screen rows and find the cursor. */
static void layout_tail(Tab *t)
{
    const char *partial = t->rows[t->nrows - 1];
    int n, cursor;
    if (t->mode == MODE_NORMAL) {
        n = snprintf(tail, sizeof tail, "%s", partial);
        memcpy(tail + n, t->input, t->len);
        cursor = n + t->cur;
        n += t->len;
    } else {
        const char *label = t->mode == MODE_SEARCH ? "Enter search term: " : "Choose a number: ";
        n = snprintf(tail, sizeof tail, "%s%s%s", partial, label, t->mode_buf);
        cursor = n;
    }
    tail[n] = 0;
    ntail = 0;
    int start = 0, col = 0;
    cur_row = cur_col = 0;
    for (int i = 0; i <= n && ntail < TAIL_MAX - 1; i++) {
        if (col == cols && ((i < n && tail[i] != '\n') || i == cursor)) {
            tail_start[ntail] = start;
            tail_len[ntail++] = i - start;
            start = i;
            col = 0;
        }
        if (i == cursor) {
            cur_row = ntail;
            cur_col = col;
        }
        if (i == n)
            break;
        if (tail[i] == '\n') {
            tail_start[ntail] = start;
            tail_len[ntail++] = i - start;
            start = i + 1;
            col = 0;
        } else {
            col++;
        }
    }
    tail_start[ntail] = start;
    tail_len[ntail++] = n - start;
}

static void tab_title(Tab *t, char *out, int size)
{
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(t->cwd, home, hl) && (t->cwd[hl] == '/' || !t->cwd[hl]))
        snprintf(out, size, "~%s", t->cwd + hl);
    else
        snprintf(out, size, "%s", t->cwd);
    int len = (int)strlen(out);
    if (len > 22) {                              /* keep the end of long paths */
        memmove(out + 3, out + len - 19, 20);
        memcpy(out, "...", 3);
    }
}

static void draw_bar(void)
{
    XSetForeground(dpy, gc, c_bar);
    XFillRectangle(dpy, pix, gc, 0, 0, W, bar_h);
    int x = PAD, n = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        tab_x0[i] = tab_x1[i] = -1;
        if (!visible(i))
            continue;
        char title[64], label[96];
        tab_title(&tabs[i], title, sizeof title);
        snprintf(label, sizeof label, "[ %d: %s ]", ++n, title);
        int w = (int)strlen(label) * cw;
        if (i == active) {
            XSetForeground(dpy, gc, c_active);
            XFillRectangle(dpy, pix, gc, x - 2, 2, w + 4, bar_h - 4);
        }
        XSetForeground(dpy, gc, i == active ? c_fg : c_bar_text);
        XDrawString(dpy, pix, gc, x, (bar_h - ch) / 2 + ascent, label, (int)strlen(label));
        tab_x0[i] = x;
        tab_x1[i] = x + w;
        x += w + cw;
    }
    XSetForeground(dpy, gc, c_bar_text);
    XDrawString(dpy, pix, gc, x, (bar_h - ch) / 2 + ascent, "[ + ]", 5);
    plus_x0 = x;
    plus_x1 = x + 5 * cw;
}

static void draw(void)
{
    XSetForeground(dpy, gc, c_bg);
    XFillRectangle(dpy, pix, gc, 0, 0, W, H);
    draw_bar();
    if (active >= 0) {
        Tab *t = &tabs[active];
        layout_tail(t);
        int total = t->nrows - 1 + ntail;
        int max_scroll = total > rows_vis ? total - rows_vis : 0;
        if (t->scroll > max_scroll)
            t->scroll = max_scroll;
        if (t->scroll < 0)
            t->scroll = 0;
        int first = total - rows_vis - t->scroll;
        if (first < 0)
            first = 0;
        draw_first = first;

        int r1 = sel_r1, c1 = sel_c1, r2 = sel_r2, c2 = sel_c2;
        if (r1 > r2 || (r1 == r2 && c1 > c2)) {
            r1 = sel_r2; c1 = sel_c2; r2 = sel_r1; c2 = sel_c1;
        }
        for (int r = 0; r < rows_vis && first + r < total; r++) {
            int v = first + r, len;
            const char *s = vrow(t, v, &len);
            int y = text_top + r * ch;
            if (sel_on && v >= r1 && v <= r2) {
                int from = v == r1 ? c1 : 0, to = v == r2 ? c2 : len;
                if (to > from) {
                    XSetForeground(dpy, gc, c_sel);
                    XFillRectangle(dpy, pix, gc, PAD + from * cw, y, (to - from) * cw, ch);
                }
            }
            XSetForeground(dpy, gc, c_fg);
            XDrawString(dpy, pix, gc, PAD, y + ascent, s, len > cols ? cols : len);
        }
        int r = t->nrows - 1 + cur_row - first;           /* the cursor */
        if (r >= 0 && r < rows_vis) {
            int x = PAD + cur_col * cw, y = text_top + r * ch, len;
            const char *s = vrow(t, first + r, &len);
            XSetForeground(dpy, gc, c_cursor);
            XFillRectangle(dpy, pix, gc, x, y, cw, ch);
            if (cur_col < len) {
                XSetForeground(dpy, gc, c_bg);
                XDrawString(dpy, pix, gc, x, y + ascent, s + cur_col, 1);
            }
        }
    }
    XCopyArea(dpy, pix, win, gc, 0, 0, W, H, 0, 0);
}

static void resize(int w, int h)
{
    W = w;
    H = h;
    cols = (W - 2 * PAD) / cw;
    if (cols < 10) cols = 10;
    if (cols > MAXCOL) cols = MAXCOL;
    rows_vis = (H - text_top - 2) / ch;
    if (rows_vis < 1) rows_vis = 1;
    if (pix)
        XFreePixmap(dpy, pix);
    pix = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, DefaultScreen(dpy)));
    dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Keyboard and mouse                                                   */
/* ------------------------------------------------------------------ */

static void on_key(XKeyEvent *e)
{
    char text[32];
    KeySym ks;
    int n = XLookupString(e, text, sizeof text - 1, &ks, NULL);
    if (IsModifierKey(ks) || active < 0)
        return;
    KeySym base = XLookupKeysym(e, 0);
    unsigned mods = e->state & (ControlMask | ShiftMask | Mod1Mask);
    int act = keys_find(base, mods);
    Tab *t = &tabs[active];
    dirty = 1;

    switch (act) {                       /* keys that work in every mode */
    case A_NEW_TAB:   open_tab(t->cwd); return;
    case A_CLOSE_TAB: close_tab(active); return;
    case A_NEXT_TAB:  switch_tab(1); return;
    case A_PREV_TAB:  switch_tab(-1); return;
    case A_COPY:      copy(); return;
    case A_PASTE:     request_paste(a_clipboard); return;
    }
    if ((mods & Mod1Mask) && base >= XK_1 && base <= XK_9) {
        goto_tab((int)(base - XK_1));
        return;
    }
    if (mods == ShiftMask && (base == XK_Page_Up || base == XK_Page_Down)) {
        t->scroll += (base == XK_Page_Up ? 1 : -1) * (rows_vis - 1);
        return;
    }
    t->scroll = 0;
    if (t->mode == MODE_SEARCH) {
        search_key(t, act, ks, text, n);
        return;
    }
    if (t->mode == MODE_PICK) {
        pick_key(t, act, ks, text, n);
        return;
    }

    switch (act) {
    case A_LEFT:       if (t->cur > 0) t->cur--; return;
    case A_RIGHT:      if (t->cur < t->len) t->cur++; return;
    case A_LINE_START: t->cur = 0; return;
    case A_LINE_END:   t->cur = t->len; return;
    case A_HIST_PREV:  history_move(t, -1); return;
    case A_HIST_NEXT:  history_move(t, 1); return;
    case A_KILL_START: delete_range(t, 0, t->cur); return;
    case A_KILL_END:   delete_range(t, t->cur, t->len); return;
    case A_KILL_WORD: {
        int k = t->cur;
        while (k > 0 && t->input[k - 1] == ' ') k--;
        while (k > 0 && t->input[k - 1] != ' ') k--;
        delete_range(t, k, t->cur);
        return;
    }
    case A_CLEAR:      clear_tab(t, 1); return;
    case A_EOF:
        if (t->len == 0)
            send_msg(t, MSG_EOF, "", 0);   /* close the tab, or EOF to a program */
        else
            delete_range(t, t->cur, t->cur + 1);
        return;
    case A_INTERRUPT:  send_key(t, "^C\n", MSG_INT); return;
    case A_SUSPEND:    send_key(t, "^Z\n", MSG_STOP); return;
    case A_SEARCH:     begin_mode(t, MODE_SEARCH); set_input(t, ""); return;
    case A_COMPLETE:   complete(t); return;
    }

    if (ks == XK_Return || ks == XK_KP_Enter)
        submit(t);
    else if (ks == XK_BackSpace)
        delete_range(t, t->cur - 1, t->cur);
    else if (ks == XK_Delete || ks == XK_KP_Delete)
        delete_range(t, t->cur, t->cur + 1);
    else if (ks == XK_Home)
        t->cur = 0;
    else if (ks == XK_End)
        t->cur = t->len;
    else if (n > 0 && !(mods & ControlMask))
        insert_text(t, text, n);
}

static int row_at(int y) { return draw_first + (y - text_top) / ch; }

static int col_at(int x)
{
    int c = (x - PAD + cw / 2) / cw;
    return c < 0 ? 0 : c > cols ? cols : c;
}

static void on_button(XButtonEvent *b)
{
    if (b->y < bar_h) {                                  /* the tab bar */
        for (int i = 0; i < MAX_TABS; i++)
            if (tab_x0[i] >= 0 && b->x >= tab_x0[i] && b->x < tab_x1[i]) {
                if (b->button == Button1)
                    set_active(i);
                else if (b->button == Button2 || b->button == Button3)
                    close_tab(i);                        /* middle/right click */
                return;
            }
        if (b->button == Button1 && b->x >= plus_x0 && b->x < plus_x1)
            open_tab(active >= 0 ? tabs[active].cwd : "/");
        return;
    }
    if (active < 0)
        return;
    Tab *t = &tabs[active];
    if (b->button == Button4)
        t->scroll += 3;
    else if (b->button == Button5)
        t->scroll -= 3;
    else if (b->button == Button2)
        request_paste(XA_PRIMARY);
    else if (b->button == Button1) {
        sel_drag = 1;
        sel_on = 0;
        sel_r1 = sel_r2 = row_at(b->y);
        sel_c1 = sel_c2 = col_at(b->x);
    }
    dirty = 1;
}

static void on_motion(XMotionEvent *m)
{
    if (!sel_drag)
        return;
    sel_r2 = row_at(m->y);
    sel_c2 = col_at(m->x);
    sel_on = sel_r1 != sel_r2 || sel_c1 != sel_c2;
    dirty = 1;
}

static void on_release(XButtonEvent *b)
{
    if (b->button != Button1 || !sel_drag)
        return;
    sel_drag = 0;
    if (sel_on && active >= 0) {             /* selecting = PRIMARY, like xterm */
        free(primary_text);
        primary_text = selected_text(&tabs[active]);
        XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
    }
}

/* ------------------------------------------------------------------ */
/* Setup and the event loop                                             */
/* ------------------------------------------------------------------ */

static unsigned long color(const char *name)
{
    XColor c;
    Colormap map = DefaultColormap(dpy, DefaultScreen(dpy));
    if (XParseColor(dpy, map, name, &c) && XAllocColor(dpy, map, &c))
        return c.pixel;
    return WhitePixel(dpy, DefaultScreen(dpy));
}

static void handle_event(XEvent *ev)
{
    switch (ev->type) {
    case Expose:           dirty = 1; break;
    case KeyPress:         on_key(&ev->xkey); break;
    case ButtonPress:      on_button(&ev->xbutton); break;
    case ButtonRelease:    on_release(&ev->xbutton); break;
    case MotionNotify:     on_motion(&ev->xmotion); break;
    case SelectionRequest: on_selection_request(&ev->xselectionrequest); break;
    case SelectionNotify:  on_selection_notify(&ev->xselection); break;
    case ConfigureNotify:
        if (ev->xconfigure.width != W || ev->xconfigure.height != H)
            resize(ev->xconfigure.width, ev->xconfigure.height);
        break;
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == a_delete)
            quit();
        break;
    }
}

void gui_run(int pmode)
{
    process_mode = pmode;
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr,
                "terminator: cannot open the X display.\n"
                "On macOS: install XQuartz (https://www.xquartz.org), log out and\n"
                "back in, then run ./terminator again from Terminal.\n");
        exit(1);
    }
    const char *fname = getenv("TERMINATOR_FONT");
    font = XLoadQueryFont(dpy, fname ? fname : "9x15");
    if (!font)
        font = XLoadQueryFont(dpy, "fixed");
    if (!font) {
        fprintf(stderr, "terminator: no usable X font\n");
        exit(1);
    }
    cw = font->max_bounds.width;
    ascent = font->ascent;
    ch = font->ascent + font->descent;
    bar_h = ch + 10;
    text_top = bar_h + 4;

    c_bg = color("#1e1f22");
    c_fg = color("#e6e6e6");
    c_bar = color("#111214");
    c_bar_text = color("#9a9ea6");
    c_active = color("#3a3d44");
    c_cursor = color("#e8b04a");
    c_sel = color("#3d5a80");

    int scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0, c_bg, c_bg);
    XStoreName(dpy, win, "Terminator");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                               Button1MotionMask | StructureNotifyMask);
    a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_delete, 1);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_prop = XInternAtom(dpy, "TERMINATOR_PASTE", False);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    resize(W, H);
    XMapWindow(dpy, win);

    char report[2048];
    keys_load(report, sizeof report);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, "/");
    open_tab(cwd);
    if (active < 0) {
        fprintf(stderr, "terminator: could not start a shell\n");
        exit(1);
    }
    if (report[0])
        show(&tabs[active], report);

    for (;;) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            handle_event(&ev);
        }
        if (dirty) {
            draw();
            dirty = 0;
        }
        if (XPending(dpy))                /* drawing may have queued events */
            continue;
        XFlush(dpy);

        /* Sleep until X or any tab has something for us. */
        struct pollfd pf[MAX_TABS + 1];
        int who[MAX_TABS + 1], np = 0;
        pf[np].fd = ConnectionNumber(dpy);
        pf[np].events = POLLIN;
        who[np++] = -1;
        for (int i = 0; i < MAX_TABS; i++)
            if (tabs[i].used) {
                pf[np].fd = tabs[i].from_shell;
                pf[np].events = POLLIN;
                who[np++] = i;
            }
        if (poll(pf, np, -1) < 0)
            continue;                     /* interrupted by SIGCHLD */
        for (int k = 1; k < np; k++) {
            if (!pf[k].revents)
                continue;
            Tab *t = &tabs[who[k]];
            char buf[8192];
            ssize_t r = read(t->from_shell, buf, sizeof buf);
            if (r > 0)
                feed(t, buf, (int)r);
            else if (r == 0 || errno != EINTR)
                tab_gone(who[k]);         /* shell exited (or crashed) */
        }
    }
}
