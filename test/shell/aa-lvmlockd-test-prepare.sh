#!/bin/sh
# Copyright (C) 2008-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Set up things to run tests with lvmlockd'

. lib/utils
. lib/inittest

[ -z "$LVM_TEST_LVMLOCKD_TEST" ] && skip;

# TODO: allow this to be configured sanlock/dlm
LVM_TEST_LVMLOCKD_TEST_DLM=1

prepare_lvmlockd_test() {
	if pgrep lvmlockd ; then
		echo "Cannot run while existing lvmlockd process exists"
		exit 1
	fi

	if test -n "$LVM_TEST_LVMLOCKD_TEST_SANLOCK"; then
		lvmlockd --test -g sanlock
	fi

	if test -n "$LVM_TEST_LVMLOCKD_TEST_DLM"; then
		lvmlockd --test -g dlm
	fi

	sleep 1
	if ! pgrep lvmlockd ; then
		echo "Failed to start lvmlockd"
		exit 1
	fi
}

prepare_lvmlockd_test

