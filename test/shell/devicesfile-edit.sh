#!/usr/bin/env bash

# Copyright (C) 2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='devices file editing with lvmdevices'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux lvmconf 'devices/scan = "/dev"'

aux prepare_devs 1

# The tests run with system dir of "/etc" but lvm when running
# normally has cmd->system_dir set to "/etc/lvm".
DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"

aux lvmconf 'devices/use_devicesfile = 1'

losetup -h | grep sector-size || skip
which fallocate || skip

fallocate -l 2M loopa
fallocate -l 2M loopb

setup_loop_devs() {
	for i in {1..5} ; do
		LOOP1=$(losetup -f loopa --show || true)
        	test -n "$LOOP1" && break
	done
	for i in {1..5} ; do
        	LOOP2=$(losetup -f loopb --show || true)
        	test -n "$LOOP2" && break
	done
}

setup_loop_devs

# Tests of devices without PV on them.

# add/del with default idtype loop_file
lvmdevices --adddev "$LOOP1"
grep "$LOOP1" $DF
lvmdevices --adddev "$LOOP2"
grep "$LOOP2" $DF
grep "IDTYPE=loop_file" $DF
not grep "IDTYPE=devname" $DF
lvmdevices --deldev "$LOOP1"
not grep "$LOOP1" $DF
lvmdevices --deldev "$LOOP2"
not grep "$LOOP2" $DF

# add/del with non-default idtype devname
lvmdevices --adddev "$LOOP1" --deviceidtype devname
grep "$LOOP1" $DF
lvmdevices --adddev "$LOOP2" --deviceidtype devname
grep "$LOOP2" $DF
grep "IDTYPE=devname" $DF
not grep "IDTYPE=loop_file" $DF
lvmdevices --deldev "$LOOP1"
not grep "$LOOP1" $DF
lvmdevices --deldev "$LOOP2"
not grep "$LOOP2" $DF

# add/del when dev is missing, using default idtype
lvmdevices --adddev "$LOOP1"
grep "$LOOP1" $DF
lvmdevices --adddev "$LOOP2"
grep "$LOOP2" $DF
losetup -D
grep "$LOOP1" $DF
grep "$LOOP2" $DF
lvmdevices --deldev "$LOOP1"
not grep "$LOOP1" $DF
lvmdevices --deldev "$LOOP2"
not grep "$LOOP2" $DF
not lvmdevices --adddev "$LOOP1"
not lvmdevices --adddev "$LOOP2"
not grep "$LOOP1" $DF
not grep "$LOOP2" $DF
setup_loop_devs
rm $DF

# add/del when dev is missing, using devname idtype
lvmdevices --adddev "$LOOP1" --deviceidtype devname
grep "$LOOP1" $DF
lvmdevices --adddev "$LOOP2" --deviceidtype devname
grep "$LOOP2" $DF
losetup -D
grep "$LOOP1" $DF
grep "$LOOP2" $DF
lvmdevices --deldev "$LOOP1"
not grep "$LOOP1" $DF
lvmdevices --deldev "$LOOP2"
not grep "$LOOP2" $DF
setup_loop_devs
rm $DF

# Tests of devices with PV on them.

