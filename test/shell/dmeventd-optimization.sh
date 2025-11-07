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

# Test dmeventd monitoring state optimization and check reduced command call

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest --skip-with-lvmpolld

test "${LVM_VALGRIND:-0}" -eq 0 || skip "Timing is too slow with valgrind."

LOG="$TESTDIR/command.log"

# TODO: make configurable monitoring timeout for 'dmeventd'
# so we do not need to waste so many seconds sleeping....


# Helper function to count LVM command executions
count_lvm_commands_() {
	grep -c "COMMAND executed" "$LOG" 2>/dev/null || true
}

# Helper function to get pool usage for any pool
get_pool_usage_() {
	local pool_name="${1:-pool}"
	get lv_field "$vg/$pool_name" data_percent | cut -d. -f1
}

# Helper function to simulate thin volume operations that trigger monitoring changes
simulate_thin_operations_() {
	local origin_name="$1"
	local num_volumes="$2"
	local prefix="${3:-thin}"

	# Creating $num_volumes snapshot thin volumes of $origin_name (prefix: $prefix).
	# This requires to suspend & resume origin volume and we want to avoid rescheduling
	# of extra  command call after this operation is finished
	for i in $(seq 1 "$num_volumes"); do
		lvcreate -s -n "${prefix}_$i" "$vg/$origin_name"
	done
}

# Main test
aux have_thin 1 0 0 || skip

# Create a custom command that logs execution
cat > command.sh << EOF
#!/bin/bash
(
echo "\$(date): COMMAND executed for \$1"
echo "Data: \$DMEVENTD_THIN_POOL_DATA"
echo "Metadata: \$DMEVENTD_THIN_POOL_METADATA"
) >> "$LOG"
EOF
chmod +x command.sh

# Configure dmeventd for testing
aux lvmconf "activation/thin_pool_autoextend_percent = 10" \
	    "activation/thin_pool_autoextend_threshold = 70" \
	    "dmeventd/thin_command = \"$PWD/command.sh\""

aux prepare_dmeventd
aux prepare_vg 1 80

# Clear log
> "$LOG"

#
# Create multiple thin pools for independent testing
#

# Creating multiple monitored thin pools (12M each)"
# and creating initial thin volumes (12M each = 100% of pool)"
lvcreate --monitor y -L12M -V12M -n $lv1 -T $vg/pool1
lvcreate --monitor y -L12M -V12M -n $lv2 -T $vg/pool2
lvcreate --monitor y -L12M -V12M -n $lv3 -T $vg/pool3

# Filling pools over >50% to trigger monitoring thresholds
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=7 oflag=direct
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv2" bs=1M count=7 oflag=direct
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv3" bs=1M count=7 oflag=direct

test 50 -lt "$(get_pool_usage_ pool1)"
test 50 -lt "$(get_pool_usage_ pool2)"
test 50 -lt "$(get_pool_usage_ pool3)"

#
# Multiple pool operations (triggers unmonitor/monitor cycles independently)
# We skip waiting for initial timeout - will verify timeout functionality
# at the end after grace period test
#
simulate_thin_operations_ $lv1 3 "test1"
simulate_thin_operations_ $lv2 3 "test2"
simulate_thin_operations_ $lv3 3 "test3"

# Wait for any pending dmeventd operations
sleep .3

# Verify monitoring is still active for all pools
check lv_field $vg/pool1 seg_monitor "monitored"
check lv_field $vg/pool2 seg_monitor "monitored"
check lv_field $vg/pool3 seg_monitor "monitored"

# At this point, no timeout has fired yet (we skipped the initial sleep 11)
# so command log should still be empty - verify optimization prevented
# any extra command calls during the operations

#
# Do some operations on multiple pools (should trigger optimization independently)
#
# Test each pool independently to verify optimization works per-pool
for round in {1..2}; do
	# Round $round: Testing independent pool operations"
	simulate_thin_operations_ $lv1 2 "opt1$round"
	simulate_thin_operations_ $lv2 2 "opt2$round"
	simulate_thin_operations_ $lv3 2 "opt3$round"

	# Small delay between rounds
	sleep .1
done

# Verify all pools are still monitored after optimization test
check lv_field $vg/pool1 seg_monitor "monitored"
check lv_field $vg/pool2 seg_monitor "monitored"
check lv_field $vg/pool3 seg_monitor "monitored"

# Still no timeout has fired, log should be empty
test 0 -eq "$(count_lvm_commands_)"


#
# Verify monitoring functionality preserved on all pools"
#
# Fill pools a bit more to test monitoring detection  (LVs are already snapshotted)
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=1 oflag=direct
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv2" bs=1M count=1 oflag=direct
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv3" bs=1M count=1 oflag=direct

# Check final pool status for all pools is now above 60%
test 60 -lt "$(get_pool_usage_ pool1)"
test 60 -lt "$(get_pool_usage_ pool2)"
test 60 -lt "$(get_pool_usage_ pool3)"

# Test that monitoring can be cleanly disabled and re-enabled for all pools
# With timeout reset behavior: when thread is reused from grace period,
# the timeout schedule is reset to ensure predictable monitoring intervals.

lvchange --monitor n $vg/pool1 $vg/pool2 $vg/pool3

check lv_field $vg/pool1 seg_monitor "not monitored"
check lv_field $vg/pool2 seg_monitor "not monitored"
check lv_field $vg/pool3 seg_monitor "not monitored"

sleep 2

# Record baseline before re-enabling
BASELINE_COUNT=$(count_lvm_commands_)

# Re-enabling monitoring for all pools after being in grace for 2 seconds
# When thread is reused from grace, timeout schedule is reset:
#   next_time = curr_time + timeout (10 seconds)
lvchange --monitor y $vg/pool1 $vg/pool2 $vg/pool3

check lv_field $vg/pool1 seg_monitor "monitored"
check lv_field $vg/pool2 seg_monitor "monitored"
check lv_field $vg/pool3 seg_monitor "monitored"

# Timeline with timeout reset behavior:
# - Thread was in grace period for 2 seconds
# - lvchange --monitor y reuses thread from grace
# - Timeout schedule reset: next_time = curr_time + 10s
# - Next monitoring check occurs 10 seconds after re-enable
# - No immediate check (appropriate since tool just checked status)

# Wait for the timeout to fire (10 seconds from re-enable + margin)
# This is the first time we wait for timeout, so this also validates:
# 1. Timeouts work at all (monitoring functionality)
# 2. Timeout reset after grace period works correctly
sleep 11

cat "$LOG"

# Validate that timeout-triggered calls happened
# Should have 3 calls (one per pool) - this is the first timeout fire
# and also validates the timeout reset after grace period
NEW_COUNT=$(count_lvm_commands_)
test "$NEW_COUNT" -eq 3 || {
    echo "Expected 3 timeout calls after re-enabling monitoring, but got $NEW_COUNT"
    echo "Baseline: $BASELINE_COUNT, New: $NEW_COUNT"
    cat "$LOG"
    exit 1
}

vgremove -ff $vg
