#!/bin/bash
# Variables, arguments, $?, nested if/elif/else, pipes and exit.
NAME=Terminator
echo "Hello from $NAME: $# arguments, first is '$1'"
FILES=$(ls | wc -l)
if [ $FILES -gt 10 ]; then
    echo "many files"
elif [ $FILES -ge 1 ]; then
    if [ -d "$1" ]; then
        echo "$1 is a directory"
    else
        echo "$1 is not a directory"
    fi
else
    echo "no files"
fi
grep -c include myprog.c > count.txt
echo "lines with include: $(cat count.txt)"
false
echo "status of false: $?"
exit 0