touch $DF
pvcreate "$LOOP1"
pvcreate "$LOOP2"
# PVID without dashes for matching devices file fields
PVID1=`pvs "$LOOP1" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID2=`pvs "$LOOP2" --noheading -o uuid | tr -d - | awk '{print $1}'`
# PVID with dashes for matching pvs -o+uuid output
OPVID1=`pvs "$LOOP1" --noheading -o uuid | awk '{print $1}'`
OPVID2=`pvs "$LOOP2" --noheading -o uuid | awk '{print $1}'`
grep "$LOOP1" $DF
grep "$LOOP2" $DF
grep "$PVID1" $DF
grep "$PVID2" $DF
rm $DF

# add/deldev with default idtype loop_file
lvmdevices --adddev "$LOOP1"
grep "$LOOP1" $DF
grep "$PVID1" $DF
lvmdevices --adddev "$LOOP2"
grep "$LOOP2" $DF
grep "$PVID2" $DF
grep "IDTYPE=loop_file" $DF
not grep "IDTYPE=devname" $DF
lvmdevices --deldev "$LOOP1"
not grep "$LOOP1" $DF
lvmdevices --deldev "$LOOP2"
not grep "$LOOP2" $DF
rm $DF

# deldev using idname
lvmdevices --adddev "$LOOP1"
lvmdevices --adddev "$LOOP2"
vgcreate $vg "$LOOP1" "$LOOP2"
IDNAME1=`pvs "$LOOP1" --noheading -o deviceid | awk '{print $1}'`
IDNAME2=`pvs "$LOOP2" --noheading -o deviceid | awk '{print $1}'`
lvmdevices --deldev "$IDNAME2" --deviceidtype loop_file
not grep "$IDNAME2" $DF
not grep "$LOOP2" $DF
lvmdevices --deldev "$IDNAME1" --deviceidtype loop_file
not grep "$IDNAME1" $DF
not grep "$LOOP1" $DF
lvmdevices --adddev "$LOOP1"
lvmdevices --adddev "$LOOP2"
vgremove $vg
rm $DF

# add/delpvid with default idtype loop_file
lvmdevices --addpvid "$PVID1"
grep "$LOOP1" $DF
grep "$PVID1" $DF
lvmdevices --addpvid "$PVID2"
grep "$LOOP2" $DF
grep "$PVID2" $DF
grep "IDTYPE=loop_file" $DF
not grep "IDTYPE=devname" $DF
lvmdevices --delpvid "$PVID1"
not grep "$LOOP1" $DF
not grep "$PVID1" $DF
lvmdevices --delpvid "$PVID2"
not grep "$LOOP2" $DF
not grep "$PVID2" $DF
rm $DF

# add/deldev with non-default idtype devname
lvmdevices --adddev "$LOOP1" --deviceidtype devname
grep "$LOOP1" $DF
grep "$PVID1" $DF
lvmdevices --adddev "$LOOP2" --deviceidtype devname
grep "$LOOP2" $DF
grep "$PVID2" $DF
grep "IDTYPE=devname" $DF
not grep "IDTYPE=loop_file" $DF
lvmdevices --deldev "$LOOP1"
not grep "$LOOP1" $DF
lvmdevices --deldev "$LOOP2"
not grep "$LOOP2" $DF
rm $DF

# add/delpvid with non-default idtype devname
lvmdevices --addpvid "$PVID1" --deviceidtype devname
grep "$LOOP1" $DF
grep "$PVID1" $DF
lvmdevices --addpvid "$PVID2" --deviceidtype devname
grep "$LOOP2" $DF
grep "$PVID2" $DF
grep "IDTYPE=devname" $DF
not grep "IDTYPE=loop_file" $DF
lvmdevices --deldev "$LOOP1"
not grep "$LOOP1" $DF
lvmdevices --deldev "$LOOP2"
not grep "$LOOP2" $DF
rm $DF

# add/deldev when dev is missing, using default idtype
lvmdevices --adddev "$LOOP1"
grep "$LOOP1" $DF
grep "$PVID1" $DF
lvmdevices --adddev "$LOOP2"
grep "$LOOP2" $DF
grep "$PVID2" $DF
losetup -D
grep "$LOOP1" $DF
grep "$LOOP2" $DF
lvmdevices --deldev "$LOOP1"
not grep "$LOOP1" $DF
not grep "$PVID1" $DF
lvmdevices --deldev "$LOOP2"
not grep "$LOOP2" $DF
not grep "$PVID2" $DF
setup_loop_devs
rm $DF

# add/delpvid when dev is missing, using devname idtype
lvmdevices --addpvid "$PVID1" --deviceidtype devname
grep "$LOOP1" $DF
grep "$PVID1" $DF
lvmdevices --addpvid "$PVID2" --deviceidtype devname
grep "$LOOP2" $DF
grep "$PVID2" $DF
losetup -D
grep "$LOOP1" $DF
grep "$LOOP2" $DF
lvmdevices --delpvid "$PVID1"
not grep "$LOOP1" $DF
not grep "$PVID1" $DF
lvmdevices --delpvid "$PVID2"
not grep "$LOOP2" $DF
not grep "$PVID2" $DF
setup_loop_devs
rm $DF

# test delnotfound
lvmdevices --addpvid "$PVID1"
echo "IDTYPE=sys_wwid IDNAME=naa.123 DEVNAME=/dev/sdx1 PVID=aaa PART=1" >> $DF
echo "IDTYPE=devname IDNAME=/dev/sdy DEVNAME=/dev/sdy PVID=bbb" >> $DF
lvmdevices
lvmdevices --update --delnotfound
not grep PVID=aaa $DF
not grep PVID=bbb $DF


# TODO: add/rem of partitions of same device

losetup -D
rm loopa loopb
