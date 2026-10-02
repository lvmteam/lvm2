#!/bin/bash
# Exercise lvmpersist command usage for devices in a local VG.
# Tests lvmpersist start/stop/clear/read/check-key/devtest commands
# with exclusive access (WE), the default for local VGs.
# Runs the full test sequence for each available device type
# (scsi, nvme, mpath).

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

KEY1=0x1001
KEY2=0x1002

# bb916d2efe86 removed unregister rollback from failed starts.  The tests
# previously relied on that cleanup; now they must verify retained keys
# and explicitly unregister them where needed.  A key is not ownership of WE.

# Build list of device types with >= 2 devices available
DEV_TYPES=""
[ "${CLUSTER_NUM_SCSI:-0}" -ge 2 ]     && DEV_TYPES="$DEV_TYPES scsi"
[ "${CLUSTER_NUM_NVME:-0}" -ge 2 ]     && DEV_TYPES="$DEV_TYPES nvme"
[ "${CLUSTER_NUM_MULTIPATH:-0}" -ge 2 ] && DEV_TYPES="$DEV_TYPES mpath"

if [ -z "$DEV_TYPES" ]; then
    echo "SKIP: no device type with >= 2 devices"
    exit 0
fi

for devtype in $DEV_TYPES; do
    case $devtype in
        scsi)  d1=$scsi1; d2=$scsi2 ;;
        nvme)  d1=$nvme1; d2=$nvme2 ;;
        mpath) d1=$mpath1; d2=$mpath2 ;;
    esac

    # Keep node1's default WEAR for multipath I/O: WE can reject writes or
    # cache flushes through another path on the same host.  Node2 requests
    # WE to conflict with node1's WEAR; parallel starts use WE to select
    # one reservation holder rather than allowing every registrant to join.
    WE_FOR_MPATH=""
    case $devtype in
        mpath) WE_FOR_MPATH="--prtype WE" ;;
    esac

    echo "=== Testing device type: $devtype ($d1, $d2) ==="

#
# Basic verification with a single device
#

node1 vgcreate testvg $d1
node1 lvcreate -l1 -n lv1 -an testvg

node1 lvmpersist devtest --vg testvg
node1 lvmpersist devtest --device $d1
node2 lvmpersist devtest --vg testvg
node2 lvmpersist devtest --device $d1

node1 lvmpersist start --ourkey $KEY1 --vg testvg
node1 lvmpersist read-keys --vg testvg | grep -q "0x1001"
node1 lvmpersist read-reservation --vg testvg | grep -qE "reservation: (WE |WEAR)"
node1 lvmpersist read --vg testvg | grep -q "0x1001"
node1 lvmpersist check-key --key $KEY1 --vg testvg
node1 not lvmpersist check-key --key $KEY2 --vg testvg
node2 lvmpersist read-keys --vg testvg | grep -q "0x1001"
node2 lvmpersist check-key --key $KEY1 --vg testvg

node1 lvmpersist stop --ourkey $KEY1 --vg testvg
node1 not lvmpersist check-key --key $KEY1 --vg testvg
node1 lvmpersist read-keys --vg testvg | grep -q "keys: none"
node1 lvmpersist read-reservation --vg testvg | grep -q "reservation: none"

node1 lvmpersist start --ourkey $KEY1 --access ex --vg testvg
node1 lvmpersist check-key --key $KEY1 --vg testvg
node1 lvmpersist stop --ourkey $KEY1 --vg testvg

node1 lvmpersist start --ourkey $KEY1 --prtype WE --vg testvg
node1 lvmpersist read-reservation --vg testvg | grep -q "reservation: WE "
node1 lvmpersist stop --ourkey $KEY1 --vg testvg

node1 lvmpersist start --ourkey $KEY1 --device $d1
node1 lvmpersist read-keys --device $d1 | grep -q "0x1001"
node1 lvmpersist read-reservation --device $d1 | grep -qE "reservation: (WE |WEAR)"
node1 lvmpersist check-key --key $KEY1 --device $d1
node1 lvmpersist stop --ourkey $KEY1 --device $d1
node1 not lvmpersist check-key --key $KEY1 --device $d1

node1 lvmpersist start --ourkey $KEY1 --vg testvg
node1 lvmpersist clear --ourkey $KEY1 --vg testvg
node1 lvmpersist read-keys --vg testvg | grep -q "keys: none"
node1 lvmpersist read-reservation --vg testvg | grep -q "reservation: none"

