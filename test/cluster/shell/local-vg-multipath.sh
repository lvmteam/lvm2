#!/bin/bash
set -e

if [ "${CLUSTER_NUM_MULTIPATH:-0}" -lt 1 ]; then
    echo "SKIP: requires at least 1 multipath device"
    exit 0
fi

if [ -z "${mpath1:-}" ]; then
    echo "SKIP: \$mpath1 not set"
    exit 0
fi

# Discover multipath component (slave) devices via sysfs.
# Get the dm device name (e.g. dm-3) for the mpath device.
DM_NAME=$(node1 "basename \$(readlink -f $mpath1)")
SLAVES=$(node1 "ls /sys/block/$DM_NAME/slaves/")
COMP1=/dev/$(echo $SLAVES | awk '{print $1}')

# Get the mpath name (e.g. mpatha) for multipath commands
MPATH_NAME=$(node1 "dmsetup info -c --noheadings -o name $mpath1")

echo "mpath device: $mpath1"
echo "dm device: $DM_NAME"
echo "slaves: $SLAVES"
echo "first component: $COMP1"
echo "mpath name: $MPATH_NAME"

#
# Section 1: Multipath device identification
# Exercises: dev_is_mpath() dev-type.c, dm_uuid_has_prefix() dev-type.c
#

echo "== Section 1: Multipath device identification =="

# Verify mpath device has DM UUID with mpath- prefix
node1 "dmsetup info -c --noheadings -o uuid $mpath1 | grep '^mpath-'"

# Verify slaves directory contains component devices
node1 "test -d /sys/block/$DM_NAME/slaves"
node1 "test \$(ls /sys/block/$DM_NAME/slaves/ | wc -w) -ge 1"

# Verify each component has the mpath DM device in its holders directory
for slave in $SLAVES; do
    node1 "ls /sys/block/$slave/holders/ | grep -q $DM_NAME"
done

#
# Section 2: Component filtering via sysfs holders
# Exercises: _dev_is_mpath_component_sysfs() dev-mpath.c, _ignore_mpath_component() filter-mpath.c
#

echo "== Section 2: Component filtering via sysfs holders =="

node1 pvcreate -y $mpath1
node1 pvs $mpath1

# Each component device should be filtered out by sysfs holders detection
for slave in $SLAVES; do
    node1 not pvs /dev/$slave
done

node1 pvremove -y $mpath1

#
# Section 3: Component filtering via wwids file
# Exercises: _dev_in_wwid_file() dev-mpath.c, _read_wwid_file() dev-mpath.c
#

echo "== Section 3: Component filtering via wwids file =="

node1 pvcreate -y $mpath1

# Get the WWID before removing the mpath device
WWID=$(node1 "multipath -ll $MPATH_NAME 2>/dev/null | head -1 | awk '{print \$2}' | tr -d '()'" || true)
echo "WWID: $WWID"

# Remove the mpath DM device; this removes the sysfs holders
# but leaves the WWID in /etc/multipath/wwids
node1 "multipath -f $MPATH_NAME"
sleep 2

# Verify the wwids file still contains the WWID
if [ -n "$WWID" ]; then
    node1 "grep -q $WWID /etc/multipath/wwids"
fi

# Without sysfs holders, LVM should fall back to wwids file detection.
# Component devices should still be filtered.
for slave in $SLAVES; do
    node1 not pvs /dev/$slave
done

# Restore the mpath device
node1 "multipath -r"
sleep 2

# Verify the mpath device is back and usable
node1 "multipath -ll $MPATH_NAME 2>/dev/null | grep -q $MPATH_NAME"
node1 pvs $mpath1

node1 pvremove -y $mpath1

#
# Section 4: Rejected operations on components
# Exercises: component filtering prevents pvcreate/vgcreate/vgextend on slave devices
#

echo "== Section 4: Rejected operations on components =="

# pvcreate on a component device should fail
node1 not pvcreate -y $COMP1

# vgcreate on a component device should fail
node1 not vgcreate testvg $COMP1

# vgextend with a component device should fail
node1 pvcreate -y $mpath1
node1 vgcreate testvg $mpath1
node1 not vgextend testvg $COMP1

