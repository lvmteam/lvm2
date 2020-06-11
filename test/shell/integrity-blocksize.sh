#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_integrity 1 5 0 || skip

losetup -h | grep sector-size || skip

# Tests with fs block sizes require a libblkid version that shows BLOCK_SIZE
aux prepare_devs 1
vgcreate $vg "$dev1"
lvcreate -n $lv1 -l8 $vg
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blkid "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE || skip
lvchange -an $vg
vgremove -ff $vg

dd if=/dev/zero of=loopa bs=$((1024*1024)) count=64 2> /dev/null
dd if=/dev/zero of=loopb bs=$((1024*1024)) count=64 2> /dev/null
dd if=/dev/zero of=loopc bs=$((1024*1024)) count=64 2> /dev/null
dd if=/dev/zero of=loopd bs=$((1024*1024)) count=64 2> /dev/null
LOOP1=$(losetup -f loopa --show)
LOOP2=$(losetup -f loopb --show)
LOOP3=$(losetup -f loopc --sector-size 4096 --show)
LOOP4=$(losetup -f loopd --sector-size 4096 --show)

echo $LOOP1
echo $LOOP2
echo $LOOP3
echo $LOOP4

aux extend_filter "a|$LOOP1|"
aux extend_filter "a|$LOOP2|"
aux extend_filter "a|$LOOP3|"
aux extend_filter "a|$LOOP4|"

aux lvmconf 'devices/scan = "/dev"'

mnt="mnt"
mkdir -p $mnt

vgcreate $vg1 $LOOP1 $LOOP2
vgcreate $vg2 $LOOP3 $LOOP4

# lvcreate on dev512, result 512
lvcreate --type raid1 -m1 --raidintegrity y -l 8 -n $lv1 $vg1
pvck --dump metadata $LOOP1 | grep 'block_size = 512'
lvremove -y $vg1/$lv1

# lvcreate on dev4k, result 4k
lvcreate --type raid1 -m1 --raidintegrity y -l 8 -n $lv1 $vg2
pvck --dump metadata $LOOP3 | grep 'block_size = 4096'
lvremove -y $vg2/$lv1

# lvcreate --bs 512 on dev4k, result fail
not lvcreate --type raid1 -m1 --raidintegrity y --raidintegrityblocksize 512 -l 8 -n $lv1 $vg2

# lvcreate --bs 4096 on dev512, result 4k
lvcreate --type raid1 -m1 --raidintegrity y --raidintegrityblocksize 4096 -l 8 -n $lv1 $vg1
pvck --dump metadata $LOOP1 | grep 'block_size = 4096'
lvremove -y $vg1/$lv1

# Test an unknown fs block size by simply not creating a fs on the lv.

# lvconvert on dev512, fsunknown, result 512
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
# clear any residual fs so that libblkid cannot find an fs block size
aux wipefs_a /dev/$vg1/$lv1
lvconvert --raidintegrity y $vg1/$lv1
pvck --dump metadata $LOOP1 | grep 'block_size = 512'
lvremove -y $vg1/$lv1

# lvconvert on dev4k, fsunknown, result 4k
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg2
# clear any residual fs so that libblkid cannot find an fs block size
aux wipefs_a /dev/$vg2/$lv1
lvconvert --raidintegrity y $vg2/$lv1
pvck --dump metadata $LOOP3 | grep 'block_size = 4096'
lvremove -y $vg2/$lv1

# lvconvert --bs 4k on dev512, fsunknown, result fail
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
# clear any residual fs so that libblkid cannot find an fs block size
aux wipefs_a /dev/$vg1/$lv1
not lvconvert --raidintegrity y --raidintegrityblocksize 4096 $vg1/$lv1
lvremove -y $vg1/$lv1

# lvconvert --bs 512 on dev4k, fsunknown, result fail
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg2
# clear any residual fs so that libblkid cannot find an fs block size
aux wipefs_a /dev/$vg2/$lv1
not lvconvert --raidintegrity y --raidintegrityblocksize 512 $vg2/$lv1
lvremove -y $vg2/$lv1

# lvconvert on dev512, xfs 512, result 512
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
aux wipefs_a /dev/$vg1/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg1/$lv1"
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"512\"
lvconvert --raidintegrity y $vg1/$lv1
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"512\"
mount "$DM_DEV_DIR/$vg1/$lv1" $mnt
umount $mnt
pvck --dump metadata $LOOP1 | grep 'block_size = 512'
lvremove -y $vg1/$lv1

# lvconvert on dev4k, xfs 4096, result 4096
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg2
aux wipefs_a /dev/$vg2/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg2/$lv1"
blkid "$DM_DEV_DIR/$vg2/$lv1" | grep BLOCK_SIZE=\"4096\"
lvconvert --raidintegrity y $vg2/$lv1
blkid "$DM_DEV_DIR/$vg2/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg2/$lv1" $mnt
umount $mnt
pvck --dump metadata $LOOP3 | grep 'block_size = 4096'
lvremove -y $vg2/$lv1

