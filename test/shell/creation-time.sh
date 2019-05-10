#!/usr/bin/env bash

# Copyright (C) 2019 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Check we can read metadata with out-of-range creation time

# Due to a bug in 32-bit version lvm2 <2.02.169  produced metadata
# contained invalid number for creation_time

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_vg 1

lvcreate -an -L1 -n $lv1 $vg

vgcfgbackup -f back $vg

sed -e 's/creation_time = \(.*\)$/creation_time = 12029933779523993599/g' back >backnew

vgcfgrestore -f backnew $vg  |& tee err

# Check the time was spotted
grep Invalid err

vgcfgbackup -f back $vg |& tee err

# Check the time is not a problem anymore
not grep Invalid err

vgremove -ff $vg
