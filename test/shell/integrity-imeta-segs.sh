#!/usr/bin/env bash

# Copyright (C) 2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test creating integrity metadata LV requiring multiple LV segments

SKIP_WITH_LOW_SPACE=256

. lib/inittest

aux have_integrity 1 5 0 || skip

which mkfs.ext4 || skip
mnt="mnt"
mkdir -p "$mnt"

# Use awk instead of anoyingly long log out from printf
#printf "%0.sA" {1..16384} >> fileA
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 16384) printf "B" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

# generate random data
dd if=/dev/urandom of=randA bs=512K count=2
dd if=/dev/urandom of=randB bs=512K count=3
dd if=/dev/urandom of=randC bs=512K count=4

_prepare_vg() {
        vgcreate $SHARED $vg "$dev1" "$dev2"
        pvs
}

_add_data_to_lv() {
        mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

        mount "$DM_DEV_DIR/$vg/$lv1" $mnt

        # add original data
        cp randA $mnt
        cp randB $mnt
        cp randC $mnt
        mkdir $mnt/1
        cp fileA $mnt/1
        cp fileB $mnt/1
        cp fileC $mnt/1
        mkdir $mnt/2
        cp fileA $mnt/2
        cp fileB $mnt/2
        cp fileC $mnt/2

        umount $mnt
}

_verify_data_on_lv() {
        mount "$DM_DEV_DIR/$vg/$lv1" $mnt

        diff randA $mnt/randA
        diff randB $mnt/randB
        diff randC $mnt/randC
        diff fileA $mnt/1/fileA
        diff fileB $mnt/1/fileB
        diff fileC $mnt/1/fileC
        diff fileA $mnt/2/fileA
        diff fileB $mnt/2/fileB
        diff fileC $mnt/2/fileC

        umount $mnt
}

_replace_data_on_lv() {
        mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"

        rm "$mnt/randA"
        rm "$mnt/randB"
        rm "$mnt/randC"
        rm "$mnt/1/fileA"
        rm "$mnt/1/fileB"
        rm "$mnt/1/fileC"
        rm "$mnt/2/fileA"
        rm "$mnt/2/fileB"
        rm "$mnt/2/fileC"

        cp randA "$mnt"
        cp randB "$mnt"
        cp randC "$mnt"
        cp fileA "$mnt/1"
        cp fileB "$mnt/1"
        cp fileC "$mnt/1"
        cp fileA "$mnt/2"
        cp fileB "$mnt/2"
        cp fileC "$mnt/2"

        umount "$mnt"
}

# Create a raid LV with multi-segment images (based on an example of vg metadata)
# Strategy: Fragment the PV by creating/removing temporary LVs, causing the raid
# images to be allocated in multiple non-contiguous segments. When adding integrity,
# the imeta will likely also need multiple segments due to the fragmented free space.

# Use smaller extent size to reduce total device size needed
# With 2MB extents, a 400MB raid LV can have multi-segment images with smaller devices
aux prepare_devs 2 256
vgcreate -s 2m $SHARED $vg "$dev1" "$dev2"

# Two-phase fragmentation strategy:
# Phase 1: Create some larger LVs, then create the raid LV which will consume the gaps
# Phase 2: Create small fragments in the remaining space where imeta will allocate

# Phase 1: Create large fragments so the raid LV will have 2-3 segments
lvcreate -n frag1 -L 60M -an $vg "$dev1"
lvcreate -n frag2 -L 60M -an $vg "$dev2"

lvcreate -n temp1 -L 80M -an $vg "$dev1"
lvcreate -n temp2 -L 80M -an $vg "$dev2"

lvcreate -n frag3 -L 40M -an $vg "$dev1"
lvcreate -n frag4 -L 40M -an $vg "$dev2"

# Remove temp1 and temp2, leaving large gaps for the raid LV
lvremove -y $vg/temp1 $vg/temp2

pvs -o+pv_free,pv_used

# Now create a raid1 LV that will use the large fragmented gaps
# This will result in raid images with 2 segments
lvcreate --type raid1 -m1 -n $lv1 -L 120M -an $vg

lvchange -ay $vg/$lv1
_add_data_to_lv
lvchange -an $vg/$lv1

lvs -a -o+devices,seg_count $vg

# Check if the rimage has multiple segments (should be 2)
check lv_field $vg/${lv1}_rimage_0 seg_count 2
check lv_field $vg/${lv1}_rimage_1 seg_count 2

# Phase 2: Now fragment the remaining free space into small gaps for imeta
# The remaining free space should be ~70MB on each device
# Create 6MB fragments leaving 6MB gaps
for i in {1..6}; do
    lvcreate -n perm${i} -L 6M -an $vg "$dev1" 2>/dev/null || break
    lvcreate -n temp${i} -L 6M -an $vg "$dev1" 2>/dev/null || break
done

for i in {1..6}; do
    lvcreate -n perm${i}_2 -L 6M -an $vg "$dev2" 2>/dev/null || break
    lvcreate -n temp${i}_2 -L 6M -an $vg "$dev2" 2>/dev/null || break
done

# Remove temp LVs, leaving 6MB (3-extent) gaps
for i in {1..6}; do
    lvremove -y $vg/temp${i} 2>/dev/null || true
    lvremove -y $vg/temp${i}_2 2>/dev/null || true
done

pvs -o+pv_free,pv_used
lvs -a -o+devices,seg_count $vg

# Now try to add integrity
# With a 120MB LV, imeta needs ~8MB (4 extents with 2MB extent size)
# The fragmented free space (only 6MB/3-extent gaps) should cause imeta allocation
# to require multiple segments.
lvchange -an $vg/$lv1
lvconvert --raidintegrity y $vg/$lv1

lvs -a -o+devices $vg

lvchange -ay $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_verify_data_on_lv
lvextend -L+4M $vg/$lv1
_verify_data_on_lv
_replace_data_on_lv
lvconvert --raidintegrity n $vg/$lv1
_verify_data_on_lv

# Clean up
lvremove -y $vg/$lv1 || true

# Remove all fragment LVs
lvremove -y $vg/frag1 $vg/frag2 $vg/frag3 $vg/frag4 || true

# Remove all perm LVs
for i in {1..6}; do
    lvremove -y $vg/perm${i} 2>/dev/null || true
    lvremove -y $vg/perm${i}_2 2>/dev/null || true
done

vgremove -ff $vg
