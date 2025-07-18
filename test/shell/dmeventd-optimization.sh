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

# Filling pools pver >50% to trigger monitoring thresholds
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=7 oflag=direct
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv2" bs=1M count=7 oflag=direct
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv3" bs=1M count=7 oflag=direct

sleep 11

test 50 -lt "$(get_pool_usage_ pool1)"
test 50 -lt "$(get_pool_usage_ pool2)"
test 50 -lt "$(get_pool_usage_ pool3)"

cat "$LOG"

#
# Count how many times the thin command was executed
#

# Expecting just 3 command executions
test 3 -eq "$(count_lvm_commands_)"

#
# Test 2: Multiple pool operations (triggers unmonitor/monitor cycles independently)
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

cat "$LOG"

# Still expect only 3 command executions
test 3 -eq "$(count_lvm_commands_)"

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

# Wait for all operations to complete
sleep 11

# Verify all pools are still monitored after optimization test"
check lv_field $vg/pool1 seg_monitor "monitored"
check lv_field $vg/pool2 seg_monitor "monitored"
check lv_field $vg/pool3 seg_monitor "monitored"

cat "$LOG"

# Count optimized executions -  still expecting only 3 command calls!
test 3 -eq "$(count_lvm_commands_)"


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
lvchange --monitor n $vg/pool1
lvchange --monitor n $vg/pool2
lvchange --monitor n $vg/pool3

sleep 2

check lv_field $vg/pool1 seg_monitor "not monitored"
check lv_field $vg/pool2 seg_monitor "not monitored"
check lv_field $vg/pool3 seg_monitor "not monitored"

# Re-enabling monitoring for all pools"
lvchange --monitor y $vg/pool1
lvchange --monitor y $vg/pool2
lvchange --monitor y $vg/pool3

check lv_field $vg/pool1 seg_monitor "monitored"
check lv_field $vg/pool2 seg_monitor "monitored"
check lv_field $vg/pool3 seg_monitor "monitored"


# Now wait less then 10 seconds (currently unconfigurable dmeventd timeout)
sleep 8

cat "$LOG"

# And there should be still only 3 command calls
# as the next call should happen within ~2 seconds
# and there was not reused 'graced' task
test 3 -eq "$(count_lvm_commands_)"

# Give some extra time so now initial command execution must have happened
sleep 3


cat "$LOG"

# Now validate new call happened
test 6 -eq "$(count_lvm_commands_)"

vgremove -ff $vg
