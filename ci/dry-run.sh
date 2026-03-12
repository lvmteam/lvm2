#!/bin/bash

die() {
	echo "ERROR: $*" >&2
	exit 1
}

hash() {
	git log -1 --pretty="format:%h" "$@"
}

# TODO: This just compares HEAD with main. Do we care about other branches?
HEAD="${1:-"HEAD"}"
DESCRIBE="$(git describe "$HEAD")"
HASH="$(hash "$HEAD")"
TARGET=${2:-"origin/main"}

answ=0

echo "Checking files changed in the MR for significant changes:"
git diff --name-only "${HEAD}" "^${TARGET}"
echo "-----"

if git diff --name-only "${HEAD}" "^${TARGET}" | grep -v '^\(\.gitlab-ci.yaml\|ci/\|WHATS_NEW\|VERSION\|man/\|doc/\|README\|TESTING\|COPYING\|INSTALL\|\.gitignore\|coverity\|ikiwiki.setup/\|nix/\|po/\)'; then
	echo "INFO: Changed files, running CI" >&2
	exit 1
else
	echo "INFO: Dry run, no significant files changed " >&2
	exit 0
fi
