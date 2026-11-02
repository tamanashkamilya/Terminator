# Terminator: design

## 1. The big picture

```
 +--------------------------- GUI (gui.c, the only Xlib code) ---------------------------+
 | window, tab bar, one text buffer per tab, line editor, Tab completion, Ctrl+R, clipboard |
 | sleeps in poll() on: the X connection + every tab's output pipe                         |
 +----------+------------------------------------+-----------------------------------------+
            | messages: line, Ctrl+C, Ctrl+Z,    ^ plain text (prompt, command output,
            | Ctrl+D                             |  "ESC]7;dir BEL" = the tab's directory)
            v                                    |
 +----------+------------------------------------+-----------------------------------------+
 | a tab's shell (shell.c): same code in both modes - a THREAD or a forked PROCESS          |
 |   read line -> complete? (parse.c) -> history_add -> run_text (exec.c)                   |
 |   run_text: statements, if/elif/else/fi -> && || -> pipelines -> fork + dup2 + execvp    |
 +---------------------------------------------------------------------------------------+
                children: stdout and stderr are dup2()'d onto the tab's output pipe
```

The shell engine doesn't care where its text comes from: the GUI, a script
file or the test driver all feed it the same way.

## 2. GUI and event loop (gui.c)

* `XOpenDisplay`, `XCreateSimpleWindow` and `XSelectInput` set up the window.
  Text is drawn with `XDrawString` into an off-screen `Pixmap`, which is then
  copied to the window with `XCopyArea`, so the window doesn't flicker.
* The loop:
  1. Handle every queued X event (`XPending` / `XNextEvent`).
  2. Redraw once if anything changed.
  3. Sleep in `poll()` on the X connection's file descriptor and on every
     tab's output pipe.

  Nothing spins, so an idle Terminator uses 0% CPU. Hidden tabs are read
  too, so they keep running.
* **Per-tab buffer:** an array of rows, 5000 rows of scrollback, each up to
  400 characters. Output is wrapped at the window width as it arrives. The
  last row is the current line, so the prompt the shell printed is there.
  * `feed()` handles `\n \r \b \t`. It also understands `ESC[2J` (clear) and
    `ESC]7;dir BEL` (the shell's directory, used for the tab title and for
    completion). Other escape sequences, such as colours, are dropped.
* **Line editing happens in the GUI**, like a terminal's "cooked" mode. Keys
  edit `input`/`cur`. On Enter the GUI shows the line in the buffer and sends
  it to the shell.
  * The screen is drawn as: buffer rows, then "current row + typed text"
    wrapped into rows (`layout_tail`), with the cursor as a block.
* **Ctrl+R and the completion list** print below the prompt, then show the
  prompt again. They remember the current row (the prompt) and re-print it.
  Ctrl+R itself is computed in the GUI, which has direct access to the shared
  history.
* **Keys:** `XLookupString` turns a `KeyPress` into text plus a keysym.
  `XLookupKeysym(e, 0)` plus the Ctrl/Shift/Alt bits of `state` are looked up
  in the binding table (keys.c).
* **Copy/paste** use real X11 selections:
  * Dragging selects text and takes ownership of PRIMARY.
  * Ctrl+Shift+C takes ownership of CLIPBOARD; XQuartz copies it to the macOS
    pasteboard.
  * Requests from other programs are answered in `on_selection_request`
    (TARGETS, UTF8_STRING, STRING).
  * Paste calls `XConvertSelection`, and the text arrives in
    `on_selection_notify`.

## 3. Thread mode and process mode

`shell_spawn()` (shell.c) is the only place where the modes differ:

