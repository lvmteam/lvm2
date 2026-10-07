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



. lib/inittest --skip-with-lvmpolld

aux have_cache 1 3 0 || skip
aux target_at_least dm-delay 1 1 0 || skip "Missing dm-delay target."

aux prepare_vg

# Slow origin writeback so SIGINT can land during flush (zero origin is instant).
ORIGIN_DELAY_MS=1000
SECTOR_SIZE=512
SECTORS_PER_MIB=2048
# dm-cache kernel status: dirty_blocks is field 14 (0-based index 13).
DM_CACHE_STATUS_DIRTY_IDX=13

ORIGIN_DEV=$dev3
ORIGIN_PE=$(( $(get pv_field "$ORIGIN_DEV" pv_pe_count) - $(get pv_field "$ORIGIN_DEV" pv_pe_alloc_count) ))
SIZE_MB=$(( ORIGIN_PE * SECTOR_SIZE / SECTORS_PER_MIB - 4 ))
test "$SIZE_MB" -gt 8 || SIZE_MB=8
lvcreate -L$((SIZE_MB * 2))M --type zero -n cpool $vg
lvconvert -y --type cache-pool --chunksize 32k $vg/cpool "$dev1"
lvcreate -l "$ORIGIN_PE" -n $lv1 $vg "$ORIGIN_DEV"
lvconvert -y -H --chunksize 32k --cachemode writeback --cachepool $vg/cpool $vg/$lv1

#
# Ensure cache gets promoted blocks
#
for i in $(seq 1 4) ; do
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=$SIZE_MB oflag=direct || true
dd if="$DM_DEV_DIR/$vg/$lv1" of=/dev/null bs=1M count=$SIZE_MB iflag=direct || true
done

aux delay_dev "$ORIGIN_DEV" 0 "$ORIGIN_DELAY_MS" "$(get first_extent_sector "$ORIGIN_DEV"):"
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=$SIZE_MB oflag=direct

lvdisplay --maps $vg

test "$(get lv_field $vg/$lv1 cache_dirty_blocks)" -gt 0 || {
	lvdisplay --maps $vg
	skip "Cannot make a dirty writeback cache LV."
}

# Keep the log in a regular file so readiness checks see all completed writes.
LVM_TEST_TAG="kill_me_$PREFIX" lvconvert -vvvv --splitcache $vg/$lv1 >logconvert 2>&1 &
PID_CONVERT=$!
sent_kill=0
# Allow slow cleaner reloads and udev waits before the interruptible wait.
deadline=$((SECONDS + 10))
while test "$SECONDS" -lt "$deadline"; do
	grep -q "Flushing.*aborted" logconvert && break
	kill -0 "$PID_CONVERT" 2>/dev/null || break
	out=$(dmsetup status --noflush "$vg-$lv1")
	# The first message precedes the cleaner reload and its udev wait.
	# The second confirms lvconvert itself has seen a dirty cleaner cache.
	if [[ "$out" =~ [[:space:]]cleaner[[:space:]] ]] &&
	   test "$(grep -c 'Flushing [0-9].* blocks for cache' logconvert)" -ge 2; then
		read -ra st <<< "$out"
		dirty=${st[DM_CACHE_STATUS_DIRTY_IDX]:-0}
		if test "$dirty" -gt 0; then
			# Retry across the short gaps where SIGINT is not enabled.
			if kill -INT "$PID_CONVERT" 2>/dev/null; then
				sent_kill=1
			fi
		fi
	fi
	sleep 0.01
done

# extra time in case we are in some slow 'flushing' suspend
sleep 0.5
aux enable_dev "$ORIGIN_DEV"
convert_rc=0
wait "$PID_CONVERT" || convert_rc=$?
cat logconvert

# A slow cleaner reload/udev wait can drain the cache before lvconvert reaches
# its interruptible wait, even on recent kernels.  A successful split with no
# second flushing message proves that this run did not exercise that wait.
if test "$convert_rc" -eq 0 &&
   test "$(grep -c 'Flushing [0-9].* blocks for cache' logconvert)" -lt 2 &&
   grep -q 'Flush complete\.' logconvert; then
	vgremove -f $vg
	skip "Cache became clean before lvconvert reached its interruptible wait."
fi

test "$sent_kill" -eq 1 || die "Did not reach an interruptible dirty cleaner cache on $vg/$lv1"
grep -E "Flushing.*aborted" logconvert || {
	vgremove -f $vg
	die "Flushing of $vg/$lv1 not aborted ?"
}
test "$convert_rc" -ne 0 || die "Aborted cache split unexpectedly succeeded"

# check the table got restored
check grep_dmsetup table $vg-$lv1 "writeback"
lvdisplay --maps $vg

vgremove -f $vg
