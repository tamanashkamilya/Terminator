/*
 * terminator.h - what the parts of Terminator need to know about each other.
 *
 *   main.c        reads the command line, starts everything
 *   gui.c         the X11 window: tabs, text, keyboard, mouse, clipboard
 *   keys.c        hotkeys and ~/.terminatorrc
 *   history.c     the command history shared by all tabs
 *   shell.c       one shell per tab: main loop, jobs, $(...)
 *   parse.c       quotes, words, tokens and $VAR / $(...) expansion
 *   exec.c        running commands: lists, if/fi, pipes, redirection, scripts
 *   builtins.c    cd, exit, history, jobs, fg, bg, source, test, multiWatch ...
 *   testdriver.c  "./terminator --test": drives tabs without a window
 */
#ifndef TERMINATOR_H
#define TERMINATOR_H

#include <sys/types.h>

#define MAX_TABS 16
#define HIST_MAX 10000      /* commands kept in the history */
#define HIST_LEN 512        /* longest command kept in the history */

/* Messages from the GUI to a tab's shell. Each one is
 * [type: 1 byte][length: 4 bytes][data: length bytes]. */
enum { MSG_LINE = 'L', MSG_INT = 'C', MSG_STOP = 'Z', MSG_EOF = 'D' };

/* history.c */
void history_init(int process_mode);
void history_cleanup(void);
void history_add(const char *cmd);
int  history_count(void);
void history_get(int i, char *buf, int size);      /* i = 0 is the oldest */
void history_search(const char *term, char *out, int size);

/* keys.c: the actions a key can be bound to */
enum {
    A_LEFT, A_RIGHT, A_LINE_START, A_LINE_END, A_HIST_PREV, A_HIST_NEXT,
    A_KILL_START, A_KILL_END, A_KILL_WORD, A_CLEAR, A_EOF, A_COPY, A_PASTE,
    A_INTERRUPT, A_SUSPEND, A_SEARCH, A_COMPLETE,
    A_NEW_TAB, A_CLOSE_TAB, A_NEXT_TAB, A_PREV_TAB,
    A_COUNT
};
void keys_load(char *report, int size);
int  keys_find(unsigned long keysym, unsigned int mods);   /* -1 = no action */

/* shell.c */
void  shell_signals(void);
pid_t shell_spawn(int slot, const char *cwd, int process_mode,
                  int *to_shell, int *from_shell,
                  const int *close_in_child, int nclose);
void  write_all(int fd, const void *buf, int n);

/* gui.c and testdriver.c */
void gui_run(int process_mode);
int  test_run(int process_mode);

#endif