# A competing start fails to acquire WE but retains its registered key.
node1 lvmpersist start --ourkey $KEY1 --vg testvg
node2 not lvmpersist start --ourkey $KEY2 $WE_FOR_MPATH --vg testvg
# When node failed, it's key was still left registered, so remove it.
node1 lvmpersist check-key --key $KEY2 --vg testvg
node2 lvmpersist stop --ourkey $KEY2 --vg testvg
node1 not lvmpersist check-key --key $KEY2 --vg testvg
node1 lvchange -ay testvg/lv1
node2 lvchange -ay testvg/lv1
node1 dd if=/dev/zero of=/dev/testvg/lv1 bs=4096 count=1 oflag=direct,sync
node2 not dd if=/dev/zero of=/dev/testvg/lv1 bs=4096 count=1 oflag=direct,sync
node2 not dd if=/dev/zero of=$d1 bs=4096 count=1 oflag=direct,sync
node1 lvchange -an testvg/lv1
node2 lvchange -an testvg/lv1

# takeover: node2 preempts node1
node2 lvmpersist start --ourkey $KEY2 --removekey $KEY1 --vg testvg
node2 lvmpersist check-key --key $KEY2 --vg testvg
node2 not lvmpersist check-key --key $KEY1 --vg testvg
node1 not dd if=/dev/zero of=$d1 bs=4096 count=1 oflag=direct,sync
node2 lvmpersist stop --ourkey $KEY2 --vg testvg

node1 vgremove -ff testvg

# WEAR mode with single device
node1 lvmpersist clear --ourkey $KEY1 --device $d1 || true
node1 lvmpersist start --ourkey $KEY1 --access sh --device $d1
node2 lvmpersist start --ourkey $KEY2 --access sh --device $d1
node1 lvmpersist read-keys --device $d1 | grep -q "0x1001"
node1 lvmpersist read-keys --device $d1 | grep -q "0x1002"
node1 lvmpersist read-reservation --device $d1 | grep -q "reservation: WEAR"
node1 dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node2 dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node2 lvmpersist stop --ourkey $KEY2 --device $d1
node1 lvmpersist check-key --key $KEY1 --device $d1
node1 not lvmpersist check-key --key $KEY2 --device $d1
node1 dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node2 not dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node1 lvmpersist clear --ourkey $KEY1 --device $d1
node1 lvmpersist read-keys --device $d1 | grep -q "keys: none"
node1 lvmpersist read-reservation --device $d1 | grep -q "reservation: none"

# clean up lvm state so 2-device section starts fresh
nodes rm -f /etc/lvm/devices/system.devices
nodes rm -f /run/lvm/hints

#
# Two device tests
#

node1 vgcreate testvg $d1 $d2
node1 lvcreate -l1 -n lv1 -an testvg $d1
node1 lvcreate -l1 -n lv2 -an testvg $d2

#
# devtest: verify devices support PR
#
node1 lvmpersist devtest --vg testvg
node1 lvmpersist devtest --device $d1
node1 lvmpersist devtest --device $d2

node2 lvmpersist devtest --vg testvg
node2 lvmpersist devtest --device $d1
node2 lvmpersist devtest --device $d2

#
# start: register key and reserve with exclusive access (default WE)
#
node1 lvmpersist start --ourkey $KEY1 --vg testvg

#
# read / read-keys / read-reservation: verify output values
# reservation type is WE for scsi/nvme, WEAR for multipath
#
node1 lvmpersist read-keys --vg testvg | grep -q "0x1001"
node1 lvmpersist read-reservation --vg testvg | grep -qE "reservation: (WE |WEAR)"
node1 lvmpersist read --vg testvg | grep -q "0x1001"
node1 lvmpersist read --vg testvg | grep -qE "reservation: (WE |WEAR)"

#
# check-key: our key is registered, other key is not
#
node1 lvmpersist check-key --key $KEY1 --vg testvg
node1 not lvmpersist check-key --key $KEY2 --vg testvg

# node2 can also read PR state on the shared devices
node2 lvmpersist read-keys --vg testvg | grep -q "0x1001"
node2 lvmpersist read-reservation --vg testvg | grep -qE "reservation: (WE |WEAR)"
node2 lvmpersist check-key --key $KEY1 --vg testvg
node2 not lvmpersist check-key --key $KEY2 --vg testvg

#
# stop: unregister key, verify read output shows empty state
#
node1 lvmpersist stop --ourkey $KEY1 --vg testvg
node1 not lvmpersist check-key --key $KEY1 --vg testvg
node1 lvmpersist read-keys --vg testvg | grep -q "keys: none"
node1 lvmpersist read-reservation --vg testvg | grep -q "reservation: none"

