#!/bin/bash
# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux prepare_dmeventd
aux prepare_pvs 3

vgcreate -c n -l 2 $vg $dev1 $dev2 $dev3
lvcreate -n one -l 1 $vg
lvcreate -n two -l 1 $vg
not lvcreate -n three -l 1 $vg
vgchange -an $vg
vgremove -ff $vg

vgcreate -c n -l 3 $vg $dev1 $dev2 $dev3
lvcreate -n one -l 1 $vg
lvcreate -n snap -s -l 1 $vg/one
lvcreate -n two -l 1 $vg
not lvcreate -n three -l 1 $vg
vgchange --monitor y $vg
vgchange -an $vg 2>&1 | tee vgchange.out
not grep "event server" vgchange.out