| | thread mode (default) | process mode (`useProcess=True`) |
|---|---|---|
| a tab is | `pthread_create` (detached, 8 MB stack; macOS's default 512 KB is too small) | `fork()` from the GUI. The child closes the X connection and the other tabs' pipes. |
| talks to GUI | two pipes | the same two pipes |
| history | same process memory | inherited `MAP_SHARED` mapping |
| history lock | `pthread_mutex` | System V semaphore with `SEM_UNDO` |
| a crash | kills the whole program | closes only that tab: the GUI sees EOF, reaps it, removes the tab |

**Per-tab state is the same in both modes.** A thread can't have its own
working directory or environment, so every tab keeps them in its `Shell`
struct, never calling `chdir()` or `setenv()` in the shared process:

* **Directory** (`sh->cwd`): `cd` only changes this string. Every child runs
  `chdir(sh->cwd)` between `fork()` and `exec`. Paths used inside the shell
  (`test -f`, `source`, globbing, a builtin's `> file`) are resolved against it.
* **Variables:** a per-tab table, copied from `environ` at start. A child gets
  the exported ones with `environ = make_env(sh)` just before `execvp`.
* **Jobs, `$?`, the half-typed multi-line command:** per tab.

### Shared state and its synchronisation (history.c)

* **History:** a ring buffer `cmd[10000][512]` in `mmap(MAP_SHARED|MAP_ANON)`
  memory. It is created in `main()` before any tab exists, so threads and
  forked tab processes all see the same bytes.
* **Lock:**
  * In process mode it is a System V semaphore (`semget`/`semop`). It works
    across processes, and `SEM_UNDO` releases it if a tab process dies while
    holding it, so one crash can't freeze the other tabs. (macOS doesn't
    support process-shared `sem_init`.)
  * In thread mode it is a mutex, with a `pthread_atfork` handler, so a child
    forked by one tab never starts with the mutex held by another thread.
* **File:** every command is appended to `~/.terminator_history` (`O_APPEND`,
  inside the lock). Newlines inside a multi-line command are stored as byte
  0x1F. At start-up the file is loaded and, if longer, trimmed to 10,000
  commands.
* **Clipboard:** the clipboard is GUI-only, so it needs no lock.

## 4. Processes and jobs (exec.c, shell.c)

* **Pipeline** `a | b | c`: N−1 `pipe()`s and N `fork()`s.
  * Each child runs `child_setup()`: reset the signals to default, `dup2` its
    input/output onto 0/1/2, close every other descriptor, then
    `chdir(sh->cwd)`. Then it applies its redirections (`open` + `dup2`) and
    runs a builtin, a script or `execvp`.
  * The parent closes each pipe end as soon as it has been handed over;
    otherwise a reader would never see EOF.
* **Process groups:** all processes of a pipeline share one group (the job).
  `setpgid` is called in both parent and child, to avoid a race.
* **Job table:** per tab, holding `pgid`, `pids`, done flags, the status of
  the last command, stopped/finished state, and the write end of the job's
  stdin pipe.
* **Waiting for a foreground job:** `wait_job()` sleeps in `poll()` on two
  things at once:
  * **the GUI pipe:**
    * Ctrl+C → `kill(-pgid, SIGINT)`
    * Ctrl+Z → `kill(-pgid, SIGTSTP)`
    * a typed line → written to the job's stdin pipe, so `cat` and
      `./a.out` can read input
    * Ctrl+D → closes that pipe (EOF)
  * **the tab's wake pipe** (SIGCHLD): it calls
    `waitpid(pid, WNOHANG|WUNTRACED|WCONTINUED)` for this tab's own pids only.
* **Stopped job:** `[n]+ Stopped cmd` is printed and the prompt returns.
  * `bg %n` sends `SIGCONT`.
  * `fg %n` sends `SIGCONT` and waits again.
  * `cmd &` gets `/dev/null` as stdin. Finished background jobs are announced
    (`[n]+ Done`) before the next prompt.
* **Builtins:** a builtin that is a whole command (`cd`, `exit`, `fg` ...) runs
  inside the shell. It writes to a file descriptor instead of `dup2`-ing over
  the shell's own descriptors, which in thread mode belong to every tab.
  * Inside a pipeline or with `&`, a builtin runs in a forked child, like in
    bash.
* **Command substitution** `$(...)` / backticks (`capture()`): fork a
  subshell (a copy of the shell, non-interactive), whose stdout is a pipe.
  * The parent reads the pipe while still listening for Ctrl+C.
  * The subshell expands its own words before running them, so the innermost
    substitution runs first.
  * Trailing newlines are removed.
* **multiWatch:** one subshell per command, each in its own process group,
  with stdout and stderr dup2'd onto its own pipe.
  * One `poll()` covers all the read ends plus the GUI pipe. Its timeout is
    the time until the next re-run, so there is no busy waiting.
  * Complete lines are printed as soon as they arrive, in the required block
    format. After EOF the command is reaped and started again 2 s later.
  * Ctrl+C makes the loop `kill(-pgid, SIGKILL)` every running command; the
    shell keeps running.
* **Scripts:** `./x.sh` (shebang mentions bash/sh, or a `.sh` file),
  `bash x.sh`, `sh x.sh` and `bash -c "..."` are run by our own `run_text()`
  in a forked child. `source x.sh` runs in the tab's shell. `$0..$n` are
  saved and restored around it.

## 5. IPC summary

| channel | between | how |
|---|---|---|
| message pipe | GUI → tab | framed `[type][len:4][data]`: `L` line, `C` Ctrl+C, `Z` Ctrl+Z, `D` Ctrl+D. Closing it closes the tab. |
| output pipe | tab and its children → GUI | raw bytes. Children get it as fd 1 and 2 through `dup2`. EOF means the tab is gone. |
| `ESC]7;dir BEL` | tab → GUI | in-band in the output pipe, before every prompt |
| job stdin pipe | tab → foreground job | typed lines while a job runs |
| pipeline pipes | child → child | `pipe` + `dup2` |
| wake pipe | SIGCHLD handler → tab | self-pipe trick |
| shared memory + semaphore/mutex | all tabs and the GUI | history |

## 6. Parsing (parse.c)

1. **Is the input complete?** `lex()` scans the input. The `skip_*` helpers
   jump over `'...'`, `"..."`, `` `...` ``, `$(...)` (nested) and `\x`. If one
   of them reaches the end of the text, the input is open.
   * The input is also incomplete if its last token is `|`, `&&` or `||`, or
     if `block_depth()` finds an `if` without its `fi`.
   * Then the shell prints `> ` and adds the next line, after a `\n`, to the
     collected text.
2. **Tokens:** words, which keep their quotes, and the operators
   `| || && & ; newline < > >> 2> 2>> 2>&1 &>`. `#` starts a comment.
   `\`+newline between words is skipped.
3. **Execution structure** (exec.c): statements are split at `; & newline`.
   * Keywords `if/then/elif/else/fi` at the start of a statement drive a
     stack of frames `{parent active, branch taken, branch active}`; this
     supports nesting.
   * Statements are split at `&& ||` into pipelines, and pipelines at `|`
     into commands with redirections.
   * The whole text is syntax-checked first (`block_depth`), so a script
     with a missing `fi` reports an error and runs nothing.
4. **Expansion** happens when a pipeline is about to run, so
   `cd x; echo $(pwd)` sees the new directory.
   * `'...'` is literal.
   * `"..."` expands `$VAR ${VAR} $? $$ $# $1 $@ $(...)` and backticks,
     without splitting.
   * Unquoted results are split on spaces, and unquoted `* ? [` are expanded
     with `glob()` against the tab's directory. A quoted `*` is escaped in the
     glob pattern.
   * `~` becomes `$HOME`. `NAME=value` sets a variable; `NAME=value cmd`
     exports it to that command only.

## 7. Data structures for history and completion

* **History:** a ring buffer (`start`, `count`, `cmd[10000][512]`) in
  shared memory.
  * ↑/↓ index it from the newest entry.
  * Ctrl+R first looks, newest first, for an exact match. Otherwise it
    computes the **longest common substring** with each entry (dynamic
    programming over two rows, O(len(term) × len(cmd))). It prints every
    entry with the best length (if that length is > 2), newest first and
    without duplicates.
* **Completion:** the word before the cursor is split into a directory part
  and a prefix.
  * `opendir`/`readdir` collect the matching names (directories get a `/`),
    which are sorted with `qsort`.
  * One match is inserted. With several, they are extended to their longest
    common prefix; if that adds nothing, the numbered list appears.
  * Picking mode collects digits: with fewer than 10 matches one digit picks,
    otherwise the user types the number and presses Enter.
* **Key bindings:** an array of `{name, default, keysym, modifiers}`, one per
  action, searched linearly per key press.

## 8. Signals

* **The Terminator process:**
  * ignores `SIGPIPE`, so writing to a closed pipe returns `EPIPE` instead of
    killing it;
  * installs a `SIGCHLD` handler (`SA_RESTART`).
* **SIGCHLD in thread mode:** all tabs share one process, so the handler may
  run in any thread. It only writes one byte into every tab's wake pipe (the
  self-pipe trick, which is async-signal-safe). Each tab then reaps only its
  own pids, so no tab can steal another tab's child. The wake pipes are
  non-blocking and never closed, so the handler can never block or write into
  a reused descriptor.
* **Keys, not a TTY:** Ctrl+C and Ctrl+Z reach Terminator as X11 key events,
  not as signals. The GUI forwards them as messages, and the tab's shell sends
  them with `kill(-pgid, ...)` to its own foreground job only. Terminator
  itself and other tabs' jobs are never signalled.
* **Children** reset `SIGINT SIGQUIT SIGTSTP SIGTTIN SIGTTOU SIGCHLD SIGPIPE
  SIGHUP` to their defaults before `exec`.
* **Closing a tab** sends `SIGHUP` + `SIGCONT` to its jobs, like bash.

## 9. Other choices

* **Forking from a multi-threaded process** (thread mode): the child
  immediately closes every descriptor above 2. Between `fork` and `exec` it
  avoids stdio and locks; the history mutex is covered by `pthread_atfork`.
  Scripts are read with `open`/`read`, not `fopen`.
* **Tab titles:** a tab's title is its directory, with `$HOME` shown as `~`.
  A new tab starts in the current tab's directory.
* **The test driver** (`./terminator --test`, testdriver.c) starts real tabs
  in either mode through `shell_spawn()` and "types" stdin into them.
  `tests/run_tests.sh` uses it to check tasks 2–6, 8, 9 and 11 automatically
  in both modes.