#
# start again, then node2 start should fail (WE is exclusive)
#
node1 lvmpersist start --ourkey $KEY1 --vg testvg
node2 not lvmpersist start --ourkey $KEY2 $WE_FOR_MPATH --vg testvg
node1 lvmpersist check-key --key $KEY1 --vg testvg
# node2 failed to start but left its key registered
node1 lvmpersist check-key --key $KEY2 --vg testvg
node2 lvmpersist stop --ourkey $KEY2 --vg testvg
node1 not lvmpersist check-key --key $KEY2 --vg testvg

#
# PR affects the ability to write to devices
#
node1 lvchange -ay testvg/lv1 testvg/lv2
node2 lvchange -ay testvg/lv1 testvg/lv2

node1 dd if=/dev/zero of=/dev/testvg/lv1 bs=4096 count=1 oflag=direct,sync
node1 dd if=/dev/zero of=/dev/testvg/lv2 bs=4096 count=1 oflag=direct,sync

node2 not dd if=/dev/zero of=/dev/testvg/lv1 bs=4096 count=1 oflag=direct,sync
node2 not dd if=/dev/zero of=/dev/testvg/lv2 bs=4096 count=1 oflag=direct,sync
node2 not dd if=/dev/zero of=$d1 bs=4096 count=1 oflag=direct,sync
node2 not dd if=/dev/zero of=$d2 bs=4096 count=1 oflag=direct,sync

node1 lvchange -an testvg/lv1 testvg/lv2
node2 lvchange -an testvg/lv1 testvg/lv2

#
# takeover: node2 start --removekey preempts node1
#
node2 lvmpersist start --ourkey $KEY2 --removekey $KEY1 --vg testvg
node2 lvmpersist check-key --key $KEY2 --vg testvg
node2 not lvmpersist check-key --key $KEY1 --vg testvg
node2 lvmpersist read-keys --vg testvg | grep -q "0x1002"
node2 lvmpersist read-reservation --vg testvg | grep -qE "reservation: (WE |WEAR)"

# after takeover, node1 cannot write, node2 can
node1 not dd if=/dev/zero of=$d1 bs=4096 count=1 oflag=direct,sync
node1 not dd if=/dev/zero of=$d2 bs=4096 count=1 oflag=direct,sync

node2 lvchange -ay testvg/lv1 testvg/lv2
node2 dd if=/dev/zero of=/dev/testvg/lv1 bs=4096 count=1 oflag=direct,sync
node2 dd if=/dev/zero of=/dev/testvg/lv2 bs=4096 count=1 oflag=direct,sync
node2 lvchange -an testvg/lv1 testvg/lv2

node2 lvmpersist stop --ourkey $KEY2 --vg testvg

#
# clear: removes all keys and reservations
#
node1 lvmpersist start --ourkey $KEY1 --vg testvg
node1 lvmpersist check-key --key $KEY1 --vg testvg
node1 lvmpersist clear --ourkey $KEY1 --vg testvg
node1 not lvmpersist check-key --key $KEY1 --vg testvg
node1 lvmpersist read-keys --vg testvg | grep -q "keys: none"
node1 lvmpersist read-reservation --vg testvg | grep -q "reservation: none"

#
# start with explicit --access ex
#
node1 lvmpersist start --ourkey $KEY1 --access ex --vg testvg
node1 lvmpersist check-key --key $KEY1 --vg testvg
node1 lvmpersist read-reservation --vg testvg | grep -qE "reservation: (WE |WEAR)"
node1 lvmpersist stop --ourkey $KEY1 --vg testvg

#
# start with explicit --prtype WE
#
node1 lvmpersist start --ourkey $KEY1 --prtype WE --vg testvg
node1 lvmpersist check-key --key $KEY1 --vg testvg
node1 lvmpersist read-reservation --vg testvg | grep -q "reservation: WE "
node1 lvmpersist stop --ourkey $KEY1 --vg testvg

#
# use --device directly instead of --vg
#
node1 lvmpersist start --ourkey $KEY1 --device $d1 --device $d2
node1 lvmpersist read-keys --device $d1 | grep -q "0x1001"
node1 lvmpersist read-reservation --device $d1 | grep -qE "reservation: (WE |WEAR)"
node1 lvmpersist check-key --key $KEY1 --device $d1
node1 lvmpersist check-key --key $KEY1 --device $d2
node1 lvmpersist stop --ourkey $KEY1 --device $d1 --device $d2
node1 not lvmpersist check-key --key $KEY1 --device $d1

