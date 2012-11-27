#!/bin/bash
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# test defaults entered through lvm.conf

. lib/test

#
# Main
#
aux have_thin 1 0 0 || skip

aux prepare_vg 2

lvcreate -T -L8M $vg/pool0

aux lvmconf "allocation/thin_pool_chunk_size = 128" \
	    "allocation/thin_pool_discards = \"ignore\"" \
	    "allocation/thin_pool_zero = 0"

lvcreate -T -L8M $vg/pool1

check lv_field $vg/pool1 chunksize "128.00k"
check lv_field $vg/pool1 discards "ignore"
check lv_field $vg/pool1 zero 0

vgremove -f $vg
