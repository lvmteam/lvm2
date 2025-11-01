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

. lib/inittest --skip-with-lvmpolld

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

# TODO: consider implementing prepare_iscsi_devs which
# uses targetcli to export files or devs, and use those
# exported devs locally so PR can be used.

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
		aux wipefs_a $dev
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
for dev in "${REAL_DEVICES[@]}"; do
	lvmpersist clear --ourkey 0x1000000000000001 --device "$dev" || true
done

vgcreate $vg "${REAL_DEVICES[@]}"

vgchange --setpersist y $vg
vgs -o persist $vg | grep 'require,autostart'

not vgchange --persist check $vg

not lvcreate -l1 $vg
not vgchange -ay $vg

vgchange --persist start $vg

lvcreate -l1 $vg
vgchange -an $vg
vgchange -ay $vg
vgchange -an $vg

vgchange --persist check $vg

vgchange --persist stop $vg

not vgchange --persist check $vg

not lvcreate -l1 $vg
not vgchange -ay $vg

vgchange --persist start $vg

vgchange --persist check $vg

for dev in "${REAL_DEVICES[@]}"; do
	lvmpersist check-key --key 0x1000000000000001 --device "$dev"
	lvmpersist read-reservation --device "$dev" | tee out
	grep "reservation: WE" out
done

vgchange --setpersist noautostart $vg
vgs -o persist $vg | grep 'require'
vgchange --setpersist autostart $vg
vgs -o persist $vg | grep 'require,autostart'
vgchange --setpersist norequire $vg
vgs -o persist $vg | grep 'autostart'
vgchange --setpersist noautostart $vg

vgchange --persist check $vg
vgchange --persist stop $vg

lvcreate -l1 -an $vg
vgchange -ay $vg
vgchange -an $vg

vgchange --setpersist y $vg
vgchange --persist start $vg
vgremove -y $vg

for dev in "${REAL_DEVICES[@]}"; do
	lvmpersist read --device "$dev" | tee out
	grep "registered keys: none" out
	grep "reservation: none" out
done

# try pr_key setting
aux lvmconf 'local/pr_key = "0xabcd1234"'
vgcreate $vg "${REAL_DEVICES[@]}"
vgchange --setpersist y $vg
vgchange --persist start $vg
vgchange --persist check $vg
lvcreate -l1 -an $vg

for dev in "${REAL_DEVICES[@]}"; do
	lvmpersist check-key --key 0xabcd1234 --device "$dev"
	lvmpersist read-reservation --device "$dev" | tee out
	grep "reservation: WE" out
done

vgremove -y $vg

rm $DF

