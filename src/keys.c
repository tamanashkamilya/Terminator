/*
 * keys.c - hotkeys, and how the user changes them.
 *
 * Every action has a default key (the table below). ~/.terminatorrc can
 * change them, one per line:
 *
 *     # ~/.terminatorrc
 *     bind clear-screen Ctrl+Shift+L
 *     bind delete-word  Alt+Backspace
 *
 * The file is read at startup, so changes need no recompiling and stay
 * after a restart. An unknown action, a key name we can't understand, or a
 * key already used by another action is reported in the first tab and the
 * action keeps its default key.
 */
#include "terminator.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *name;       /* name used in ~/.terminatorrc */
    const char *def;        /* default key */
    KeySym sym;             /* the key now in use */
    unsigned mods;          /* ControlMask | ShiftMask | Mod1Mask */
    int custom;             /* set from the file */
    char text[64];          /* the key as the user wrote it */
} Binding;

static Binding keys[A_COUNT] = {
    [A_LEFT]       = { "cursor-left",     "Left" },
    [A_RIGHT]      = { "cursor-right",    "Right" },
    [A_LINE_START] = { "line-start",      "Ctrl+A" },
    [A_LINE_END]   = { "line-end",        "Ctrl+E" },
    [A_HIST_PREV]  = { "history-prev",    "Up" },
    [A_HIST_NEXT]  = { "history-next",    "Down" },
    [A_KILL_START] = { "delete-to-start", "Ctrl+U" },
    [A_KILL_END]   = { "delete-to-end",   "Ctrl+K" },
    [A_KILL_WORD]  = { "delete-word",     "Ctrl+W" },
    [A_CLEAR]      = { "clear-screen",    "Ctrl+L" },
    [A_EOF]        = { "eof",             "Ctrl+D" },
    [A_COPY]       = { "copy",            "Ctrl+Shift+C" },
    [A_PASTE]      = { "paste",           "Ctrl+Shift+V" },
    [A_INTERRUPT]  = { "interrupt",       "Ctrl+C" },
    [A_SUSPEND]    = { "suspend",         "Ctrl+Z" },
    [A_SEARCH]     = { "history-search",  "Ctrl+R" },
    [A_COMPLETE]   = { "complete",        "Tab" },
    [A_NEW_TAB]    = { "new-tab",         "Ctrl+T" },
    [A_CLOSE_TAB]  = { "close-tab",       "Ctrl+Shift+W" },
    [A_NEXT_TAB]   = { "next-tab",        "Ctrl+Page_Down" },
    [A_PREV_TAB]   = { "prev-tab",        "Ctrl+Page_Up" },
};

/* Keys with a fixed meaning that can't be given to an action. */
static const char *reserved[] = { "Return", "BackSpace", "Delete", "Home", "End",
                                  "Escape", "Shift+Page_Up", "Shift+Page_Down" };

/* "Ctrl+Shift+C" -> keysym 'c' + ControlMask|ShiftMask. 0 if invalid. */
static int parse_key(const char *spec, KeySym *sym, unsigned *mods)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%s", spec);
    char *p = buf, *plus;
    *mods = 0;
    while ((plus = strchr(p, '+')) && plus[1]) {
        *plus = 0;
        if (!strcasecmp(p, "ctrl") || !strcasecmp(p, "control"))
            *mods |= ControlMask;
        else if (!strcasecmp(p, "shift"))
            *mods |= ShiftMask;
        else if (!strcasecmp(p, "alt") || !strcasecmp(p, "meta") || !strcasecmp(p, "option"))
            *mods |= Mod1Mask;
        else
            return 0;
        p = plus + 1;
    }
    if (!*p)
        return 0;
    if (strlen(p) == 1) {                      /* a letter, digit or symbol */
        *sym = tolower((unsigned char)*p);
        return isgraph((unsigned char)*p);
    }
    static const char *alias[][2] = {
        { "PageUp", "Page_Up" }, { "PgUp", "Page_Up" }, { "PageDown", "Page_Down" },
        { "PgDn", "Page_Down" }, { "Esc", "Escape" }, { "Enter", "Return" },
        { "Space", "space" }, { "Backspace", "BackSpace" }, { "Del", "Delete" },
    };
    for (unsigned k = 0; k < sizeof alias / sizeof alias[0]; k++)
        if (!strcasecmp(p, alias[k][0]))
            p = (char *)alias[k][1];
    *sym = XStringToKeysym(p);
    if (*sym == NoSymbol) {                    /* "left" -> "Left" */
        char cap[64];
        snprintf(cap, sizeof cap, "%s", p);
        cap[0] = (char)toupper((unsigned char)cap[0]);
        *sym = XStringToKeysym(cap);
    }
    if (*sym >= 'A' && *sym <= 'Z')
        *sym += 32;
    return *sym != NoSymbol;
}

