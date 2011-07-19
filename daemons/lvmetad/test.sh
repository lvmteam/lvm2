#!/bin/bash

export LD_LIBRARY_PATH="$1"

test -n "$2" && {
    rm -f /var/run/lvmetad.{socket,pid}
    chmod +rx lvmetad
    valgrind ./lvmetad -f &
    PID=$!
    sleep 1
    ./testclient
    kill $PID
    exit 0
}

sudo ./test.sh "$1" .
