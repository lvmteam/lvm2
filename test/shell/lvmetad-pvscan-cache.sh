#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

test -e LOCAL_LVMETAD || skip

aux prepare_pvs 2

vgcreate $vg1 $dev1 $dev2
vgs | grep $vg1

pvscan --cache

vgs | grep $vg1
