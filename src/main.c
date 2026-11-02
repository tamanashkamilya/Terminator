/*
 * main.c - start Terminator.
 *
 *   ./terminator                    thread mode: every tab is a thread
 *   ./terminator useProcess=True    process mode: every tab is a process
 *   ./terminator --test             run tab shells without a window
 *                                   (used by tests/run_tests.sh)
 */
#include "terminator.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

int main(int argc, char **argv)
{
    int process_mode = 0, test = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncasecmp(argv[i], "useProcess=", 11)) {
            const char *v = argv[i] + 11;
            process_mode = !strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "yes");
        } else if (!strcmp(argv[i], "--test")) {
            test = 1;
        } else {
            fprintf(stderr, "usage: %s [useProcess=True|False]\n", argv[0]);
            return 2;
        }
    }
    shell_signals();                /* SIGCHLD handler, ignore SIGPIPE */
    history_init(process_mode);     /* shared memory + lock, before any tab */
    if (test) {
        int r = test_run(process_mode);
        history_cleanup();
        return r;
    }
    gui_run(process_mode);          /* never returns */
    return 0;
}
