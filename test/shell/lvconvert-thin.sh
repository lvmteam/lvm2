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

prepare_lvs()
{
	lvremove -f $vg
	lvcreate -L10M -n $lv1 $vg
	lvcreate -L8M -n $lv2 $vg
}

#
# Main
#
aux have_thin 1 0 0 || skip

aux prepare_pvs 4 64

# build one large PV
vgcreate $vg1 $(cut -d ' ' -f -3 DEVICES)
lvcreate -s -l 100%FREE -n $lv $vg1 --virtualsize 64T
aux extend_filter_LVMTEST

pvcreate "$DM_DEV_DIR/$vg1/$lv"
vgcreate $vg -s 64K $(cut -d ' ' -f 4 DEVICES) "$DM_DEV_DIR/$vg1/$lv"

# create mirrored LVs for data and metadata volumes
lvcreate -aey -L10M --type mirror -m1 --mirrorlog core -n $lv1 $vg
lvcreate -aey -L10M -n $lv2 $vg
lvchange -an $vg/$lv1

# conversion fails for mirror segment type
not lvconvert --thinpool $vg/$lv1
not lvconvert --thinpool $vg/$lv2 --poolmetadata $vg/$lv2
lvremove -f $vg

# create RAID LVs for data and metadata volumes
lvcreate -aey -L10M --type raid1 -m1 -n $lv1 $vg
lvcreate -aey -L10M --type raid1 -m1 -n $lv2 $vg
lvchange -an $vg/$lv1

# conversion fails for internal volumes
not lvconvert --thinpool $vg/${lv1}_rimage_0
not lvconvert --thinpool $vg/$lv1 --poolmetadata $vg/${lv2}_rimage_0
# can't use --readahead with --poolmetadata
not lvconvert --thinpool $vg/$lv1 --poolmetadata $vg/$lv2 --readahead 512

lvconvert --thinpool $vg/$lv1 --poolmetadata $vg/$lv2

prepare_lvs
lvconvert -c 64 --stripes 2 --thinpool $vg/$lv1 --readahead 48

lvremove -f $vg
lvcreate -L1T -n $lv1 $vg
lvconvert -c 8M --thinpool $vg/$lv1

lvremove -f $vg
# test with bigger sizes
lvcreate -L1T -n $lv1 $vg
lvcreate -L8M -n $lv2 $vg
lvcreate -L1M -n $lv3 $vg

# chunk size is bigger then size of thin pool data
not lvconvert -c 1G --thinpool $vg/$lv3
# stripes can't be used with poolmetadata
not lvconvert --stripes 2 --thinpool $vg/$lv1 --poolmetadata $vg/$lv2
# too small metadata (<2M)
not lvconvert -c 64 --thinpool $vg/$lv1 --poolmetadata $vg/$lv3
# too small chunk size fails
not lvconvert -c 4 --thinpool $vg/$lv1 --poolmetadata $vg/$lv2
# too big chunk size fails
not lvconvert -c 2G --thinpool $vg/$lv1 --poolmetadata $vg/$lv2
# negative chunk size fails
not lvconvert -c -256 --thinpool $vg/$lv1 --poolmetadata $vg/$lv2
# non power of 2 fails
not lvconvert -c 88 --thinpool $vg/$lv1 --poolmetadata $vg/$lv2

# Warning about smaller then suggested
lvconvert -c 256 --thinpool $vg/$lv1 --poolmetadata $vg/$lv2 |& tee err
grep "WARNING: Chunk size is smaller" err

lvremove -f $vg
lvcreate -L1T -n $lv1 $vg
lvcreate -L32G -n $lv2 $vg
# Warning about bigger then needed
lvconvert --thinpool $vg/$lv1 --poolmetadata $vg/$lv2 |& tee err
grep "WARNING: Maximum size" err

lvremove -f $vg
lvcreate -L24T -n $lv1 $vg
# Warning about bigger then needed (24T data and 16G -> 128K chunk)
lvconvert -c 64 --thinpool $vg/$lv1 |& tee err
grep "WARNING: Chunk size is too small" err

#lvs -a -o+chunk_size,stripe_size,seg_pe_ranges

# Convertions of pool to mirror or RAID is unsupported
not lvconvert --type mirror -m1 $vg/$lv1
not lvconvert --type raid1 -m1 $vg/$lv1

vgremove -ff $vg
