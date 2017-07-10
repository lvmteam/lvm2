#!/bin/sh
# Copyright (C) 2014-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Exercise cache flushing is abortable

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 3 0 || skip

aux prepare_vg 2

# Data device on later delayed dev1
lvcreate -L4 -n cpool $vg "$dev1"
lvconvert -y --type cache-pool $vg/cpool "$dev2"
lvcreate -H -L 4 -n $lv1 --chunksize 32k --cachemode writeback --cachepool $vg/cpool $vg "$dev2"

#
# Ensure cache gets promoted blocks
#
for i in $(seq 1 10) ; do
echo 3 >/proc/sys/vm/drop_caches
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=64K count=20 conv=fdatasync || true
echo 3 >/proc/sys/vm/drop_caches
dd if="$DM_DEV_DIR/$vg/$lv1" of=/dev/null bs=64K count=20 || true
done


# Delay dev to ensure we have some time to 'capture' interrupt in flush
aux delay_dev "$dev1" 0 500 "$(get first_extent_sector "$dev1"):"

lvdisplay --maps $vg
sync
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=4k count=100 conv=fdatasync

LVM_TEST_TAG="kill_me_$PREFIX" lvconvert -v --splitcache $vg/$lv1 >log 2>&1 &
PID_CONVERT=$!
sleep 0.2
kill -INT $PID_CONVERT
aux enable_dev "$dev1"
wait
egrep "Flushing.*aborted" log
# check the table got restored
check grep_dmsetup table $vg-$lv1 "writeback"

vgremove -f $vg
