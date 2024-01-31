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

test_description='devices file backups'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 3

aux lvmconf 'devices/use_devicesfile = 1'

# Stupid tests use plain /etc/ rather than /etc/lvm/
DFDIR="$LVM_SYSTEM_DIR/devices"
BKDIR="$LVM_SYSTEM_DIR/devices/backup"
mkdir -p "$DFDIR" || true
mkdir -p "$BKDIR" || true
DF="$DFDIR/system.devices"

vgcreate $vg "$dev1"
diff $DF $BKDIR/*.0001

pvcreate "$dev2"
diff $DF $BKDIR/*.0002

lvmdevices --deldev "$dev2"
diff $DF $BKDIR/*.0003

lvmdevices --adddev "$dev2"
diff $DF $BKDIR/*.0004

# DF update and backup when an entry is manually removed
cat $DF | grep -v "$dev2" > tmp1
cp tmp1 $DF
pvs
diff $DF $BKDIR/*.0005

lvmdevices --adddev "$dev2"
diff $DF $BKDIR/*.0006

# DF update and abckup when HASH value changes
sed -e "s|HASH=.......|HASH=1111111|" $DF > tmp1
cp tmp1 $DF
pvs
not grep "HASH=1111111" $DF
diff $DF $BKDIR/*.0007

# DF update and backup when old DF has no HASH value
cat $DF | grep -v HASH > tmp1
cp tmp1 $DF
pvs
grep HASH $DF
diff $DF $BKDIR/*.0008

# DF update and backup when dev names change
pvcreate "$dev3"
diff $DF $BKDIR/*.0009
grep "$dev2" $DF
grep "$dev3" $DF
dd if="$dev2" of=dev2_header bs=1M count=1
dd if="$dev3" of=dev3_header bs=1M count=1
dd if=dev2_header of="$dev3"
dd if=dev3_header of="$dev2"
pvs
diff $DF $BKDIR/*.0010

# backup limit, remove 1
aux lvmconf 'devices/devicesfile_backup_limit = 10'
lvmdevices --deldev "$dev2"
diff $DF $BKDIR/*.0011
not ls $BKDIR/*.0001

# backup limit, remove N
aux lvmconf 'devices/devicesfile_backup_limit = 5'
lvmdevices --adddev "$dev2"
diff $DF $BKDIR/*.0012
not ls $BKDIR/*.0002
not ls $BKDIR/*.0003
not ls $BKDIR/*.0004
not ls $BKDIR/*.0005
not ls $BKDIR/*.0006
not ls $BKDIR/*.0007
ls $BKDIR/*.0008
ls $BKDIR/*.0009
ls $BKDIR/*.0010
ls $BKDIR/*.0011

# backup disabled
aux lvmconf 'devices/devicesfile_backup_limit = 0'
lvmdevices --deldev "$dev2"
not ls $BKDIR/*.0013

# backup re-enabled
aux lvmconf 'devices/devicesfile_backup_limit = 5'
lvmdevices --adddev "$dev2"
ls $BKDIR/*.0014
not ls $BKDIR/*.0013

vgremove -ff $vg