static void report(char *out, int size, const char *fmt, ...)
{
    int used = (int)strlen(out);
    if (used >= size - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + used, size - used, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s", out + used);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static int is_reserved(KeySym sym, unsigned mods)
{
    for (unsigned k = 0; k < sizeof reserved / sizeof reserved[0]; k++) {
        KeySym s;
        unsigned m;
        if (parse_key(reserved[k], &s, &m) && s == sym && m == mods)
            return 1;
    }
    return 0;
}

void keys_load(char *out, int size)
{
    out[0] = 0;
    for (int a = 0; a < A_COUNT; a++) {
        parse_key(keys[a].def, &keys[a].sym, &keys[a].mods);
        keys[a].custom = 0;
    }
    const char *home = getenv("HOME");
    char path[1024];
    snprintf(path, sizeof path, "%s/.terminatorrc", home ? home : ".");
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    int no = 0;
    while (fgets(line, sizeof line, f)) {
        no++;
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        char word[64], name[64], key[64];
        if (sscanf(s, "%63s %63s %63s", word, name, key) != 3 || strcmp(word, "bind")) {
            report(out, size, "~/.terminatorrc line %d: expected 'bind ACTION KEY'\n", no);
            continue;
        }
        int a = 0;
        while (a < A_COUNT && strcmp(keys[a].name, name))
            a++;
        if (a == A_COUNT) {
            report(out, size, "~/.terminatorrc line %d: unknown action '%s'\n", no, name);
            continue;
        }
        KeySym sym;
        unsigned mods;
        if (!parse_key(key, &sym, &mods)) {
            report(out, size, "~/.terminatorrc line %d: invalid key '%s' for %s, using %s\n",
                   no, key, name, keys[a].def);
            continue;
        }
        if (is_reserved(sym, mods)) {
            report(out, size, "~/.terminatorrc line %d: %s is reserved, %s stays on %s\n",
                   no, key, name, keys[a].def);
            continue;
        }
        keys[a].sym = sym;
        keys[a].mods = mods;
        keys[a].custom = 1;
        snprintf(keys[a].text, sizeof keys[a].text, "%s", key);
    }
    fclose(f);

    /* Two actions on one key: the custom one goes back to its default.
     * Repeat until nothing clashes (defaults never clash with each other). */
    for (int again = 1; again;) {
        again = 0;
        for (int a = 0; a < A_COUNT; a++)
            for (int b = 0; b < A_COUNT; b++)
                if (a != b && keys[a].custom && keys[a].sym == keys[b].sym &&
                    keys[a].mods == keys[b].mods) {
                    report(out, size, "~/.terminatorrc: %s for %s conflicts with %s, using %s\n",
                           keys[a].text, keys[a].name, keys[b].name, keys[a].def);
                    parse_key(keys[a].def, &keys[a].sym, &keys[a].mods);
                    keys[a].custom = 0;
                    again = 1;
                }
    }
}

int keys_find(unsigned long sym, unsigned int mods)
{
    if (sym >= 'A' && sym <= 'Z')
        sym += 32;
    for (int a = 0; a < A_COUNT; a++)
        if (keys[a].sym == sym && keys[a].mods == mods)
            return a;
    return -1;
}