#
# use --device with --vg as label
#
node1 lvmpersist start --ourkey $KEY1 --vg testvg --device $d1 --device $d2
node1 lvmpersist check-key --key $KEY1 --device $d1
node1 lvmpersist stop --ourkey $KEY1 --vg testvg --device $d1 --device $d2

#
# parallel devtest: all nodes can test PR support concurrently
#
nodep lvmpersist devtest --vg testvg
assert_all_success

#
# parallel read: all nodes can read PR state concurrently
#
node1 lvmpersist start --ourkey $KEY1 --vg testvg

nodep lvmpersist read --vg testvg
assert_all_success

nodep lvmpersist read-keys --vg testvg
assert_all_success

nodep lvmpersist read-reservation --vg testvg
assert_all_success

#
# parallel check-key: all nodes can check keys concurrently
#
nodep lvmpersist check-key --key $KEY1 --vg testvg
assert_all_success

nodep lvmpersist check-key --key $KEY2 --vg testvg || true
assert_all_fail

node1 lvmpersist stop --ourkey $KEY1 --vg testvg

#
# parallel start race: WE reservation is exclusive, only one node wins
# each node uses its own key 0x100N
#
for i in $(seq 1 5); do
    nodep 'lvmpersist start --ourkey 0x100${NODE_NUM} '"$WE_FOR_MPATH"' --vg testvg' || true
    assert_one_success

    success_node 'lvmpersist check-key --key 0x100${NODE_NUM} --vg testvg'

    for node_num in $NODEP_FAIL_NODES; do
        # Losing the WE race leaves registrations but does not allow writes.
        noden ${node_num} lvmpersist check-key --key 0x100${node_num} --vg testvg
        noden ${node_num} not dd if=/dev/zero of=$d1 bs=4096 count=1 oflag=direct,sync
        noden ${node_num} not dd if=/dev/zero of=$d2 bs=4096 count=1 oflag=direct,sync
        # Remove losing keys before the winner stops, leaving the next race clean.
        noden ${node_num} lvmpersist stop --ourkey 0x100${node_num} --vg testvg
    done

    success_node 'lvmpersist stop --ourkey 0x100${NODE_NUM} --vg testvg'

    sleep 0.5
done

#
# one node can take over
#
node1 lvmpersist start --ourkey $KEY1 --vg testvg

node2 'lvmpersist start --ourkey 0x100${NODE_NUM} --removekey 0x1001 '"$WE_FOR_MPATH"' --vg testvg'
node2 'lvmpersist check-key --key 0x100${NODE_NUM} --vg testvg'

if [ "$CLUSTER_NUM_NODES" -gt 2 ]; then
    for node_num in $(seq 3 "$CLUSTER_NUM_NODES"); do
        noden ${node_num} not lvmpersist check-key --key 0x100${node_num} --vg testvg
    done
fi

node2 lvmpersist stop --ourkey $KEY2 --vg testvg

#
# parallel start race repeated with clear between iterations
#
for i in $(seq 1 3); do
    nodep 'lvmpersist start --ourkey 0x100${NODE_NUM} '"$WE_FOR_MPATH"' --vg testvg' || true
    assert_one_success

    success_node 'lvmpersist clear --ourkey 0x100${NODE_NUM} --vg testvg'

    sleep 0.5
done

#
# remove with missing device: must fail because fencing cannot be
# complete if a PV device is missing (target may retain its PR key).
#
node1 lvmpersist start --ourkey $KEY1 --access sh --vg testvg
node2 lvmpersist start --ourkey $KEY2 --access sh --vg testvg
node1 lvmpersist check-key --key $KEY1 --vg testvg
node1 lvmpersist check-key --key $KEY2 --vg testvg

# Populate system.devices explicitly.  vgcreate does not create it when the
# node already has an OS VG, and without it LVM would continue to scan d2.
node1 vgimportdevices testvg

# remove d2 from system.devices on node1 so it appears as [unknown]
node1 lvmdevices --deldev $d2

# remove must fail when a VG device is missing
node1 not lvmpersist remove --ourkey $KEY1 --removekey $KEY2 --vg testvg

# key was removed from available d1, but remains on missing d2
node1 not lvmpersist check-key --key $KEY2 --device $d1
node2 lvmpersist check-key --key $KEY2 --device $d2

# restore d2 in system.devices, retry removes key from d2
node1 lvmdevices --adddev $d2
node1 lvmpersist remove --ourkey $KEY1 --removekey $KEY2 --vg testvg
node1 not lvmpersist check-key --key $KEY2 --vg testvg

