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

. lib/test

check_lv_exists_()
{
	for d in $*; do
		check lv_exists $vg $d
	done
}

check_lv_field_modules_()
{
	mod=$1
	shift

	for d in $*; do
		check lv_field $vg/$d modules $mod
	done
}


#
# Main
#
aux target_at_least dm-thin-pool 1 0 0 || skip

aux prepare_devs 2 64

pvcreate $dev1 $dev2

clustered=
test -e LOCAL_CLVMD && clustered="--clustered y"

vgcreate $clustered $vg -s 64K $dev1 $dev2

# Create named pool only
lvcreate -l1 -T $vg/pool1
lvcreate -l1 -T --thinpool $vg/pool2
lvcreate -l1 -T --thinpool pool3 $vg
lvcreate -l1 --type thin $vg/pool4
lvcreate -l1 --type thin --thinpool $vg/pool5
lvcreate -l1 --type thin --thinpool pool6 $vg
lvcreate -l1 --type thin-pool $vg/pool7
lvcreate -l1 --type thin-pool --thinpool $vg/pool8
lvcreate -l1 --type thin-pool --thinpool pool9 $vg

lvremove -ff $vg/pool1 $vg/pool2 $vg/pool3 $vg/pool4 $vg/pool5 $vg/pool6 $vg/pool7 $vg/pool8 $vg/pool9
check vg_field $vg lv_count 0


# Create default pool name
lvcreate -l1 -T $vg
lvcreate -l1 --type thin $vg
lvcreate -l1 --type thin-pool $vg

lvremove -ff $vg/lvol0 $vg/lvol1 $vg/lvol2
check vg_field $vg lv_count 0


# Create default pool and default thin LV
lvcreate -l1 -V2G -T $vg
lvcreate -l1 -V2G --type thin $vg

lvremove -ff $vg


# Create named pool and default thin LV
lvcreate -L4M -V2G -T $vg/pool1
lvcreate -L4M -V2G -T --thinpool $vg/pool2
lvcreate -L4M -V2G -T --thinpool pool3 $vg
lvcreate -L4M -V2G --type thin $vg/pool4
lvcreate -L4M -V2G --type thin --thinpool $vg/pool5
lvcreate -L4M -V2G --type thin --thinpool pool6 $vg

check_lv_exists_ lvol0 lvol1 lvol2 lvol3 lvol4 lvol5
lvremove -ff $vg


# Create named pool and named thin LV
lvcreate -L4M -V2G -T $vg/pool1 --name lv1
lvcreate -L4M -V2G -T $vg/pool2 --name $vg/lv2
lvcreate -L4M -V2G -T --thinpool $vg/pool3 --name lv3
lvcreate -L4M -V2G -T --thinpool $vg/pool4 --name $vg/lv4
lvcreate -L4M -V2G -T --thinpool pool5 --name lv5 $vg
lvcreate -L4M -V2G -T --thinpool pool6 --name $vg/lv6 $vg

check_lv_exists_ lv1 lv2 lv3 lv4 lv5 lv6
lvremove -ff $vg


lvcreate -L4M -V2G --type thin $vg/pool1 --name lv1
lvcreate -L4M -V2G --type thin $vg/pool2 --name $vg/lv2
lvcreate -L4M -V2G --type thin --thinpool $vg/pool3 --name lv3
lvcreate -L4M -V2G --type thin --thinpool $vg/pool4 --name $vg/lv4
lvcreate -L4M -V2G --type thin --thinpool pool5 --name lv5 $vg
lvcreate -L4M -V2G --type thin --thinpool pool6 --name $vg/lv6 $vg

check_lv_exists_ lv1 lv2 lv3 lv4 lv5 lv6
lvremove -ff $vg


# Create default thin LV in existing pool
lvcreate -L4M -T $vg/pool
lvcreate -V2G -T $vg/pool
lvcreate -V2G -T --thinpool $vg/pool
lvcreate -V2G -T --thinpool pool $vg
lvcreate -V2G --type thin $vg/pool
lvcreate -V2G --type thin --thinpool $vg/pool
lvcreate -V2G --type thin --thinpool pool $vg

check_lv_exists_ lvol0 lvol1 lvol2 lvol3 lvol4 lvol5


# Create named thin LV in existing pool
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

lvremove -ff $vg
check vg_field $vg lv_count 0

# Create thin snapshot of thinLV
lvcreate -L10M -V10M -T $vg/pool --name lv1
mkfs.ext4 $DM_DEV_DIR/$vg/lv1
lvcreate -s $vg/lv1
fsck -p $DM_DEV_DIR/$vg/lvol0
lvcreate -s $vg/lv1 --name lv2
lvcreate -s $vg/lv1 --name $vg/lv3
lvcreate --type snapshot $vg/lv1
lvcreate --type snapshot $vg/lv1 --name lv4
lvcreate --type snapshot $vg/lv1 --name $vg/lv5

check_lv_field_modules_ thin-pool lv1 lvol0 lv2 lv3 lvol1 lv4 lv5
check vg_field $vg lv_count 8
lvremove -ff $vg


# Normal Snapshots of thinLV
lvcreate -L4M -V2G -T $vg/pool --name lv1
lvcreate -s $vg/lv1 -l1
lvcreate -s $vg/lv1 -l1 --name lv2
lvcreate -s $vg/lv1 -l1 --name $vg/lv3
lvcreate -s lv1 -L4M --name $vg/lv4

check_lv_field_modules_ snapshot lvol0 lv2 lv3 lv4
check vg_field $vg lv_count 6

lvremove -ff $vg
check vg_field $vg lv_count 0


# Fail cases
# Too small pool size (1 extent 64KB) for given chunk size
not lvcreate --chunksize 256 -l1 -T $vg/pool1
# Too small chunk size (min is 64KB -  128 sectors)
not lvcreate --chunksize 32 -l1 -T $vg/pool1

lvcreate -L4M -V2G --name lv1 -T $vg/pool1
# Origin name is not accepted
not lvcreate -s $vg/lv1 -L4M -V2G --name $vg/lv4
