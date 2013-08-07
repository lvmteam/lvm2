#!/bin/sh
# Copyright (C) 2010-2013 Red Hat, Inc. All rights reserved.
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

kernel_at_least 2 6 33 || skip

aux prepare_pvs 2

vgcreate -s 4k $vg $(cat DEVICES)
lvcreate -aey -l 5 -n foo $vg
lvcreate -s -n snap $vg/foo -l 3 -c 4k
lvcreate -s -n snap2 $vg/foo -l 6 -c 4k
dd if=/dev/urandom of="$DM_DEV_DIR/$vg/snap2" count=1 bs=1024
lvcreate -aey --type mirror -m 1 -n mirr $vg -l 1 --mirrorlog core
lvs $vg
aux apitest percent $vg

vgremove -ff $vg
