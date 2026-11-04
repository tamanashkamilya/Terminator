#!/bin/bash
# variables, arguments, $?, nested if, pipes, redirection, $(...), exit
NAME=world
echo "hello $NAME, $# args, first=$1 second=$2"
COUNT=$(ls | wc -l)
if [ $COUNT -gt 100 ]; then
    echo "big folder"
elif [ $COUNT -ge 1 ]; then
    if [ "$1" = "x" ]; then
        echo "nested: first arg is x"
    else
        echo "nested: first arg is not x"
    fi
else
    echo "empty"
fi
false
echo "status of false: $?"
ls *.c | wc -l > count.out
echo "c files: $(cat count.out)"
exit 3
echo "never printed"
