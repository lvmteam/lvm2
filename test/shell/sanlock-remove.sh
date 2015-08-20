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

test_description='Remove the sanlock test setup'

. lib/inittest

[ -z "$LVM_TEST_LOCK_TYPE_SANLOCK" ] && skip;

# FIMXME: get this to run after a test fails

# Removes the VG with the global lock that was created by
# the corresponding create script.

vgremove --config 'devices { global_filter=["a|GL_DEV|", "r|.*|"] filter=["a|GL_DEV|", "r|.*|"]}' glvg

# FIXME: collect debug logs (only if a test failed?)
# lvmlockctl -d > lvmlockd-debug.txt
# sanlock log_dump > sanlock-debug.txt

killall lvmlockd
killall sanlock

killall -9 lvmlockd
killall -9 sanlock

# FIXME: dmsetup remove LVMTEST*-lvmlock

dmsetup remove glvg-lvmlock || true
dmsetup remove GL_DEV || true

