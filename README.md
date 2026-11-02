# Terminator

A terminal emulator **and** a shell, written in C for **macOS**.
Terminator draws its own X11 window (plain Xlib) with tabs. Every tab runs
its own small bash-like shell. That shell supports pipes, redirection,
quoting, multi-line input, `$(...)`, job control, a shared searchable
history, Tab completion, `multiWatch` and bash scripts (which Terminator
interprets itself).

```
[ 1: ~/Downloads/mydir ] [ 2: /tmp ] [ + ]
user@terminator> ls *.txt | wc -l
3
```

## Setup (once)

1. Install the compiler and `make`:
   ```sh
   xcode-select --install
   ```
2. Install **XQuartz**, the X11 window server for macOS:
   download it from <https://www.xquartz.org>, or run
   `brew install --cask xquartz`.
3. **Log out and log back in.** This sets `DISPLAY` for your session.

## Build and run

```sh
cd terminator
make                            # builds ./terminator
./terminator                    # thread mode (default): each tab is a thread
./terminator useProcess=True    # process mode: each tab is its own process
make test                       # automated tests in both modes (no window needed)
```

Running `./terminator` from Terminal.app starts XQuartz automatically and
opens the Terminator window. The first tab starts in the directory you
launched it from.

* If `make` says the X11 headers are missing, XQuartz is not installed.
  If you use Homebrew's `libx11` instead, run `make X11=$(brew --prefix)`.
* To use a different font: `TERMINATOR_FONT=10x20 ./terminator`
  (`xlsfonts` lists the available fonts).
* Optional: to use **Alt+1…9** to switch tabs, turn on *XQuartz → Settings →
  Input → "Option keys send Alt_L and Alt_R"*.

## Keys

All keys marked *(bindable)* can be changed in `~/.terminatorrc` (see below).

| Action | Default key |
|---|---|
| Move in the line | ← / → *(bindable)*, Home / End |
| Start / end of line | Ctrl+A / Ctrl+E *(bindable)* |
| Browse history (shared by all tabs) | ↑ / ↓ *(bindable)* |
| Delete to start / end of line | Ctrl+U / Ctrl+K *(bindable)* |
| Delete previous word | Ctrl+W *(bindable)* |
| Clear the screen | Ctrl+L *(bindable)* |
| EOF: close the tab at an empty prompt, or end a program's input | Ctrl+D *(bindable)* |
| Copy / paste (X11 CLIPBOARD) | Ctrl+Shift+C / Ctrl+Shift+V *(bindable)* |
| Stop / suspend the running command | Ctrl+C / Ctrl+Z *(bindable)* |
| Search history | Ctrl+R *(bindable)* |
| Complete a file name | Tab *(bindable)* |
| New tab | Ctrl+T *(bindable)*, or click `[ + ]` |
| Close tab | Ctrl+Shift+W *(bindable)*, middle- or right-click the tab, `exit`, or Ctrl+D |
| Next / previous tab | Ctrl+PageDown / Ctrl+PageUp *(bindable)* (on a MacBook: Ctrl+Fn+↓ / ↑), Alt+1…9, or click the tab |
| Scroll | mouse wheel / trackpad, Shift+PageUp / Shift+PageDown |
| Select text | drag with the mouse. The selection becomes the PRIMARY selection; middle-click pastes it. |

### Changing keys: `~/.terminatorrc`

```
# ~/.terminatorrc
bind clear-screen Ctrl+Shift+L
bind delete-word  Alt+Backspace
bind next-tab     Ctrl+Shift+Right
```

Actions: `cursor-left cursor-right line-start line-end history-prev
history-next delete-to-start delete-to-end delete-word clear-screen eof copy
paste interrupt suspend history-search complete new-tab close-tab next-tab
prev-tab`.

Keys are written as modifiers (`Ctrl`, `Shift`, `Alt`) plus a key name: a
single character, or an X key name such as `Left`, `Up`, `Tab`, `F5`,
`Page_Down`, `Backspace`, `Space`.

Terminator reads the file at startup, so changes need no recompiling and
survive restarts. Three kinds of mistake are reported at the top of the first
tab, and the action keeps its default key:
* an unknown action
* a key name Terminator doesn't understand
* a key that another action already uses, or a reserved key such as Return

`terminatorrc.example` is a sample file.

## Built-in commands

`cd [dir|-]`, `pwd`, `exit [n]`, `history`, `jobs`, `fg [%n]`, `bg [%n]`,
`source FILE [args]` (or `. FILE`), `multiWatch ["cmd1", "cmd2", ...]`,
`export NAME[=value]`, `unset NAME`, `echo [-n]`, `test` / `[ ... ]`,
`clear`, `help`.

Everything else runs as a child process with `fork()` + `execvp()`.

## What is implemented

