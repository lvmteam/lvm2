#!/bin/bash

export LD_LIBRARY_PATH="$1"

test -n "$2" && {
    ./lvmetad -f &
    PID=$!
    sleep .1
    ./testclient
    kill $PID
    exit 0
}

sudo ./test.sh "$1" .
