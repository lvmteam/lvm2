#!/usr/bin/env bash

# Copyright (C) 2007-2023 Red Hat, Inc. All rights reserved.
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

which mkfs.ext4 || skip
which resize2fs || skip
which mkswap || skip

aux prepare_vg 2 100

#
# Blkid is not able to detected 'running' filesystem after resize
# freeeing and unfreezing stores fs metadata on disk
#
# Workaround for kernel bug fixed with:
#    a408f33e895e455f16cf964cb5cd4979b658db7b
# refreshing DM device - using fsfreeze with suspend
#
workaround_() {
	local vol=${1-$lv}
	blkid -p "$DM_DEV_DIR/$vg/$vol" >/dev/null || {
		dmsetup suspend $vg-$vol
		dmsetup resume $vg-$vol
	}
}

mount_dir="mnt_lvresize_fs"
mkdir -p "$mount_dir"

mount_dir_space="other mnt dir"
mkdir -p "$mount_dir_space"

mount_dir_2="mnt_lvresize_fs_2"
mkdir -p "$mount_dir_2"

# Test combinations of the following:
# lvreduce / lvextend
# no fs / ext4
# each --fs opt / no --fs opt / --resizefs
# active / inactive
# mounted / unmounted
# fs size less than, equal to or greater than reduced lv size


###################
#
# lvextend, no fs
#
###################

# lvextend, no fs, active, no --fs
lvcreate -n $lv -L 10M $vg
lvextend -L+20M $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"

# lvextend, no fs, active, --fs resize fails with no fs found
not lvextend -L+20M --fs resize $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"

lvchange -an $vg/$lv

# lvextend, no fs, inactive, --fs resize error requires active lv
not lvextend -L+20M --fs resize $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"

# lvextend, no fs, inactive, no --fs
lvextend -L+30M $vg/$lv
check lv_field $vg/$lv lv_size "60.00m"

lvremove -f $vg

# Without blkid being linked - no more tests
test "1" = "$(lvm lvmconfig --typeconfig default --valuesonly allocation/use_blkid_wiping)" || exit 0


###################
#
# lvreduce, no fs
#
###################

# lvreduce, no fs, active, no --fs setting is same as --fs checksize
lvcreate -n $lv -L 50M $vg
lvreduce -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "40.00m"
lvchange -an $vg/$lv

HAVE_FSINFO=1
aux have_fsinfo || HAVE_FSINFO=

if [ -n "$HAVE_FSINFO" ]; then

# lvreduce, no fs, inactive, no --fs setting is same as --fs checksize
not lvreduce -L-10M $vg/$lv
lvreduce --fs checksize -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"

# lvreduce, no fs, inactive, --fs ignore
lvreduce -L-10M --fs ignore $vg/$lv
check lv_field $vg/$lv lv_size "20.00m"

# lvreduce, no fs, active, --fs resize requires fs to be found
lvchange -ay $vg/$lv
not lvreduce -L-10M --fs resize $vg/$lv
check lv_field $vg/$lv lv_size "20.00m"

fi # HAVE_FSINFO

lvremove -f $vg/$lv


#################
#
# lvextend, ext4
#
#################

# Use one instance of ext4 for series of lvextend tests:
lvcreate -n $lv -L 20M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"

# --fs tests require a libblkid version that shows FSLASTBLOCK
# so exit 0 test here, if the feature is not present
blkid -p "$DM_DEV_DIR/$vg/$lv" | grep FSLASTBLOCK || exit 0


# lvextend, ext4, active, mounted, no --fs setting is same as --fs ignore
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=8 oflag=direct
lvextend -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"
# with no --fs used, the fs size should be the same
df --output=size "$mount_dir" |tee df2
diff df1 df2
resize2fs "$DM_DEV_DIR/$vg/$lv"
df --output=size "$mount_dir" |tee df3
not diff df2 df3
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=10 oflag=direct
# keep mounted fs

# lvextend, ext4, active, mounted, --fs ignore
df --output=size "$mount_dir" |tee df1
lvextend --fs ignore -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "40.00m"
df --output=size "$mount_dir" |tee df2
diff df1 df2
# keep mounted fs

