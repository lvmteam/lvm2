#!/usr/bin/env bash

# Copyright (C) 2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Exercise cache flushing is abortable


SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 3 0 || skip

aux prepare_vg 2

SIZE_MB=4

# Data device on later delayed dev1
lvcreate -L4 -n cpool $vg "$dev1"
lvconvert -y --type cache-pool $vg/cpool "$dev2"
lvcreate -H -L $SIZE_MB -n $lv1 --chunksize 32k --cachemode writeback --cachepool $vg/cpool $vg "$dev2"

#
# Ensure cache gets promoted blocks
#
for i in $(seq 1 3) ; do
echo 3 >/proc/sys/vm/drop_caches
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=$SIZE_MB conv=fdatasync || true
echo 3 >/proc/sys/vm/drop_caches
dd if="$DM_DEV_DIR/$vg/$lv1" of=/dev/null bs=1M count=$SIZE_MB || true
done

aux delay_dev "$dev2" 0 300 "$(get first_extent_sector "$dev2"):"
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=$SIZE_MB

lvdisplay --maps $vg
# Delay dev to ensure we have some time to 'capture' interrupt in flush

# TODO, how to make writeback cache dirty
test "$(get lv_field $vg/$lv1 cache_dirty_blocks)" -gt 0 || {
	lvdisplay --maps $vg
	skip "Cannot make a dirty writeback cache LV."
}

LVM_TEST_TAG="kill_me_$PREFIX" lvconvert -vvvv --splitcache $vg/$lv1 >logconvert 2>&1 &
PID_CONVERT=$!
for i in {1..50}; do
	dmsetup table "$vg-$lv1" |& tee out
	grep cleaner out && break
	echo "$i: Waiting for cleaner policy on $vg/$lv1"
	sleep .05
done
test "$i" -ge 49 && die "Waited for cleaner policy on $vg/$lv1 too long!"

# While lvconvert updated table to 'cleaner' policy now it 
# should be running in 'Flushing' loop and just 1 KILL should
# cause abortion of flushing
kill -INT $PID_CONVERT
aux enable_dev "$dev2"
wait

#cat logconvert || true

# Problem of this test is, in older kernels, even the initial change to cleaner
# policy table line causes long suspend which in practice is cleaning all the
# dirty blocks - so the test can't really break the cache clearing.
#
# So the failure of test is reported only for recent kernels > 5.6
# ans skipped otherwise - as those can't be fixed anyway
grep -E "Flushing.*aborted" logconvert || {
	cat logconvert || true
	vgremove -f $vg
	aux kernel_at_least 5 6 || skip "Cache missed to abort flushing with older kernel"
	die "Flushing of $vg/$lv1 not aborted ?"
}

# check the table got restored
check grep_dmsetup table $vg-$lv1 "writeback"

vgremove -f $vg
