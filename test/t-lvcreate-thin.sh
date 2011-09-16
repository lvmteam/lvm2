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

# test currently needs to drop
# 'return NULL' in _lv_create_an_lv after log_error("Can't create %s without using "
exit 200

. lib/test

aux prepare_vg 4

# FIXME: !!!disabled activation for testing for now!!!
aux lvmconf "global/activation = 0"

check_lv_exists_()
{
	for d in $*; do
		check lv_exists $vg $d
        done
}


# Create named pool only
lvcreate -L4M -T $vg/pool1
lvcreate -L4M -T --thinpool $vg/pool2
lvcreate -L4M -T --thinpool pool3 $vg
lvcreate -L4M --type thin $vg/pool4
lvcreate -L4M --type thin --thinpool $vg/pool5
lvcreate -L4M --type thin --thinpool pool6 $vg
lvcreate -L4M --type thin_pool $vg/pool7
lvcreate -L4M --type thin_pool --thinpool $vg/pool8
lvcreate -L4M --type thin_pool --thinpool pool9 $vg

lvremove $vg/pool1 $vg/pool2 $vg/pool3 $vg/pool4 $vg/pool5 $vg/pool6 $vg/pool7 $vg/pool8 $vg/pool9


# Create default pool name
lvcreate -L8M -T $vg
lvcreate -L8M --type thin $vg
lvcreate -L8M --type thin_pool $vg

lvremove $vg/lvol0 $vg/lvol1 $vg/lvol2


# Create default pool and default thin LV
lvcreate -L8M -V2G -T $vg
lvcreate -L8M -V2G --type thin $vg

lvremove -ff $vg/lvol0 $vg/lvol2


# Create given pool and default thin LV
lvcreate -L8M -V2G -T $vg/pool1
lvcreate -L8M -V2G -T --thinpool $vg/pool2
lvcreate -L8M -V2G -T --thinpool pool3 $vg
lvcreate -L8M -V2G --type thin $vg/pool4
lvcreate -L8M -V2G --type thin --thinpool $vg/pool5
lvcreate -L8M -V2G --type thin --thinpool pool6 $vg

check_lv_exists_ lvol0 lvol1 lvol2 lvol3 lvol4 lvol5
lvremove -ff $vg/pool1 $vg/pool2 $vg/pool3 $vg/pool4 $vg/pool5 $vg/pool6


# Create given pool and given thin LV
lvcreate -L8M -V2G -T $vg/pool1 --name lv1
lvcreate -L8M -V2G -T $vg/pool2 --name $vg/lv2
lvcreate -L8M -V2G -T --thinpool $vg/pool3 --name lv3
lvcreate -L8M -V2G -T --thinpool $vg/pool4 --name $vg/lv4
lvcreate -L8M -V2G -T --thinpool pool5 --name lv5 $vg
lvcreate -L8M -V2G -T --thinpool pool6 --name $vg/lv6 $vg

check_lv_exists_ lv1 lv2 lv3 lv4 lv5 lv6
lvremove -ff $vg/pool1 $vg/pool2 $vg/pool3 $vg/pool4 $vg/pool5 $vg/pool6


lvcreate -L8M -V2G --type thin $vg/pool1 --name lv1
lvcreate -L8M -V2G --type thin $vg/pool2 --name $vg/lv2
lvcreate -L8M -V2G --type thin --thinpool $vg/pool3 --name lv3
lvcreate -L8M -V2G --type thin --thinpool $vg/pool4 --name $vg/lv4
lvcreate -L8M -V2G --type thin --thinpool pool5 --name lv5 $vg
lvcreate -L8M -V2G --type thin --thinpool pool6 --name $vg/lv6 $vg

check_lv_exists_ lv1 lv2 lv3 lv4 lv5 lv6
lvremove -ff $vg/pool1 $vg/pool2 $vg/pool3 $vg/pool4 $vg/pool5 $vg/pool6


check vg_field $vg lv_count 0

# Create thin LV in existing pool
lvcreate -L8M -T $vg/pool
lvcreate -V2G -T $vg/pool
lvcreate -V2G -T --thinpool $vg/pool
lvcreate -V2G -T --thinpool pool $vg
lvcreate -V2G --type thin $vg/pool
lvcreate -V2G --type thin --thinpool $vg/pool
lvcreate -V2G --type thin --thinpool pool $vg

check_lv_exists_ lvol0 lvol1 lvol2 lvol3 lvol4 lvol5


lvcreate -V2G -T $vg/pool --name lv1
lvcreate -V2G -T $vg/pool --name $vg/lv2
lvcreate -V2G -T --thinpool $vg/pool --name lv3
lvcreate -V2G -T --thinpool $vg/pool --name $vg/lv4
lvcreate -V2G -T --thinpool pool --name lv5 $vg
lvcreate -V2G -T --thinpool pool --name $vg/lv6 $vg
lvcreate -V2G --type thin $vg/pool --name lv7
lvcreate -V2G --type thin $vg/pool --name $vg/lv8
lvcreate -V2G --type thin --thinpool $vg/pool --name lv9
lvcreate -V2G --type thin --thinpool $vg/pool --name $vg/lv10
lvcreate -V2G --type thin --thinpool pool --name lv11 $vg
lvcreate -V2G --type thin --thinpool pool --name $vg/lv12 $vg

check_lv_exists_ lv1 lv2 lv3 lv4 lv5 lv6 lv7 lv8 lv9 lv10 lv11 lv12
check vg_field $vg lv_count 19
lvremove -ff $vg/pool


check vg_field $vg lv_count 0

exit 0
# FIXME: !!!Unsupported yet!!!

# Create snapshot of thinLV
lvcreate -L8M -V2G -T $vg/pool --name lv1
should lvcreate -s $vg/lv1
should lvcreate -s $vg/lv1 --name lv2
should lvcreate -s $vg/lv1 --name $vg/lv3
should lvcreate --type snapshot $vg/lv1
should lvcreate --type snapshot $vg/lv1 --name lv4
should lvcreate --type snapshot $vg/lv1 --name $vg/lv5

# Normal Snapshots
should lvcreate -s $vg/lv0 -L8M
should lvcreate -s $vg/lv0 -L8M --name lv6
should lvcreate -s $vg/lv0 -L8M --name $vg/lv7
should lvcreate -s lv0 -L12M --name $vg/lv8
