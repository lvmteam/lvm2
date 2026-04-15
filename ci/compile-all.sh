#!/bin/bash

die() {
	echo "ERROR: $*" >&2
	exit 1
}

hash() {
	git log -1 --pretty="format:%h" "$@"
}

HEAD="${1:-"HEAD"}"
DESCRIBE="$(git describe "$HEAD")"
HASH="$(hash "$HEAD")"
TARGET=${2:-"origin/main"}

DEST=results
mkdir "$DEST" || die "Failed to create '$DEST'"

# TODO: What should be default configure options?
CONFIGURE=

answ=0

################################################################################
# Test autoreconf OR make generate needs to run:
################################################################################
autoreconf
git commit -a -m "configure: autoreconf" || :

./configure $CONFIGURE && make generate
git add man/*_pregen || :
git commit -a -m "make: generate" || :

if git diff $HASH..HEAD &>/dev/null; then
	answ=1
	echo "ERROR: May need to run autoreconf or make generate" >&2
	git diff $HASH..HEAD > $DEST/autoreconf_make_generate.out
fi

################################################################################
# Test all commits compile:
################################################################################
# git log to show all commits not in the branch into which we are pushing - oldest first
commits="$(git log --pretty="%h" "${HASH}" "^${TARGET}" | tac)"
[[ -n "$commits" ]] || die "No commits found in '${HEAD}' ($DESCRIBE) which are not already in '${TARGET}'"

for commit in $commits; do
	git reset --hard $commit

	OUT=$DEST/$commit.configure.out
	if ! ./configure $CONFIGURE &> "$OUT"; then
		answ=1
		echo "$commit: ERROR: Failed to configure" >&2
		continue
	fi
	rm $OUT

	OUT=$DEST/$commit.make.out
	if ! make &>> $OUT; then
		answ=1
		echo "$commit: ERROR: Failed to compile" >&2
		continue
	fi
	rm $OUT

	OUT=$DEST/$commit.make_rpm.out
	if ! make rpm &>> $OUT; then
		answ=1
		echo "$commit: ERROR: Failed to make rpm" >&2
		continue
	fi
	rm $OUT

	echo "$commit: ok" >&2
done

exit $answ