# lvextend, ext4, active, mounted, --fs resize
workaround_

lvextend --fs resize -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "50.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
# keep mounted fs

workaround_

# lvextend, ext4, active, mounted, --resizefs (same as --fs resize)
df --output=size "$mount_dir" |tee df1
lvextend --resizefs -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "60.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
# keep mounted fs

if [ -n "$HAVE_FSINFO" ]; then

workaround_

# lvextend, ext4, active, mounted, --fs resize --fsmode manage (same as --fs resize)
df --output=size "$mount_dir" |tee df1
lvextend --fs resize --fsmode manage -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "70.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros3" bs=1M count=10 oflag=direct
# keep mounted fs

workaround_

# lvextend, ext4, active, mounted, --fs resize_fsadm
df --output=size "$mount_dir" |tee df1
lvextend --fs resize_fsadm -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "80.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
# keep mounted fs

workaround_

# lvextend, ext4, active, mounted, --fs resize --fsmode nochange
df --output=size "$mount_dir" |tee df1
lvextend --fs resize --fsmode nochange -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "90.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
# keep mounted fs

fi # HAVE_FSINFO

# lvextend|lvreduce, ext4, active, mounted, --fs resize, renamed LV
lvrename $vg/$lv $vg/$lv2
not lvextend --fs resize -L+32M $vg/$lv2
not lvreduce --fs resize -L-32M $vg/$lv2
umount "$mount_dir"

# lvextend|lvreduce, ext4, active, mounted, mount dir with space, --fs resize, renamed LV
mount "$DM_DEV_DIR/$vg/$lv2" "$mount_dir_space"
lvrename $vg/$lv2 $vg/$lv3
not lvextend --fs resize -L+32M $vg/$lv3
not lvreduce --fs resize -L-32M $vg/$lv3
umount "$mount_dir_space"

lvremove -f $vg/$lv3


#################################
#
# lvextend, ext4, multiple mounts
#
#################################

# Use one instance of ext4 for series of lvextend tests:
lvcreate -n $lv -L 32M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir_2"

# lvextend, ext4, active, mounted twice, -r
lvextend -r -L+8M $vg/$lv
check lv_field $vg/$lv lv_size "40.00m"

workaround_

lvrename $vg/$lv $vg/$lv2
not lvextend -r -L+8M $vg/$lv2
not lvreduce -r -L-8M $vg/$lv2
umount "$mount_dir"
umount "$mount_dir_2"
lvextend -r -L+8M $vg/$lv2

mount "$DM_DEV_DIR/$vg/$lv2" "$mount_dir"
mount --bind "$mount_dir" "$mount_dir_2"
lvextend -r -L+8M $vg/$lv2
check lv_field $vg/$lv2 lv_size "56.00m"
lvrename $vg/$lv2 $vg/$lv3
not lvextend -r -L+8M $vg/$lv3
not lvreduce -r -L-8M $vg/$lv3
umount "$mount_dir"
umount "$mount_dir_2"
mount "$DM_DEV_DIR/$vg/$lv3" "$mount_dir"
lvextend -r -L+8M $vg/$lv3

workaround_ $lv3

lvreduce -r -y -L-8M $vg/$lv3
umount "$mount_dir"

lvremove -f $vg/$lv3

# Following test require --fs checksize being compiled in
test -z "$HAVE_FSINFO" && exit 0

#####################################
#
# Now let do some unmounted tests
#
#####################################

# prepare new ext4 setup
lvcreate -n $lv -L 15M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"

# lvextend, ext4, inactive, --fs ignore
lvchange -an $vg/$lv
lvextend --fs ignore -L+5M $vg/$lv
check lv_field $vg/$lv lv_size "20.00m"
lvchange -ay $vg/$lv

# lvextend, ext4, active, mounted, --fs resize --fsmode offline
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=8 oflag=direct
lvextend --fs resize --fsmode offline -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"
# fsmode offline leaves fs unmounted
df | tee dfa
not grep "$mount_dir" dfa
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=10 oflag=direct
umount "$mount_dir"

# lvextend, ext4, active, unmounted, --fs resize --fsmode nochange
df --output=size "$mount_dir" |tee df1
lvextend --fs resize --fsmode nochange -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "40.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
umount "$mount_dir"

