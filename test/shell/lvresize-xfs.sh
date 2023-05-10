#!/usr/bin/env bash

# Copyright (C) 2023 Red Hat, Inc. All rights reserved.
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

aux prepare_vg 1 500

which mkfs.xfs || skip
which xfs_growfs || skip

mount_dir="mnt_lvresize_fs"
mkdir -p "$mount_dir"

mount_dir_space="other mnt dir"
mkdir -p "$mount_dir_space"


# Test combinations of the following:
# lvreduce / lvextend
# xfs
# each --fs opt / no --fs opt / --resizefs
# active / inactive
# mounted / unmounted
# fs size less than, equal to or greater than reduced lv size

#################
#
# lvextend, xfs
#
#################

# lvextend, xfs, active, mounted, no --fs setting is same as --fs ignore
lvcreate -n $lv -L 300M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"

# --fs tests require a libblkid version that shows FSLASTBLOCK
# so exit 0 test here, if the feature is not present
blkid -p "$DM_DEV_DIR/$vg/$lv" | grep FSLASTBLOCK || skip

df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=10 oflag=direct
lvextend -L+20M $vg/$lv
check lv_field $vg/$lv lv_size "320.00m"
# with no --fs used, the fs size should be the same
df --output=size "$mount_dir" |tee df2
diff df1 df2
xfs_growfs "$DM_DEV_DIR/$vg/$lv"
df --output=size "$mount_dir" |tee df3
not diff df2 df3
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=20 oflag=direct
# keep it mounted

# lvextend, xfs, active, mounted, --fs ignore
df --output=size "$mount_dir" |tee df1
lvextend --fs ignore -L+20 $vg/$lv
check lv_field $vg/$lv lv_size "340.00m"
df --output=size "$mount_dir" |tee df2
diff df1 df2

# lvextend, xfs, active, mounted, --fs resize
lvextend --fs resize -L+20M $vg/$lv
check lv_field $vg/$lv lv_size "360.00m"
df --output=size "$mount_dir" |tee df3
not diff df2 df3

# lvextend, xfs, active, mounted, --resizefs (same as --fs resize)
lvextend --resizefs -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "370.00m"
df --output=size "$mount_dir" |tee df4
not diff df3 df4

# lvextend, xfs, active, mounted, --fs resize --fsmode manage (same as --fs resize)
lvextend --fs resize --fsmode manage -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "380.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2

umount "$mount_dir"
lvchange -an $vg/$lv

# lvextend, xfs, inactive, --fs ignore
lvextend --fs ignore -L+20M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"

lvremove -f $vg/$lv

####################
# start with new fs 
####################

# lvextend, xfs, active, mounted, --fs resize --fsmode offline
lvcreate -n $lv -L 300M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir_space"
df --output=size "$mount_dir_space" |tee df1
dd if=/dev/zero of="$mount_dir_space/zeros1" bs=1M count=20 oflag=direct
# xfs_growfs requires the fs to be mounted, so extending the lv is
# succeeds, then the xfs extend fails because it cannot be done unmounted
not lvextend --fs resize --fsmode offline -L+20M $vg/$lv
check lv_field $vg/$lv lv_size "320.00m"
df -a | tee dfa
grep "$mount_dir_space" dfa
df --output=size "$mount_dir_space" |tee df2
# fs not extended so fs size not changed
diff df1 df2

# lvextend, xfs, active, mounted, --fs resize --fsmode nochange
lvextend --fs resize --fsmode nochange -L+20M $vg/$lv
check lv_field $vg/$lv lv_size "340.00m"
df --output=size "$mount_dir_space" |tee df2
not diff df1 df2

# lvextend, xfs, active, mounted, --fs resize_fsadm
lvextend --fs resize_fsadm -L+20M $vg/$lv
check lv_field $vg/$lv lv_size "360.00m"
df --output=size "$mount_dir_space" |tee df3
not diff df2 df3
umount "$mount_dir_space"

# lvextend, xfs, active, unmounted, --fs resize --fsmode nochange
# xfs_growfs requires the fs to be mounted to grow, so --fsmode nochange
# with an unmounted fs fails
not lvextend --fs resize --fsmode nochange -L+20M $vg/$lv
check lv_field $vg/$lv lv_size "380.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir_space"
df --output=size "$mount_dir_space" |tee df4
# fs not extended so fs size not changed
diff df3 df4
umount "$mount_dir_space"

