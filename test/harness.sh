#!/bin/sh

tests="$@"
test -z "$tests" && tests=`echo t-*.sh`

for t in $tests; do
    printf "Running %-40s" "$t ..."
    out=`sh ./$t 2>&1`
    ret=$?
    if test $ret = 0; then
	echo " passed."
    elif test $ret = 200; then
	echo " skipped."
    else
	echo " FAILED!"
	len=`echo $t | wc -c`
	# fancy formatting...
	printf -- "--- Output: $t -"
	for i in `seq $(($len + 14)) 78`; do echo -n "-"; done; echo
	printf "%s\n" "$out"
	printf -- "--- End: $t ----"
	for i in `seq $(($len + 14)) 78`; do echo -n "-"; done; echo
	failed="$failed $t"
    fi
done

if test -n "$failed"; then
    echo "TESTS FAILED:"
    for t in $failed; do
	printf "\t%s\n" $t
    done
else
    echo "All tests passed."
fi
