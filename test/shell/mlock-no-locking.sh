#!/usr/bin/env bash

# Copyright (C) 2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
#
# Check whether activation without memory locking works as expected
. lib/inittest

aux prepare_vg

lvcreate -L1 -n $lv1 $vg

# Create snapshot which normally requires suspend -> locking memory
lvcreate -s -L1 $vg/$lv1 -vvvv &> log

grep "Locking memory" log

# This should be making snapshot without memory locking
lvcreate -s --config 'activation/reserved_memory=0' -L1 $vg/$lv1 -vvvv &> log
grep "Skipping memory locking" log
not grep "Locking memory" log

lvcreate -s --config 'activation/reserved_stack=0' -L1 $vg/$lv1 -vvvv &> log
grep "Skipping memory locking" log
not grep "Locking memory" log

lvcreate -s --config 'activation/reserved_stack=0 activation/reserved_memory=0' -L1 $vg/$lv1 -vvvv &> log
grep "Skipping memory locking" log
not grep "Locking memory" log

vgremove -f $vg