# lvextend, xfs, active, unmounted, --fs resize
# --yes needed because mount changes are required and plain "resize"
# fsopt did not specify if the user wants to change mount state
lvextend --yes --fs resize -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "390.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir_space"
df --output=size "$mount_dir_space" |tee df5
not diff df4 df5
umount "$mount_dir_space"

# lvextend, xfs, active, unmounted, --fs resize_fsadm
lvextend --fs resize_fsadm -L+10M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir_space"
df --output=size "$mount_dir_space" |tee df6
not diff df5 df6
umount "$mount_dir_space"
lvremove -f $vg/$lv


#################################################
#
# lvreduce, xfs (xfs does not support shrinking)
#
##################################################

# lvreduce, xfs, active, mounted, no --fs setting is same as --fs checksize
# fs smaller than the reduced size
lvcreate -n $lv -L 300M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
lvextend -L+100M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=20 oflag=direct
df --output=size "$mount_dir" |tee df1
# fs is 300M, reduced size is 326M, so no fs reduce is needed
lvreduce -L326M $vg/$lv
check lv_field $vg/$lv lv_size "326.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv

# lvreduce, xfs, inactive, no fs setting is same as --fs checksize
# fs smaller than the reduced size
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce --fs checksize -L316M $vg/$lv
check lv_field $vg/$lv lv_size "316.00m"
lvchange -ay $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2

# lvreduce, xfs, active, mounted, no --fs setting is same as --fs checksize
# fs equal to the reduced size
# fs is 300M, reduced size is 300M, so no fs reduce is needed
lvreduce -L300M $vg/$lv
check lv_field $vg/$lv lv_size "300.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2

# lvreduce, xfs, active, unmounted, no --fs setting is same as --fs checksize
# fs smaller than the reduced size
lvextend -L+100M $vg/$lv
umount "$mount_dir"
# fs is 300M, reduced size is 316M, so no fs reduce is needed
lvreduce -L356M $vg/$lv
check lv_field $vg/$lv lv_size "356.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2

# lvreduce, xfs, active, mounted, --fs resize
# fs smaller than the reduced size
# fs is 300M, reduced size is 316M, so no fs reduce is needed
lvreduce -L316M $vg/$lv
check lv_field $vg/$lv lv_size "316.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv


# lvreduce, xfs, inactive, no --fs setting is same as --fs checksize
# fs equal to the reduced size
# fs is 300M, reduced size is 300M, so no fs reduce is needed
lvreduce --fs checksize -L300M $vg/$lv
check lv_field $vg/$lv lv_size "300.00m"
lvchange -ay $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"

lvremove -f $vg/$lv


##########################################################
#
# lvreduce bigger xfs size (xfs does not support shrinking)
#
##########################################################

# lvreduce, xfs, active, mounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 400M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=20 oflag=direct
df --output=size "$mount_dir" |tee df1
# lvreduce fails because fs needs to be reduced
not lvreduce -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
# keep fs mounted

# lvreduce, xfs, active, mounted, --fs resize
# fs larger than the reduced size, fs not using reduced space
# lvreduce fails because xfs cannot shrink
not lvreduce --yes --fs resize -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"

# lvreduce, xfs, active, unmounted, --fs resize*
# fs larger than the reduced size, fs not using reduced space
# lvreduce fails because xfs cannot shrink
not lvreduce --yes --fs resize -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"
not lvreduce --yes --fs resize --fsmode manage -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"
not lvreduce --yes --fs resize --fsmode nochange -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"
not lvreduce --yes --fs resize --fsmode offline -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"
not lvreduce --yes --fs resize_fsadm -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"
not lvreduce --yes --resizefs -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"

mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv

# lvreduce, xfs, inactive, no --fs setting is same as --fs checksize
# fs larger than the reduced size
# lvreduce fails because fs needs to be reduced
not lvreduce -L-100M $vg/$lv
check lv_field $vg/$lv lv_size "400.00m"

vgremove -f $vg
