#!/bin/sh
# Copyright (C) 2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# 'Exercise duplicate metadata diagnostics'

. lib/test

aux prepare_devs 3

vgcreate -c n --metadatasize 128k $vg1 $dev1

# copy mda
dd if=$dev1 of=$dev2 bs=256K count=1
dd if=$dev1 of=$dev3 bs=256K count=1

pvs $dev1
vgs $vg1
