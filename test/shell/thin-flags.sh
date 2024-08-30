#!/usr/bin/env bash

# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# test presence of various thin-pool/thin flags

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

#
# Main
#
aux have_thin 1 3 0 || skip
aux thin_pool_error_works_32 || skip

aux prepare_vg 2 256

###############################################
#  Testing failing thin-pool metadata device  #
###############################################

lvcreate -T -L1M --errorwhenfull y $vg/pool
lvcreate -V2 -n $lv2 $vg/pool

aux error_dev  "$dev2" 2054:2
# Check our 'lvs' is not flushing pool - should be still OK
check lv_attr_bit health $vg/pool "-"
# Enforce flush on thin pool device to notice error device.
dmsetup status $vg-pool-tpool
check lv_attr_bit health $vg/pool "F"
check lv_attr_bit health $vg/$lv2 "F"
aux enable_dev "$dev2"

lvchange -an $vg

# Overfill data area
lvchange -ay $vg
dd if=/dev/zero of="$DM_DEV_DIR/mapper/$vg-$lv2" bs=1M count=2 oflag=direct || true
check lv_attr_bit health $vg/pool "D"
# TODO use spaces ??
check lv_field $vg/pool lv_health_status "out_of_data"

lvremove -ff $vg


#######################################################
#  Testing what happens on system without thin-check  #
#######################################################

lvcreate -L200M --errorwhenfull y -T $vg/pool
lvcreate -V2 -n $lv2 $vg/pool
lvchange -an $vg

# Drop usage of  thin_check
aux lvmconf 'global/thin_check_executable = ""'

# Prepare some fake metadata prefilled to ~100%
lvcreate -L2 -n $lv1 $vg "$dev2" # tmp for metadata

VOLS=490
aux thin_restore_needs_more_volumes || VOLS=445
aux prepare_thin_metadata $VOLS 1 | tee data

# Note: we like want to test BOTH sizes (445 & 490) as ATM it gives
# different errors (5.9-rc5 kernel does not handle it as expected by this test)

"$LVM_TEST_THIN_RESTORE_CMD" -i data -o "$DM_DEV_DIR/mapper/$vg-$lv1"

# Swap volume with restored fake metadata
lvconvert -y --thinpool $vg/pool --poolmetadata $vg/$lv1

lvchange -ay $vg

lvchange -ay $vg/$lv2
# Provisioning and last free bits in metadata
dd if=/dev/zero of="$DM_DEV_DIR/mapper/$vg-$lv2" bs=1M count=1 oflag=direct || true

check lv_attr_bit health $vg/pool "M" || {
        echo "TEST ""WARNING: Missing metadata corruption for this version of thin-pool."
        exit 0
}

# TODO - use spaces ??
check lv_field $vg/pool lv_health_status "metadata_read_only"
check lv_attr_bit health $vg/$lv2 "-"

not lvcreate -s $vg/$lv2
not lvcreate -V10 -n $lv3 $vg/pool

lvs -ao+seg_pe_ranges $vg

# needs_check needs newer version
THINMINVER="1 20 0"
aux kernel_at_least 4 18 && THINMINVER="1 19 0" # kernel >=4.18 already had changes from 1.20

if aux have_thin $THINMINVER ; then
	check lv_attr_bit state $vg/pool "a"

	dmsetup suspend $vg-pool-tpool

	check lv_attr_bit state $vg/pool "s"

	dmsetup resume $vg-pool-tpool

	lvresize -L+2M $vg/pool_tmeta

	# Newer version recovers when metadata grow up
	check lv_attr_bit state $vg/pool "a"
	check lv_field $vg/pool lv_health_status ""

elif aux have_thin 1 16 0 ; then
	check lv_attr_bit state $vg/pool "c"
	check lv_field $vg/pool lv_check_needed "check needed"

	dmsetup suspend $vg-pool-tpool

	# suspended  thin-pool with Capital 'c'
	check lv_attr_bit state $vg/pool "C"

	dmsetup resume $vg-pool-tpool

	lvresize -L+2M $vg/pool_tmeta

	# still require thin_check
	check lv_attr_bit state $vg/pool "c"
fi

vgremove -ff $vg