# lvextend, ext4, active, unmounted, --fs resize
lvextend --fs resize -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "50.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
umount "$mount_dir"

# lvextend, ext4, active, unmounted, --fs resize_fsadm
lvextend --fs resize_fsadm -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "60.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
umount "$mount_dir"

lvremove -f $vg/$lv


####################################################################
#
# lvreduce, ext4, no --fs setting and the equivalent --fs checksize
# i.e. fs is not resized
#
####################################################################

# lvreduce, ext4, active, mounted, no --fs setting is same as --fs checksize
# fs smaller than the reduced size
lvcreate -n $lv -L 20M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+25M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=10 oflag=direct
df --output=size "$mount_dir" |tee df1
# fs is 20M, reduced size is 27M, so no fs reduce is needed
# todo: check that resize2fs was not run?
lvreduce -L27M $vg/$lv
check lv_field $vg/$lv lv_size "27.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
# keep fs mounted

# lvreduce, ext4, active, mounted, no --fs setting is same as --fs checksize
# fs equal to the reduced size
lvreduce -L20M $vg/$lv
check lv_field $vg/$lv lv_size "20.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
# keep fs mounted

# lvreduce, ext4, active, mounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs not using reduced space
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce -L-18M $vg/$lv
check lv_field $vg/$lv lv_size "20.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
# keep fs mounted

# lvreduce, ext4, active, mounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs is using reduced space
not lvreduce -L-5M $vg/$lv
check lv_field $vg/$lv lv_size "20.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"

lvremove -f $vg/$lv


############################################################
#
# repeat lvreduce tests with unmounted instead of mounted fs
#
############################################################

# lvreduce, ext4, active, unmounted, no --fs setting is same as --fs checksize
# fs smaller than the reduced size
lvcreate -n $lv -L 20M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+25M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=10 oflag=direct
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# fs is 20M, reduced size is 27M, so no fs reduce is needed
lvreduce -L27M $vg/$lv
check lv_field $vg/$lv lv_size "27.00m"

# lvreduce, ext4, active, unmounted, no --fs setting is same as --fs checksize
# fs equal to the reduced size
# fs is 20M, reduced size is 20M, so no fs reduce is needed
lvreduce -L20M $vg/$lv
check lv_field $vg/$lv lv_size "20.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"


# lvreduce, ext4, active, unmounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs not using reduced space
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce -L-5M $vg/$lv

# lvreduce, ext4, active, unmounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs is using reduced space
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce -L-10M $vg/$lv

check lv_field $vg/$lv lv_size "20.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"

lvremove -f $vg


#########################################################################
#
# repeat a couple prev lvreduce that had no --fs setting,
# now using --fs checksize to verify it's the same as using no --fs set
#
#########################################################################

# lvreduce, ext4, active, mounted, --fs checksize (same as no --fs set)
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 100M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=20 oflag=direct
df --output=size "$mount_dir" |tee df1
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce --fs checksize -L-50M $vg/$lv
check lv_field $vg/$lv lv_size "100.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"

# lvreduce, ext4, active, unmounted, --fs checksize (same as no --fs set)
# fs smaller than the reduced size
lvextend -L+50M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"

# fs is 100M, reduced size is 120M, so no fs reduce is needed
lvreduce --fs checksize -L120M $vg/$lv
check lv_field $vg/$lv lv_size "120.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv

# lvreduce with inactive and no --fs setting fails because
# default behavior is fs checksize which activates the LV
# and sees the fs

# lvreduce, ext4, inactive, no --fs setting same as --fs checksize
# fs larger than the reduced size, fs not using reduced space
# lvreduce fails because default is --fs checksize which sees the fs
not lvreduce -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "120.00m"

lvremove -f $vg/$lv


#################################
#
# lvreduce, ext4, --fs resize*
#
#################################

# lvreduce, ext4, active, mounted, --fs resize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 50M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=10 oflag=direct
df --output=size "$mount_dir" |tee df1
# lvreduce runs resize2fs to shrink the fs
lvreduce --yes --fs resize -L-20M $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2

