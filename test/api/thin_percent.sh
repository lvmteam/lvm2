#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

. lib/test

aux have_thin 1 0 0 || skip

# disable thin_check if not present in system
which thin_check || aux lvmconf 'global/thin_check_executable = ""'

aux prepare_devs 2

vgcreate -s 64k $vg $(cat DEVICES)

lvcreate -L5M -T $vg/pool

lvcreate -V1M -T $vg/pool -n thin
dd if=/dev/urandom of="$DM_DEV_DIR/$vg/thin" count=2 bs=256K

lvcreate -s $vg/thin -K -n snap
dd if=/dev/urandom of="$DM_DEV_DIR/$vg/snap" count=3 bs=256K

lvs -o+discards $vg

aux apitest thin_percent $vg

vgremove -ff $vg