| Task | |
|---|---|
| Two modes | Thread mode (default) and process mode (`useProcess=True`) behave the same. In both, tabs share the history, keep their own directory and jobs, and Ctrl+C only reaches the current tab's job. In process mode a crashing tab doesn't affect the others. |
| 1. X11 GUI and tabs | One text buffer per tab (5000 lines of scrollback). Tabs are drawn with `XDrawString` onto an off-screen pixmap, so the window doesn't flicker. Hidden tabs keep running. The event loop sleeps in `poll()` and uses 0% CPU when idle. |
| 2. Commands | Builtins, plus `fork`/`execvp`/`waitpid` for everything else. An unknown command prints `terminator: X: command not found`. `$?` holds the last exit status. |
| 3. Multi-line input | Shows the `> ` prompt after a trailing `\`, an open `'` or `"`, an open `$(` or backtick, a trailing `\|`, `&&` or `\|\|`, or an `if` without its `fi`. Supports `$(...)` and backticks, nested and spanning several lines; the innermost runs first. Quoting follows bash: single quotes are literal, double quotes expand `$VAR`. Also supports `~`, `*.txt` globbing, `;`, `&&`, `\|\|` and `#` comments. |
| 4. Redirection | `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`, `&>`, applied with `open` + `dup2` in the child. A missing input file is reported and the command doesn't run. Background jobs reading the same file each open it separately, so they don't interfere. |
| 5. Pipes | Pipelines of any length (N commands, N−1 pipes, one process group), combined with redirection. |
| 6. multiWatch | One process and one pipe per command, all read with `poll()`. Each piece of output is printed in the required `"cmd", <unix time>:` block format, and each command re-runs 2 s after it finishes. Ctrl+C kills every child. |
| 7. Hotkeys | Everything in the table above, configurable through `~/.terminatorrc`. |
| 8. Signals and jobs | Ctrl+C and Ctrl+Z are sent with `kill()` to the job's process group. Also `cmd &`, `jobs`, `fg %n`, `bg %n`, and "Done" notices. Terminator itself never receives these signals. |
| 9. History | 10,000 commands in `~/.terminator_history`, shared by all tabs. `history` shows the most recent 1,000. Ctrl+R prints the newest exact match, otherwise the commands sharing the longest common substring (if it is more than 2 characters long), otherwise `No match for search term in history`. |
| 10. Tab completion | Completes file names in the tab's directory, or in a directory written in the word (`src/ma`). One match is completed. Several are extended to their longest common prefix, then shown as a numbered list: type the number to pick (press Enter after it if there are 10 or more). |
| 11. Bash scripts | `./x.sh`, `bash x.sh` and `source x.sh` are interpreted by Terminator itself, not by `/bin/bash`. Scripts can use comments, variables, `$1`… `$#` `$?`, pipes, redirection, `$(...)`, nested `if/elif/else/fi` with `[ ]` tests (`-eq -ne -gt -ge -lt -le = != -z -n -f -d -e -r -w -x -s ! -a -o`) and `exit [n]`. Syntax errors such as a missing `fi` are reported and nothing runs. |

## Try it

```sh
cd demo
../terminator
```

Then, in the window:

```
gcc -o myprog myprog.c
./myprog
echo "Isn't process creation exciting?\
Fork, exec, repeat!"
echo "Files in $(basename $(pwd)): $(ls | \
wc -l)"
echo 'literal: $HOME' "expanded: $HOME"
sort < somefile.txt
ls *.txt | wc -l
cat myprog.c | sort | head -5
multiWatch ["date +%T", "ls | wc -l"]      (then Ctrl+C)
sleep 100       (Ctrl+Z)    bg %1    jobs    fg %1    (Ctrl+C)
./check.sh myprog.c
bash check.sh nothere.c
```

Then try Ctrl+R `myprog`, and typing `./myprog ab` followed by Tab.

## Known limitations

* **No pseudo-terminal.** Programs write into a pipe, not a terminal, so
  full-screen programs (`vim`, `top`, `less`) don't work. `ls` also prints one
  name per line; use `ls -C` for columns. A C program that prints a prompt
  without `\n` should call `fflush(stdout)`.
* **macOS `wc -l` pads its number with spaces**, so the assignment's example
  prints `Files in mydir:        3` instead of `Files in mydir: 3`. bash on
  macOS prints the same.
* **Double quotes and line continuation.** Inside double quotes, `\` + Enter
  keeps the line break. The assignment's first example requires this, but bash
  would join the lines.
* **Fonts.** X11 core fonts are ASCII. Other characters are shown as `?`.
* **Not supported:** `$((arithmetic))`, `( subshells )`, loops, functions and
  here-documents. Loops and functions are not required by the assignment.
* **Thread mode crashes.** In thread mode, a crash in one tab takes down the
  whole program, because all tabs share one process. Process mode doesn't have
  this problem.
* **`exit` in a sourced script** ends only the script, not the tab.

## Files

```
src/main.c        command line, start-up
src/gui.c         X11 window, tabs, buffers, line editing, completion, Ctrl+R, clipboard
src/keys.c        default hotkeys and ~/.terminatorrc
src/history.c     shared history (shared memory + mutex / semaphore, history file)
src/shell.c       a tab's shell: main loop, starting tabs, jobs, $(...)
src/parse.c       tokens, quoting, incomplete-input check, expansion
src/exec.c        lists, if/fi, pipelines, redirection, scripts
src/builtins.c    builtins, test/[ ], multiWatch, variables
src/testdriver.c  ./terminator --test (runs tabs without a window)
tests/run_tests.sh
demo/             files for trying it out
DESIGN.md         how it works
```
