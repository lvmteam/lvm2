#!/usr/bin/env bash
# Copyright (C) 2011-2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

initskip() {
	test "$#" -eq 0 || echo "TEST SKIPPED: $@"
	exit 200
}

# sanitize the environment
LANG=C
LC_ALL=C
TZ=UTC

# Put script name into variable, so it can used in external scripts
TESTNAME=${0##*/}
# Nice debug message
PS4='#${BASH_SOURCE[0]##*/}:${LINENO}+ '
export TESTNAME PS4

if test -n "$LVM_TEST_FLAVOUR"; then
	. lib/flavour-$LVM_TEST_FLAVOUR
fi

test -n "$SKIP_WITHOUT_CLVMD" && test "$LVM_TEST_LOCKING" -ne 3 && initskip
test -n "$SKIP_WITH_CLVMD" && test "$LVM_TEST_LOCKING" -eq 3 && initskip

test -n "$SKIP_WITHOUT_LVMETAD" && test -z "$LVM_TEST_LVMETAD" && initskip
test -n "$SKIP_WITH_LVMETAD" && test -n "$LVM_TEST_LVMETAD" && initskip

test -n "$SKIP_WITH_LVMPOLLD" && test -n "$LVM_TEST_LVMPOLLD" && initskip

test -n "$SKIP_WITH_LVMLOCKD" && test -n "$LVM_TEST_LVMLOCKD" && initskip

unset CDPATH

# grab some common utilities
. lib/utils

TESTOLDPWD=$(pwd)
COMMON_PREFIX="LVMTEST"
PREFIX="${COMMON_PREFIX}$$"

# Check we are not conflickting with some exiting setup
dmsetup table | not grep "${PREFIX}[^0-9]" || die "DM table already has devices with prefix $PREFIX!"

if test -z "$LVM_TEST_DIR"; then LVM_TEST_DIR=$TMPDIR; fi
TESTDIR=$(mkdtemp "${LVM_TEST_DIR:-/tmp}" "$PREFIX.XXXXXXXXXX") || \
	die "failed to create temporary directory in ${LVM_TEST_DIR:-$TESTOLDPWD}"
RUNNING_DMEVENTD=$(pgrep dmeventd || true)

export TESTOLDPWD TESTDIR COMMON_PREFIX PREFIX RUNNING_DMEVENTD
export LVM_LOG_FILE_EPOCH=DEBUG
export LVM_LOG_FILE_MAX_LINES=100000
export LVM_EXPECTED_EXIT_STATUS=1

test -n "$BASH" && trap 'set +vx; STACKTRACE; set -vx' ERR
trap 'aux teardown' EXIT # don't forget to clean up

cd "$TESTDIR"
mkdir lib

# Setting up symlink from $i to $TESTDIR/lib
test -n "$abs_top_builddir" && \
    find "$abs_top_builddir/daemons/dmeventd/plugins/" -name '*.so' \
    -exec ln -s -t lib "{}" +
find "$TESTOLDPWD/lib" ! \( -name '*.sh' -o -name '*.[cdo]' \
    -o -name '*~' \)  -exec ln -s -t lib "{}" +

DM_DEFAULT_NAME_MANGLING_MODE=none
DM_DEV_DIR="$TESTDIR/dev"
LVM_SYSTEM_DIR="$TESTDIR/etc"
# abort on the internal dm errors in the tests (allowing test user override)
DM_ABORT_ON_INTERNAL_ERRORS=${DM_ABORT_ON_INTERNAL_ERRORS:-1}

export DM_DEFAULT_NAME_MANGLING_MODE DM_DEV_DIR LVM_SYSTEM_DIR DM_ABORT_ON_INTERNAL_ERRORS

mkdir "$LVM_SYSTEM_DIR" "$DM_DEV_DIR"
if test -n "$LVM_TEST_DEVDIR" ; then
	test -d "$LVM_TEST_DEVDIR" || die "Test device directory LVM_TEST_DEVDIR=\"$LVM_TEST_DEVDIR\" is not valid."
	DM_DEV_DIR=$LVM_TEST_DEVDIR
else
	mknod "$DM_DEV_DIR/testnull" c 1 3 || die "mknod failed"
	echo >"$DM_DEV_DIR/testnull" || \
		die "Filesystem does support devices in $DM_DEV_DIR (mounted with nodev?)"
	# dmsetup makes here needed control entry if still missing
	dmsetup version || \
		die "Dmsetup in $DM_DEV_DIR can't report version?"
fi

echo "$TESTNAME" >TESTNAME

echo "Kernel is $(uname -a)"
# Report SELinux mode
echo "Selinux mode is $(getenforce 2>/dev/null || echo not installed)."
free -m || true

# Set vars from utils now that we have TESTDIR/PREFIX/...
prepare_test_vars

test -n "$BASH" && set -eE -o pipefail

# Vars for harness
echo "@TESTDIR=$TESTDIR"
echo "@PREFIX=$PREFIX"

if test -n "$LVM_TEST_LVMETAD" ; then
	export LVM_LVMETAD_SOCKET="$TESTDIR/lvmetad.socket"
	export LVM_LVMETAD_PIDFILE="$TESTDIR/lvmetad.pid"
	aux prepare_lvmetad
else
	# lvmetad prepares its own lvmconf
	export LVM_LVMETAD_PIDFILE="$TESTDIR/non-existing-file"
	aux lvmconf
	aux prepare_clvmd
fi

test -n "$LVM_TEST_LVMPOLLD" && {
	export LVM_LVMPOLLD_SOCKET="$TESTDIR/lvmpolld.socket"
	export LVM_LVMPOLLD_PIDFILE="$TESTDIR/lvmpolld.pid"
	aux prepare_lvmpolld
}

if test -n "$LVM_TEST_LVMLOCKD" ; then
	if test -n "$LVM_TEST_LOCK_TYPE_SANLOCK" ; then
		aux lvmconf 'local/host_id = 1'
	fi

	export SHARED="--shared"
fi

# for check_lvmlockd_test, lvmlockd is restarted for each shell test.
# for check_lvmlockd_{sanlock,dlm}, lvmlockd is started once by
# aa-lvmlockd-{sanlock,dlm}-prepare.sh and left running for all shell tests.

if test -n "$LVM_TEST_LVMLOCKD_TEST" ; then
	aux prepare_lvmlockd
fi

echo "<======== Processing test: \"$TESTNAME\" ========>"

set -vx
