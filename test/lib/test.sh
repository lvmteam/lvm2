#!/bin/bash
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

TESTDIR=$(mkdtemp "${LVM_TEST_DIR:-$TESTOLDPWD}" "$PREFIX.XXXXXXXXXX") || \
	die "failed to create temporary directory in ${LVM_TEST_DIR:-$TESTOLDPWD}"
RUNNING_DMEVENTD=$(pgrep dmeventd) || true

export TESTOLDPWD TESTDIR COMMON_PREFIX PREFIX RUNNING_DMEVENTD

test -n "$BASH" && trap 'set +vx; STACKTRACE; set -vx' ERR
trap 'aux teardown' EXIT # don't forget to clean up

DM_DEV_DIR="$TESTDIR/dev"
LVM_SYSTEM_DIR="$TESTDIR/etc"
mkdir "$LVM_SYSTEM_DIR" "$TESTDIR/lib" "$DM_DEV_DIR"
if test -n "$LVM_TEST_DEVDIR" ; then
	DM_DEV_DIR=$LVM_TEST_DEVDIR
else
	mknod "$DM_DEV_DIR/testnull" c 1 3 || die "mknod failed";
	echo >"$DM_DEV_DIR/testnull" || \
		die "Filesystem does support devices in $DM_DEV_DIR (mounted with nodev?)"
	mkdir "$DM_DEV_DIR/mapper"
fi

export DM_DEV_DIR LVM_SYSTEM_DIR

cd "$TESTDIR"

echo "$TESTNAME" >TESTNAME

# Setting up symlink from $i to $TESTDIR/lib
find "$abs_top_builddir/daemons/dmeventd/plugins/" -name '*.so' \
	-exec ln -s -t lib "{}" +
find "$abs_top_builddir/test/lib" ! \( -name '*.sh' -o -name '*.[cdo]' \
	-o -name '*~' \)  -exec ln -s -t lib "{}" +

# Set vars from utils now that we have TESTDIR/PREFIX/...
prepare_test_vars

test -n "$BASH" && set -eE -o pipefail

aux lvmconf
aux prepare_clvmd
test -n "$LVM_TEST_LVMETAD" && {
	aux prepare_lvmetad
	export LVM_LVMETAD_SOCKET="$TESTDIR/lvmetad.socket"
}
echo "@TESTDIR=$TESTDIR"
echo "@PREFIX=$PREFIX"

set -vx
