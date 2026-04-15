#!/bin/bash

die() {
	echo "ERROR: $*" >&2
	exit 2
}

hash() {
	git log -1 --pretty="format:%h" "$@"
}

# TODO: This just compares HEAD with main. Do we care about other branches?
HEAD="${1:-"HEAD"}"
TARGET=${2:-"origin/main"}

answ=0

echo -e "\nFiles changed in the MR:"
git diff --exit-code --name-only "${HEAD}" "^${TARGET}"
case $? in
	0)
		echo "INFO: Huh, empty MR?" >&2
		exit 0 ;;
	1)
		;;
	*)
		die "Problem with git diff. The tests will run anyway."
		exit 2 ;;
esac

echo -e "\nChecking the files for significant changes:"

if git diff --name-only "${HEAD}" "^${TARGET}" | grep -v '^\(\.gitlab-ci.yml\|ci/\|WHATS_NEW\|VERSION\|man/\|doc/\|README\|TESTING\|COPYING\|INSTALL\|\.gitignore\|coverity\|ikiwiki.setup/\|nix/\|po/\)'; then
	echo "INFO: Changed files, running CI" >&2
	exit 1
else
	echo "INFO: Dry run, no significant files changed " >&2
	exit 0
fi