# verify node2 can no longer write
node2 not dd if=/dev/zero of=$d1 bs=4096 count=1 oflag=direct,sync
node2 not dd if=/dev/zero of=$d2 bs=4096 count=1 oflag=direct,sync

node1 lvmpersist stop --ourkey $KEY1 --vg testvg

# Cleanup VG
node1 vgremove -ff testvg

#
# WEAR mode: all started nodes can write, stopped nodes cannot.
# Uses --device only (no VG involved).
#
node1 lvmpersist clear --ourkey $KEY1 --device $d1 --device $d2 || true

node1 lvmpersist start --ourkey $KEY1 --access sh --device $d1 --device $d2
node2 lvmpersist start --ourkey $KEY2 --access sh --device $d1 --device $d2

# verify WEAR reservation and both keys registered
node1 lvmpersist read-keys --device $d1 | grep -q "0x1001"
node1 lvmpersist read-keys --device $d1 | grep -q "0x1002"
node1 lvmpersist read-reservation --device $d1 | grep -q "reservation: WEAR"

# both nodes can write
node1 dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node2 dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node1 dd if=/dev/zero of=$d2 bs=4096 count=1 seek=256 oflag=direct,sync
node2 dd if=/dev/zero of=$d2 bs=4096 count=1 seek=256 oflag=direct,sync

# node2 stops
node2 lvmpersist stop --ourkey $KEY2 --device $d1 --device $d2

# verify only KEY1 remains
node1 lvmpersist check-key --key $KEY1 --device $d1
node1 not lvmpersist check-key --key $KEY2 --device $d1
node1 lvmpersist read-keys --device $d1 | grep -q "0x1001"

# node1 can still write
node1 dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node1 dd if=/dev/zero of=$d2 bs=4096 count=1 seek=256 oflag=direct,sync

# node2 cannot write
node2 not dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node2 not dd if=/dev/zero of=$d2 bs=4096 count=1 seek=256 oflag=direct,sync

# node2 restarts, can write again
node2 lvmpersist start --ourkey $KEY2 --access sh --device $d1 --device $d2
node2 dd if=/dev/zero of=$d1 bs=4096 count=1 seek=256 oflag=direct,sync
node2 dd if=/dev/zero of=$d2 bs=4096 count=1 seek=256 oflag=direct,sync

# clean up all PR state
node1 lvmpersist clear --ourkey $KEY1 --device $d1 --device $d2
node1 lvmpersist read-keys --device $d1 | grep -q "keys: none"
node1 lvmpersist read-reservation --device $d1 | grep -q "reservation: none"

# clean up lvm state for next iteration
nodes rm -f /etc/lvm/devices/system.devices
nodes rm -f /run/lvm/hints

done

# ============================================================
# All devices: basic lvmpersist operations on a VG using all imported devices
# ============================================================

ALL_DEVS=""
for i in $(seq 1 $CLUSTER_DEV_COUNT); do
    ALL_DEVS="$ALL_DEVS $(eval echo \$dev$i)"
done

node1 vgcreate testvg $ALL_DEVS

node1 lvmpersist devtest --vg testvg

node1 lvmpersist start --ourkey $KEY1 --vg testvg
node1 lvmpersist read-reservation --vg testvg | grep -qE "reservation: (WE |WEAR)"

for i in $(seq 1 $CLUSTER_DEV_COUNT); do
    node1 lvmpersist check-key --key $KEY1 --device $(eval echo \$dev$i)
done

node1 lvmpersist stop --ourkey $KEY1 --vg testvg

for i in $(seq 1 $CLUSTER_DEV_COUNT); do
    node1 lvmpersist read-keys --device $(eval echo \$dev$i) | grep -q "keys: none"
done

node1 lvmpersist start --ourkey $KEY1 --vg testvg
node2 lvmpersist start --ourkey $KEY2 --removekey $KEY1 --vg testvg
node2 lvmpersist check-key --key $KEY2 --vg testvg
node2 not lvmpersist check-key --key $KEY1 --vg testvg

for i in $(seq 1 $CLUSTER_DEV_COUNT); do
    node2 lvmpersist check-key --key $KEY2 --device $(eval echo \$dev$i)
    node2 not lvmpersist check-key --key $KEY1 --device $(eval echo \$dev$i)
done

node2 lvmpersist stop --ourkey $KEY2 --vg testvg
node1 vgremove -ff testvg

exit 0
