#include <stdio.h>

/* A tiny program for trying Terminator: prints its arguments, then echoes
 * stdin back (so ./myprog < somefile.txt works). */
int main(int argc, char **argv)
{
    printf("hello from myprog (%d args)\n", argc - 1);
    for (int i = 1; i < argc; i++)
        printf("  arg %d: %s\n", i, argv[i]);
    char line[256];
    while (fgets(line, sizeof line, stdin))
        printf("read: %s", line);
    return 0;
}
