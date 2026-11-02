#!/bin/sh
# Automated tests for the shell part of Terminator, in thread mode and in
# process mode. Uses "./terminator --test", which runs the real tab shells
# without a window and "types" the lines below into them.
#
#   sh tests/run_tests.sh        (or: make test)

cd "$(dirname "$0")/.." || exit 1
BIN="$PWD/terminator"
W=$(mktemp -d /tmp/terminator-test.XXXXXX)
W=$(cd "$W" && pwd -P)              # macOS: /tmp is really /private/tmp
mkdir -p "$W/home" "$W/mydir"
touch "$W/mydir/abc.txt" "$W/mydir/abcd.txt" "$W/mydir/def.txt"
printf '#include <stdio.h>\nint main(void){printf("hello from myprog\\n");return 0;}\n' > "$W/mydir/myprog.c"
printf 'banana\napple\ncherry\n' > "$W/somefile.txt"
printf '1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n' > "$W/numbers.txt"

# A script for task 11 (interpreted by us, not bash)
cat > "$W/check.sh" <<'EOF'
#!/bin/bash
# comment line
if [ -f "$1" ]; then
    echo "File exists, compiling..."
    gcc "$1" -o myprog
else
    echo "Source file missing!"
fi
EOF
chmod +x "$W/check.sh"

cat > "$W/vars.sh" <<'EOF'
#!/bin/bash
NAME=world
echo "hello $NAME, args=$# first=$1 second=$2"
COUNT=$(ls | wc -l)
if [ $COUNT -gt 100 ]; then
    echo big
elif [ $COUNT -ge 1 ]; then
    if [ "$1" = "x" ]; then
        echo "nested yes"
    else
        echo "nested no"
    fi
else
    echo empty
fi
false
echo "status=$?"
ls *.txt | wc -l > count.out
cat < count.out | wc -l
exit 3
echo "not reached"
EOF

cat > "$W/bad.sh" <<'EOF'
if [ 1 -eq 1 ]; then
    echo "should not run"
EOF

cat > "$W/input" <<'EOF'
cd mydir
pwd
gcc -o myprog myprog.c
@wait 1500
./myprog
notacommand
echo status=$?
echo "Isn't process creation exciting?\
Fork, exec, repeat!"
echo "Files in $(basename $(pwd)): $(ls | \
wc -l)"
echo 'literal: $HOME' "expanded: $HOME"
echo `echo outer $(echo inner)`
echo 'two
lines'
sort < ../somefile.txt
ls > ../abc
cat ../abc | sort | head -2
sort < missing.txt
echo after-missing
ls *.txt | wc -l
cat ../numbers.txt | sort -n | tail -3 | head -1
sort < ../somefile.txt > ../sorted.txt
cat ../sorted.txt
touch x1.tmp x2.tmp
ls *.tmp | xargs rm
ls *.tmp
sort -n < ../numbers.txt > ../o1.txt & sort -n < ../numbers.txt > ../o2.txt &
@wait 800
echo lines=$(cat ../o1.txt ../o2.txt | wc -l | tr -d " ")
false && echo no || echo yes
X=5
echo "x is $X and ${X}"
sleep 30
@C
echo after-int
sleep 100
@Z
bg %1
jobs
fg %1
@C
echo after-fg
sleep 1 &
@wait 1500
echo next
cat
typed into cat
@D
echo after-cat
../check.sh myprog.c
@wait 2000
bash ../check.sh nothere.c
source ../check.sh nothere.c
cd ..
bash vars.sh x y
bash vars.sh
echo "script exit=$?"
bash bad.sh
cd mydir
multiWatch ["echo watch-one", "ls | wc -l"]
@wait 2500
@C
echo after-watch
history
@new
pwd
echo from-tab-2
history
@tab 1
pwd
exit
EOF

fail=0
check() {      # check <file> <text that must appear>
    if grep -qF -- "$2" "$1"; then echo "  ok    $2"
    else echo "  FAIL  $2"; fail=1; fi
}
check_not() {  # check_not <file> <text that must NOT appear>
    if grep -qF -- "$2" "$1"; then echo "  FAIL  (should not appear) $2"; fail=1
    else echo "  ok    (absent) $2"; fi
}
count() {      # count <file> <text> <expected number of lines containing it>
    n=$(grep -cF -- "$2" "$1")
    if [ "$n" -ge "$3" ]; then echo "  ok    $2 (x$n)"
    else echo "  FAIL  $2 (x$n, wanted $3)"; fail=1; fi
}

for mode in thread process; do
    arg=""; [ $mode = process ] && arg="useProcess=True"
    echo "== $mode mode"
    out="$W/out.$mode"
    rm -f "$W/home/.terminator_history"
    (cd "$W" && HOME="$W/home" "$BIN" --test $arg < "$W/input" > "$out" 2>&1)
    check "$out" "$W/mydir"
    check "$out" "hello from myprog"
    check "$out" "terminator: notacommand: command not found"
    check "$out" "status=127"
    check "$out" "Isn't process creation exciting?"
    check "$out" "Fork, exec, repeat!"
    check "$out" "Files in mydir:"
    check "$out" "literal: \$HOME expanded: $W/home"
    check "$out" "outer inner"
    check "$out" "> lines'"
    check "$out" "apple"
    check "$out" "terminator: missing.txt: No such file or directory"
    check "$out" "after-missing"
    check "$out" "*.tmp"
    check "$out" "lines=20"
    check "$out" "yes"
    check "$out" "x is 5 and 5"
    check "$out" "after-int"
    check "$out" "[1]+ Stopped sleep 100"
    check "$out" "[1]+ sleep 100 &"
    check "$out" "Running  sleep 100 &"
    check "$out" "after-fg"
    check "$out" "Done sleep 1"
    check "$out" "typed into cat"
    check "$out" "after-cat"
    check "$out" "File exists, compiling..."
    count "$out" "Source file missing!" 2
    check "$out" "hello world, args=2 first=x second=y"
    check "$out" "nested yes"
    check "$out" "nested no"
    check "$out" "status=1"
    check "$out" "script exit=3"
    check_not "$out" "not reached"
    check "$out" "syntax error: missing 'fi'"
    check_not "$out" "should not run"
    check "$out" "\"echo watch-one\", "
    check "$out" "\"ls | wc -l\", "
    check "$out" "----------------------------------------------------"
    count "$out" "watch-one" 3
    check "$out" "after-watch"
    check "$out" "<<tab 2>>"
    check "$out" "from-tab-2"
    count "$out" "echo from-tab-2" 2
    check "$out" "<<tab 1 closed>>"
done

echo
if [ $fail = 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED (outputs in $W)"; fi
exit $fail
