# Terminator - build on macOS
#
#   make          build ./terminator
#   make test     run the automated tests (no window needed)
#   make clean
#
# Needs the Xcode command line tools (xcode-select --install) and
# XQuartz (https://www.xquartz.org), which puts X11 in /opt/X11.
# Using Homebrew's libx11 instead?  make X11=$(brew --prefix)

CC      = cc
X11    ?= /opt/X11
CFLAGS  = -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(X11)/include $(EXTRA)
LDFLAGS = -L$(X11)/lib
LDLIBS  = -lX11 -lpthread

SRCS = src/main.c src/gui.c src/keys.c src/history.c src/shell.c \
       src/parse.c src/exec.c src/builtins.c src/testdriver.c
OBJS = $(SRCS:.c=.o)

terminator: check-x11 $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/terminator.h src/shell.h
	$(CC) $(CFLAGS) -c $< -o $@

check-x11:
	@test -f $(X11)/include/X11/Xlib.h || { \
	  echo ""; \
	  echo "X11 headers not found in $(X11)."; \
	  echo "Install XQuartz from https://www.xquartz.org (or: brew install --cask xquartz),"; \
	  echo "log out and back in, then run make again."; \
	  echo ""; exit 1; }

test: terminator
	sh tests/run_tests.sh

clean:
	rm -f terminator $(OBJS)

.PHONY: check-x11 test clean