node1 vgremove -y testvg
node1 pvremove -y $mpath1

#
# Section 5: Basic LVM operations on multipath
# Exercises: full PV/VG/LV lifecycle on mpath devices
#

echo "== Section 5: Basic LVM operations on multipath =="

node1 pvcreate -y $mpath1
node1 pvs $mpath1

node1 vgcreate testvg $mpath1
node1 vgs testvg

node1 lvcreate -l5 -n lv1 testvg
node1 lvs testvg/lv1

node1 lvchange -an testvg/lv1
node1 lvchange -ay testvg/lv1

node1 lvchange -an testvg/lv1
node1 lvremove -y testvg/lv1
node1 vgremove -y testvg
node1 pvremove -y $mpath1

#
# Section 6: Devices file integration
# Exercises: dev_has_mpath_uuid() device_id.c, DEV_ID_TYPE_MPATH_UUID entries
#

echo "== Section 6: Devices file integration =="

node1 touch /etc/lvm/devices/system.devices
node1 pvcreate -y $mpath1
node1 vgcreate testvg $mpath1

# lvmdevices output should show mpath_uuid type
node1 "lvmdevices 2>/dev/null | grep mpath_uuid"

# devices file should have IDTYPE=mpath_uuid
node1 "grep 'IDTYPE=mpath_uuid' /etc/lvm/devices/system.devices"

# IDNAME should start with mpath-
node1 "grep 'IDTYPE=mpath_uuid' /etc/lvm/devices/system.devices | grep 'IDNAME=mpath-'"

# Component device names should not appear in the devices file
for slave in $SLAVES; do
    node1 "not grep -w $slave /etc/lvm/devices/system.devices"
done

# lvmdevices --update should be idempotent
node1 lvmdevices --update
node1 "lvmdevices 2>/dev/null | grep mpath_uuid"

node1 vgremove -y testvg
node1 pvremove -y $mpath1

#
# Section 7: Multiple multipath PVs
# Exercises: vgextend/vgreduce with mpath devices
#

if [ "${CLUSTER_NUM_MULTIPATH:-0}" -ge 2 ]; then
    echo "== Section 7: Multiple multipath PVs =="

    node1 pvcreate -y $mpath1 $mpath2
    node1 vgcreate testvg $mpath1
    node1 vgextend testvg $mpath2

    node1 "pvs --noheadings -o pv_name -S vg_name=testvg | wc -l | grep -q 2"

    node1 vgreduce testvg $mpath2
    node1 "pvs --noheadings -o pv_name -S vg_name=testvg | wc -l | grep -q 1"

    node1 vgremove -y testvg
    node1 pvremove -y $mpath1 $mpath2
else
    echo "== Section 7: SKIP (requires CLUSTER_NUM_MULTIPATH >= 2) =="
fi

#
# Section 8: multipath_component_detection config override
# Exercises: devices_multipath_component_detection_CFG, filter enable/disable path
#

echo "== Section 8: multipath_component_detection config override =="

node1 pvcreate -y $mpath1

# Default config: component is filtered
node1 not pvs $COMP1

# With detection disabled: component should be visible
# (read-only check, using --config for a single command only)
node1 "pvs --config 'devices { use_devicesfile=0 multipath_component_detection=0 filter=[\"a|.*|\"] global_filter=[\"a|.*|\"] }' $COMP1"

node1 pvremove -y $mpath1

#
# Section 9: Duplicate PV detection
# Exercises: _all_multipath_components() lvmcache.c
#

echo "== Section 9: Duplicate PV detection =="

node1 pvcreate -y $mpath1
node1 vgcreate testvg $mpath1

# Only 1 PV should appear for this VG despite PV header being on
# both the mpath device and all its component devices
node1 "pvs --noheadings -o pv_name -S vg_name=testvg | wc -l | grep -q 1"

# The listed PV should be the mpath device, not a component
node1 "pvs --noheadings -o pv_name -S vg_name=testvg | grep mapper"

node1 vgremove -y testvg
node1 pvremove -y $mpath1

echo "== All multipath tests passed =="

exit 0