# lvreduce, ext4, active, mounted, --fs resize
# fs larger than the reduced size, fs is using reduced space
# lvreduce runs resize2fs to shrink the fs but resize2fs fails
# the fs is not remounted after resize2fs fails because the
# resize failure might leave the fs in an unknown state
not lvreduce --yes --fs resize -L-15M $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df3
# fs size is unchanged
diff df2 df3
umount "$mount_dir"

lvremove -f $vg/$lv


############################################
#
# repeat with unmounted instead of mounted
#
############################################

# lvreduce, ext4, active, unmounted, --fs resize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 50M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=10 oflag=direct
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce runs resize2fs to shrink the fs
lvreduce --fs resize -L-20M $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2
umount "$mount_dir"

# lvreduce, ext4, active, unmounted, --fs resize
# fs larger than the reduced size, fs is using reduced space
# lvreduce runs resize2fs to shrink the fs but resize2fs fails
not lvreduce --yes --fs resize -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "30.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df3
# fs size is unchanged
diff df2 df3
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv


##################################################################
#
# repeat resizes that shrink the fs, replacing --fs resize with
# --fs resize --fsmode nochange|offline, --fs resize_fsadm.
# while mounted and unmounted
#
##################################################################

# lvreduce, ext4, active, mounted, --fs resize --fsmode nochange
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 90M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=10 oflag=direct
df --output=size "$mount_dir" |tee df1
# lvreduce needs to unmount to run resize2fs but fsmode nochange doesn't let it
not lvreduce --fs resize --fsmode nochange -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "90.00m"
df --output=size "$mount_dir" |tee df2
# fs size is unchanged
diff df1 df2
umount "$mount_dir"

# lvreduce, ext4, active, unmounted, --fs resize --fsmode nochange
# fs larger than the reduced size, fs not using reduced space
# lvreduce runs resize2fs to shrink the fs
lvreduce --fs resize --fsmode nochange -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "80.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df3
# fs size is changed
not diff df2 df3
# keep fs mounted

# lvreduce, ext4, active, mounted, --fs resize --fsmode offline
# fs larger than the reduced size, fs not using reduced space
# lvreduce runs resize2fs to shrink the fs
# fsmode offline leaves the fs unmounted
lvreduce --fs resize --fsmode offline -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "70.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df4
# fs size is changed
not diff df3 df4
umount "$mount_dir"

# lvreduce, ext4, active, unmounted, --fs resize --fsmode offline
# fs larger than the reduced size, fs not using reduced space
# lvreduce runs resize2fs to shrink the fs
lvreduce --fs resize --fsmode offline -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "60.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df5
# fs size is changed
not diff df4 df5
# keep fs mounted

# lvreduce, ext4, active, mounted, --fs resize_fsadm
# fs larger than the reduced size, fs not using reduced space
# lvreduce runs resize2fs to shrink the fs
lvreduce --yes --fs resize_fsadm -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "50.00m"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df6
# fs size is changed
not diff df5 df6
umount "$mount_dir"

# lvreduce, ext4, active, unmounted, --fs resize_fsadm
# fs larger than the reduced size, fs not using reduced space
# lvreduce runs resize2fs to shrink the fs
lvreduce --fs resize_fsadm -L-10M $vg/$lv
check lv_field $vg/$lv lv_size "40.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df7
# fs size is changed
not diff df6 df7
umount "$mount_dir"

lvremove -f $vg

######################################
#
# lvreduce, lvextend with swap device
#
######################################

lvcreate -n $lv -L 16M $vg
mkswap /dev/$vg/$lv

# lvreduce not allowed if LV size < swap size
not lvreduce --fs checksize -L8m $vg/$lv
check lv_field $vg/$lv lv_size "16.00m"

# lvreduce not allowed if LV size < swap size,
# even with --fs resize, this is not supported
not lvreduce --fs resize $vg/$lv
check lv_field $vg/$lv lv_size "16.00m"

# lvextend allowed if LV size > swap size
lvextend -L32m $vg/$lv
check lv_field $vg/$lv lv_size "32.00m"

# lvreduce allowed if LV size == swap size
lvreduce -L16m $vg/$lv
check lv_field $vg/$lv lv_size "16.00m"

vgremove -ff $vg
