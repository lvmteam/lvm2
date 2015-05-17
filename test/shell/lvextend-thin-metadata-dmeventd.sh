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

# Test autoextension of thin metadata volume

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

meta_percent_() {
	get lv_field $vg/pool metadata_percent | cut -d. -f1
}

wait_for_change_() {
	# dmeventd only checks every 10 seconds :(
	for i in $(seq 1 15) ; do
		test "$(meta_percent_)" != "$1" && return
		sleep 1
	done

	return 1  # timeout
}

#
# Temporary solution to create some occupied thin metadata
# This heavily depends on thin metadata output format to stay as is.
# Currently it expects 2MB thin metadata and 200MB data volume size
# Argument specifies how many devices should be created.
fake_metadata_() {
	echo '<superblock uuid="" time="1" transaction="'$2'" data_block_size="128" nr_data_blocks="3200">'
	for i in $(seq 1 $1)
	do
		echo ' <device dev_id="'$i'" mapped_blocks="785" transaction="0" creation_time="0" snap_time="1">'
		echo '  <range_mapping origin_begin="0" data_begin="0" length="37" time="0"/>'
		echo ' </device>'
	done
	echo "</superblock>"
}

test -n "$LVM_TEST_THIN_RESTORE_CMD" || LVM_TEST_THIN_RESTORE_CMD=$(which thin_restore) || skip
"$LVM_TEST_THIN_RESTORE_CMD" -V || skip
aux have_thin 1 10 0 || skip

aux prepare_dmeventd

aux lvmconf "activation/thin_pool_autoextend_percent = 10" \
	    "activation/thin_pool_autoextend_threshold = 70"

aux prepare_pvs 3 256

vgcreate -s 1M $vg $(cat DEVICES)

# Testing dmeventd autoresize
lvcreate -L200M -V1G -n thin -T $vg/pool
lvcreate -L2M -n $lv1 $vg
lvchange -an $vg/thin $vg/pool

# Prepare some fake metadata with unmatching id
# Transaction_id is lower by 1 and there are no message -> ERROR
fake_metadata_ 10 0 >data
"$LVM_TEST_THIN_RESTORE_CMD" -i data -o "$DM_DEV_DIR/mapper/$vg-$lv1"
lvconvert -y --thinpool $vg/pool --poolmetadata $vg/$lv1
not vgchange -ay $vg 2>&1 | tee out
grep expected out

check inactive $vg pool_tmeta

# Transaction_id is higher by 1
fake_metadata_ 10 2 >data
"$LVM_TEST_THIN_RESTORE_CMD" -i data -o "$DM_DEV_DIR/mapper/$vg-$lv1"
lvconvert -y --thinpool $vg/pool --poolmetadata $vg/$lv1
not vgchange -ay $vg 2>&1 | tee out
grep expected out

check inactive $vg pool_tmeta

# Prepare some fake metadata prefilled to ~81% (>70%)
fake_metadata_ 400 1 >data
"$LVM_TEST_THIN_RESTORE_CMD" -i data -o "$DM_DEV_DIR/mapper/$vg-$lv1"

# Swap volume with restored fake metadata
lvconvert -y --thinpool $vg/pool --poolmetadata $vg/$lv1

vgchange -ay $vg

# Check dmeventd resizes metadata
pre=$(meta_percent_)
wait_for_change_ $pre

vgremove -f $vg
