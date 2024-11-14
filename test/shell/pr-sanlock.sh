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

test_description='persistent reservations'

. lib/inittest

[ -z "$LVM_TEST_LOCK_TYPE_SANLOCK" ] && skip;

# Create a device and a VG that are both outside the scope of
# the standard lvm test suite so that they will not be removed
# and will remain in place while all the tests are run.
#
# Use this VG to hold the sanlock global lock which will be used
# by lvmlockd during other tests.
#
# This script will be run before any standard tests are run.
# After all the tests are run, another script will be run
# to remove this VG and device.

aux prepare_sanlock
aux prepare_lvmlockd

# To use this test, add devices which support PR to a file, e.g.
# $ cat /tmp/devs
# /dev/sdb
# /dev/sdc
#
# Specify this file as LVM_TEST_DEVICE_LIST=/tmp/devs
# when running the test.
#
# This test will wipe these devices.

if [ -z ${LVM_TEST_DEVICE_LIST+x} ]; then echo "LVM_TEST_DEVICE_LIST is unset" && skip; else echo "LVM_TEST_DEVICE_LIST is set to '$LVM_TEST_DEVICE_LIST'"; fi

test -e "$LVM_TEST_DEVICE_LIST" || skip

num_devs=$(wc -l < "$LVM_TEST_DEVICE_LIST")

aux prepare_real_devs

aux lvmconf 'devices/dir = "/dev"'
aux lvmconf 'devices/use_devicesfile = 1'
aux lvmconf 'local/host_id = 1'
DFDIR="$LVM_SYSTEM_DIR/devices"
DF="$DFDIR/system.devices"
mkdir $DFDIR || true
not ls $DF

get_real_devs

test $num_devs -gt 0 || skip

wipe_all() {
        for dev in "${REAL_DEVICES[@]}"; do
                wipefs -a $dev
        done
}

wipe_all

for dev in "${REAL_DEVICES[@]}"; do
        lvmdevices --adddev "$dev"
done

for dev in "${REAL_DEVICES[@]}"; do
        lvmpersist devtest --device "$dev"
done

# clear PR from a previous run
# dont know if prev run failed with key 10001 or 20001
for dev in "${REAL_DEVICES[@]}"; do
	lvmpersist clear --ourkey 0x1000000000010001 --device "$dev" || true
	lvmpersist clear --ourkey 0x1000000000020001 --device "$dev" || true
done

vgcreate --shared $vg "${REAL_DEVICES[@]}"

vgchange --setpersist y $vg
vgs -o persist $vg | grep 'require,autostart'

not vgchange --persist check $vg
vgchange --persist start $vg
vgchange --persist check $vg
vgchange --lockstart --lockopt nodelay $vg
lvmpersist check-key --key 0x1000000000010001 --device "$dev"
lvcreate -l1 $vg
vgchange -an $vg
not vgchange --persist stop $vg
vgchange --lockstop $vg
vgchange --persist stop $vg
vgchange --persist start $vg
lvmpersist check-key --key 0x1000000000020001 --device "$dev"
vgchange --persist check $vg | grep 0x1000000000020001
vgchange --lockstart $vg

# FIXME: add --lockopt nobusy to skip the other hosts check
# in vgremove / free_vg / lm_hosts
sanlock gets -h 1
sleep 20
sanlock gets -h 1

vgremove -y $vg

killall -9 lvmlockd
sanlock shutdown -f 1

