/*
 * history.c - one command history, shared by every tab.
 *
 * The list lives in shared memory (mmap MAP_SHARED | MAP_ANON) made before
 * any tab exists, so all tabs see the same bytes - threads because they
 * share the process, tab processes because they inherit the mapping.
 *
 * Locking:
 *   thread mode   a pthread mutex
 *   process mode  a System V semaphore. It works across processes, and with
 *                 SEM_UNDO the kernel releases it if a tab process crashes
 *                 while holding it, so one crash can't freeze the others.
 *                 (macOS has no process-shared sem_init.)
 *
 * Every command is also appended to ~/.terminator_history, which keeps
 * the last 10,000 commands between runs (like ~/.bash_history).
 */
#include "terminator.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <unistd.h>

typedef struct {
    int start, count;                 /* a ring buffer of HIST_MAX commands */
    char cmd[HIST_MAX][HIST_LEN];
} History;

static History *hist;
static int semid = -1;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static char file[1024];

static void lock(void)
{
    if (semid >= 0) {
        struct sembuf op = { 0, -1, SEM_UNDO };
        while (semop(semid, &op, 1) < 0 && errno == EINTR) {
        }
    } else {
        pthread_mutex_lock(&mutex);
    }
}

static void unlock(void)
{
    if (semid >= 0) {
        struct sembuf op = { 0, 1, SEM_UNDO };
        while (semop(semid, &op, 1) < 0 && errno == EINTR) {
        }
    } else {
        pthread_mutex_unlock(&mutex);
    }
}

/* Thread mode: a tab thread may fork() while another thread holds the
 * mutex. Taking it around fork() means the child never starts with it
 * locked by a thread that doesn't exist in the child. */
static void before_fork(void) { pthread_mutex_lock(&mutex); }
static void after_fork(void) { pthread_mutex_unlock(&mutex); }

static const char *entry(int i) { return hist->cmd[(hist->start + i) % HIST_MAX]; }

static void add_entry(const char *cmd)            /* caller holds the lock */
{
    int slot = (hist->start + hist->count) % HIST_MAX;
    if (hist->count == HIST_MAX)
        hist->start = (hist->start + 1) % HIST_MAX;  /* forget the oldest */
    else
        hist->count++;
    snprintf(hist->cmd[slot], HIST_LEN, "%s", cmd);
}

/* One command per line in the file; newlines inside a multi-line
 * command are stored as the byte 0x1f. */
static void save_line(int fd, const char *cmd)
{
    char buf[HIST_LEN + 1];
    int n = 0;
    for (; cmd[n] && n < HIST_LEN - 1; n++)
        buf[n] = cmd[n] == '\n' ? 0x1f : cmd[n];
    buf[n++] = '\n';
    write_all(fd, buf, n);
}

void history_init(int process_mode)
{
    hist = mmap(NULL, sizeof *hist, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (hist == MAP_FAILED) {
        perror("terminator: mmap");
        exit(1);
    }
    if (process_mode) {
        semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
        if (semid < 0) {
            perror("terminator: semget");
            exit(1);
        }
        union { int val; void *buf; unsigned short *array; } arg;
        arg.val = 1;                                  /* 1 = unlocked */
        semctl(semid, 0, SETVAL, arg);
    } else {
        pthread_atfork(before_fork, after_fork, after_fork);
    }

    const char *home = getenv("HOME");
    snprintf(file, sizeof file, "%s/.terminator_history", home ? home : ".");
    FILE *f = fopen(file, "r");
    int lines = 0;
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, f)) > 0) {
            if (line[n - 1] == '\n')
                line[--n] = 0;
            for (ssize_t k = 0; k < n; k++)
                if (line[k] == 0x1f)
                    line[k] = '\n';
            if (n) {
                add_entry(line);
                lines++;
            }
        }
        free(line);
        fclose(f);
    }
    if (lines > HIST_MAX) {                 /* trim the file to 10,000 */
        int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            for (int i = 0; i < hist->count; i++)
                save_line(fd, entry(i));
            close(fd);
        }
    }
}

void history_cleanup(void)
{
    if (semid >= 0)
        semctl(semid, 0, IPC_RMID);
}

void history_add(const char *cmd)
{
    if (!hist || !*cmd)
        return;
    lock();
    add_entry(cmd);
    int fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        save_line(fd, cmd);
        close(fd);
    }
    unlock();
}

int history_count(void)
{
    lock();
    int n = hist->count;
    unlock();
    return n;
}

void history_get(int i, char *buf, int size)
{
    lock();
    snprintf(buf, size, "%s", (i >= 0 && i < hist->count) ? entry(i) : "");
    unlock();
}

/* Length of the longest common substring of a and b (dynamic programming:
 * cur[j] = length of the common run ending at b[i] and a[j-1]). */
static int common_substring(const char *a, const char *b)
{
    int m = (int)strlen(a), prev[65] = { 0 }, cur[65], best = 0;
    if (m > 64)
        m = 64;
    for (; *b; b++) {
        cur[0] = 0;
        for (int j = 1; j <= m; j++) {
            cur[j] = (*b == a[j - 1]) ? prev[j - 1] + 1 : 0;
            if (cur[j] > best)
                best = cur[j];
        }
        memcpy(prev, cur, sizeof(int) * (m + 1));
    }
    return best;
}

/* Ctrl+R:
 *  1. the most recent command equal to term, else
 *  2. the commands sharing the longest common substring with term
 *     (newest first), if that is longer than 2 characters, else
 *  3. "No match for search term in history". */
void history_search(const char *term, char *out, int size)
{
    out[0] = 0;
    lock();
    int n = hist->count;
    for (int i = n - 1; i >= 0; i--)
        if (!strcmp(entry(i), term)) {
            snprintf(out, size, "%s\n", term);
            unlock();
            return;
        }
    int *len = malloc(sizeof(int) * (n + 1)), best = 0;
    for (int i = 0; i < n; i++) {
        len[i] = common_substring(term, entry(i));
        if (len[i] > best)
            best = len[i];
    }
    if (best > 2) {
        int used = 0, shown[50], nshown = 0;
        for (int i = n - 1; i >= 0 && nshown < 50 && used < size - 1; i--) {
            if (len[i] != best)
                continue;
            int dup = 0;
            for (int k = 0; k < nshown; k++)
                if (!strcmp(entry(shown[k]), entry(i)))
                    dup = 1;
            if (dup)
                continue;
            used += snprintf(out + used, size - used, "%s\n", entry(i));
            shown[nshown++] = i;
        }
    } else {
        snprintf(out, size, "No match for search term in history\n");
    }
    free(len);
    unlock();
}
