#include <stdio.h>

/* Prints its arguments, then echoes stdin back (so ./myprog < somefile.txt works).
 * fflush(stdout) makes the text appear immediately: Terminator gives programs
 * a pipe, not a terminal, so printf output is otherwise held in a buffer. */
int main(int argc, char **argv)
{
    printf("hello from myprog (%d args)\n", argc - 1);
    for (int i = 1; i < argc; i++)
        printf("  arg %d: %s\n", i, argv[i]);
    fflush(stdout);

    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        printf("read: %s", line);
        fflush(stdout);
    }
    return 0;
}