# lvconvert on dev512, ext4 1024, result 1024 
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
aux wipefs_a /dev/$vg1/$lv1
mkfs.ext4 -b 1024 "$DM_DEV_DIR/$vg1/$lv1"
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"1024\"
lvconvert --raidintegrity y $vg1/$lv1
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"1024\"
mount "$DM_DEV_DIR/$vg1/$lv1" $mnt
umount $mnt
pvck --dump metadata $LOOP1 | grep 'block_size = 1024'
lvremove -y $vg1/$lv1

# lvconvert on dev4k, ext4 4096, result 4096
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg2
aux wipefs_a /dev/$vg2/$lv1
mkfs.ext4 "$DM_DEV_DIR/$vg2/$lv1"
blkid "$DM_DEV_DIR/$vg2/$lv1" | grep BLOCK_SIZE=\"4096\"
lvconvert --raidintegrity y $vg2/$lv1
blkid "$DM_DEV_DIR/$vg2/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg2/$lv1" $mnt
umount $mnt
pvck --dump metadata $LOOP3 | grep 'block_size = 4096'
lvremove -y $vg2/$lv1

# lvconvert --bs 512 on dev512, xfs 4096, result 512
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
aux wipefs_a /dev/$vg1/$lv1
mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg1/$lv1"
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"4096\"
lvconvert --raidintegrity y --raidintegrityblocksize 512 $vg1/$lv1
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg1/$lv1" $mnt
umount $mnt
pvck --dump metadata $LOOP1 | grep 'block_size = 512'
lvremove -y $vg1/$lv1

# FIXME: kernel error reported, disallow this combination?
# lvconvert --bs 1024 on dev512, xfs 4096, result 1024
#lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
#aux wipefs_a /dev/$vg1/$lv1
#mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg1/$lv1"
#blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"4096\"
#lvconvert --raidintegrity y --raidintegrityblocksize 1024 $vg1/$lv1
#blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"4096\"
#mount "$DM_DEV_DIR/$vg1/$lv1" $mnt
#umount $mnt
#pvck --dump metadata $LOOP1 | grep 'block_size = 1024'
#lvremove -y $vg1/$lv1

# lvconvert --bs 512 on dev512, ext4 1024, result 512
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
aux wipefs_a /dev/$vg1/$lv1
mkfs.ext4 -b 1024 "$DM_DEV_DIR/$vg1/$lv1"
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"1024\"
lvconvert --raidintegrity y --raidintegrityblocksize 512 $vg1/$lv1
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"1024\"
mount "$DM_DEV_DIR/$vg1/$lv1" $mnt
umount $mnt
pvck --dump metadata $LOOP1 | grep 'block_size = 512'
lvremove -y $vg1/$lv1

# lvconvert --bs 512 on dev4k, ext4 4096, result fail
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg2
aux wipefs_a /dev/$vg2/$lv1
mkfs.ext4 "$DM_DEV_DIR/$vg2/$lv1"
not lvconvert --raidintegrity y --raidintegrityblocksize 512 $vg2/$lv1
lvremove -y $vg2/$lv1

# FIXME: need to use scsi_debug to create devs with LBS 512 PBS 4k
# FIXME: lvconvert, fsunknown, LBS 512, PBS 4k: result 512
# FIXME: lvconvert --bs 512, fsunknown, LBS 512, PBS 4k: result 512
# FIXME: lvconvert --bs 4k, fsunknown, LBS 512, PBS 4k: result 4k

# lvconvert on dev512, xfs 512, result 512, (detect fs with LV inactive)
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
aux wipefs_a /dev/$vg1/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg1/$lv1"
mount "$DM_DEV_DIR/$vg1/$lv1" $mnt
echo "test" > $mnt/test
umount $mnt
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"512\"
lvchange -an $vg1/$lv1
lvconvert --raidintegrity y $vg1/$lv1
lvchange -ay $vg1/$lv1
mount "$DM_DEV_DIR/$vg1/$lv1" $mnt
cat $mnt/test
umount $mnt
blkid "$DM_DEV_DIR/$vg1/$lv1" | grep BLOCK_SIZE=\"512\"
pvck --dump metadata $LOOP1 | grep 'block_size = 512'
lvchange -an $vg1/$lv1
lvremove -y $vg1/$lv1

# lvconvert on dev4k, xfs 4096, result 4096 (detect fs with LV inactive)
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg2
aux wipefs_a /dev/$vg2/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg2/$lv1"
mount "$DM_DEV_DIR/$vg2/$lv1" $mnt
echo "test" > $mnt/test
umount $mnt
blkid "$DM_DEV_DIR/$vg2/$lv1" | grep BLOCK_SIZE=\"4096\"
lvchange -an $vg2/$lv1
lvconvert --raidintegrity y $vg2/$lv1
lvchange -ay $vg2/$lv1
mount "$DM_DEV_DIR/$vg2/$lv1" $mnt
cat $mnt/test
umount $mnt
blkid "$DM_DEV_DIR/$vg2/$lv1" | grep BLOCK_SIZE=\"4096\"
pvck --dump metadata $LOOP3 | grep 'block_size = 4096'
lvchange -an $vg2/$lv1
lvremove -y $vg2/$lv1

vgremove -ff $vg1
vgremove -ff $vg2

losetup -d $LOOP1
losetup -d $LOOP2
losetup -d $LOOP3
losetup -d $LOOP4
rm loopa
rm loopb
rm loopc
rm loopd

