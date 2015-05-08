#!/bin/sh
# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#
# Checking consistency of old-snapshot metadata after de/activation
# Validates recent snapshot target kernel updates and error
# is triggered by kernel 3.14-rc[1..5]
# http://www.redhat.com/archives/dm-devel/2014-March/msg00005.html
#
. lib/inittest

test -e LOCAL_LVMPOLLD && skip

# Snapshot should remain unmodified
check_s_() {
	check dev_md5sum $vg s
	#diff data "$DM_DEV_DIR/$vg/s"
}

which md5sum || skip

aux prepare_vg

# 8M file with some random data
dd if=/dev/urandom of=data bs=1M count=1
dd if=data of=data bs=1M count=7 seek=1
echo "$(md5sum data | cut -d' ' -f1)  $DM_DEV_DIR/$vg/s" >md5.${vg}-s

lvcreate -aey -L 8M -n o $vg
dd if=data of="$DM_DEV_DIR/$vg/o" bs=1M

lvcreate -L 8M -s -n s $vg/o
check_s_

dd if=data of="$DM_DEV_DIR/$vg/o" bs=1234567 count=1 skip=1
check_s_
lvchange -an $vg

lvchange -ay $vg
check_s_

dd if=data of="$DM_DEV_DIR/$vg/o" bs=1234567 count=2 skip=1
check_s_

lvchange -an $vg
lvchange -ay $vg
check_s_

vgremove -f $vg
