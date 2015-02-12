#!/usr/bin/env bash
# Copyright (C) 2011-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# sanitize the environment
LANG=C
LC_ALL=C
TZ=UTC

# Put script name into variable, so it can used in external scripts
TESTNAME=${0##*/}
# Nice debug message
PS4='#${BASH_SOURCE[0]##*/}:${LINENO}+ '
export TESTNAME PS4

unset CDPATH

# grab some common utilities
. lib/utils

TESTOLDPWD=$(pwd)
COMMON_PREFIX="LVMTEST"
PREFIX="${COMMON_PREFIX}$$"

if test -z "$LVM_TEST_DIR"; then LVM_TEST_DIR=$TMPDIR; fi
TESTDIR=$(mkdtemp "${LVM_TEST_DIR:-/tmp}" "$PREFIX.XXXXXXXXXX") || \
	die "failed to create temporary directory in ${LVM_TEST_DIR:-$TESTOLDPWD}"
RUNNING_DMEVENTD=$(pgrep dmeventd) || true

export TESTOLDPWD TESTDIR COMMON_PREFIX PREFIX RUNNING_DMEVENTD

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

if test -n "$LVM_TEST_FLAVOUR"; then
	. lib/flavour-$LVM_TEST_FLAVOUR
fi

DM_DEFAULT_NAME_MANGLING_MODE=none
DM_DEV_DIR="$TESTDIR/dev"
LVM_SYSTEM_DIR="$TESTDIR/etc"
mkdir "$LVM_SYSTEM_DIR" "$DM_DEV_DIR"
if test -n "$LVM_TEST_DEVDIR" ; then
	DM_DEV_DIR=$LVM_TEST_DEVDIR
else
	mknod "$DM_DEV_DIR/testnull" c 1 3 || die "mknod failed";
	echo >"$DM_DEV_DIR/testnull" || \
		die "Filesystem does support devices in $DM_DEV_DIR (mounted with nodev?)"
	mkdir "$DM_DEV_DIR/mapper"
fi

# abort on the internal dm errors in the tests (allowing test user override)
DM_ABORT_ON_INTERNAL_ERRORS=${DM_ABORT_ON_INTERNAL_ERRORS:-1}

export DM_DEFAULT_NAME_MANGLING_MODE DM_DEV_DIR LVM_SYSTEM_DIR DM_ABORT_ON_INTERNAL_ERRORS

echo "$TESTNAME" >TESTNAME

echo "Kernel is $(uname -a)"
# Report SELinux mode
if which getenforce &>/dev/null ; then
	echo "Selinux mode is \"$(getenforce 2>/dev/null)\"."
else
	echo "Selinux mode is not installed."
fi

# Set vars from utils now that we have TESTDIR/PREFIX/...
prepare_test_vars

test -n "$BASH" && set -eE -o pipefail

aux lvmconf
aux prepare_clvmd
test -n "$LVM_TEST_LVMETAD" && {
	export LVM_LVMETAD_SOCKET="$TESTDIR/lvmetad.socket"
	export LVM_LVMETAD_PIDFILE="$TESTDIR/lvmetad.pid"
	aux prepare_lvmetad
}

# Vars for harness
echo "@TESTDIR=$TESTDIR"
echo "@PREFIX=$PREFIX"

set -vx
