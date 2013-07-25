#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux have_thin 1 9 0 || skip

aux prepare_pvs 3

vgcreate -s 1M $vg $(cat DEVICES)

for deactivate in true false; do
	lvcreate -l1 -T $vg/pool

	test $deactivate && lvchange -an $vg

# Confirm we have basic 2M metadata
	check lv_field $vg/pool_tmeta size "2.00m"

	lvresize --poolmetadata +2 $vg/pool

# Test it's been resized to 4M
	check lv_field $vg/pool_tmeta size "4.00m"

# TODO: Add more tests when kernel is fixed


# TODO: Make a full metadata device and test dmeventd support
	lvremove -ff $vg
done
