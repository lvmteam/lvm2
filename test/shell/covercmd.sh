#!/bin/sh
# Copyright (C) 2008-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#
# tests basic functionality of read-ahead and ra regressions
#

. lib/test

aux prepare_devs 5

TEST_UUID="aaaaaa-aaaa-aaaa-aaaa-aaaa-aaaa-aaaaaa"

pvcreate "$dev1"
pvcreate --metadatacopies 0 "$dev2"
pvcreate --metadatacopies 0 "$dev3"
pvcreate "$dev4"
pvcreate --norestorefile -u $TEST_UUID --metadatacopies 0 "$dev5"
vgcreate $vg $(cat DEVICES)
lvcreate -l 5 -i5 -I256 -n $lv $vg

lvcreate -aey -l 5 -n $lv1 $vg
lvcreate -s -l 5 -n $lv2 $vg/$lv1

if aux have_readline; then
# test *scan and *display tools
cat <<EOF | lvm
pvscan --uuid
vgscan --mknodes
lvscan
lvmdiskscan
vgdisplay --units k $vg
lvdisplay --units g $vg
pvdisplay -c "$dev1"
pvdisplay -s "$dev1"
vgdisplay -c $vg
vgdisplay -C $vg
vgdisplay -s $vg
lvdisplay -c $vg
lvdisplay -C $vg
lvdisplay -m $vg
EOF

for i in h b s k m g t p e H B S K M G T P E; do
	echo pvdisplay --units $i "$dev1"
done | lvm
else
pvscan --uuid
vgscan --mknodes
lvscan
lvmdiskscan
vgdisplay --units k $vg
lvdisplay --units g $vg
pvdisplay -c "$dev1"
pvdisplay -s "$dev1"
vgdisplay -c $vg
vgdisplay -C $vg
vgdisplay -s $vg
lvdisplay -c $vg
lvdisplay -C $vg
lvdisplay -m $vg

for i in h b s k m g t p e H B S K M G T P E; do
	pvdisplay --units $i "$dev1"
done
fi

not lvdisplay -C -m $vg
not lvdisplay -c -v $vg
not lvdisplay --aligned $vg
not lvdisplay --noheadings $vg
not lvdisplay --options lv_name $vg
not lvdisplay --separator : $vg
not lvdisplay --sort size $vg
not lvdisplay --unbuffered $vg

not vgdisplay -C -A
not vgdisplay -C -c
not vgdisplay -C -s
not vgdisplay -c -s
not vgdisplay -A $vg1

# "-persistent y --major 254 --minor 20"
# "-persistent n"
# test various lvm utils
for i in dumpconfig devtypes formats segtypes tags; do
	lvm $i
done

for i in pr "p rw" an ay "-monitor y" "-monitor n" \
        -refresh "-addtag MYTAG" "-deltag MYETAG"; do
	lvchange -$i $vg/$lv
done

pvck "$dev1"
vgs -o all $vg
lvrename $vg $lv $lv-rename
vgcfgbackup -f backup.$$ $vg
vgchange -an $vg
vgcfgrestore  -f backup.$$ $vg
pvremove -y -ff "$dev5"
not vgcfgrestore  -f backup.$$ $vg
pvcreate -u $TEST_UUID --restorefile  backup.$$ "$dev5"
vgremove -f $vg

# test pvresize functionality
# missing params
not pvresize
# negative size
not pvresize --setphysicalvolumesize -10M "$dev1"
# not existing device
not pvresize --setphysicalvolumesize 10M "$dev7"
pvresize --setphysicalvolumesize 10M "$dev1"
pvresize "$dev1"

# test various errors and obsoleted tools
not lvmchange
not lvmsadc
not lvmsar
not pvdata

not lvrename $vg
not lvrename $vg-xxx
not lvrename $vg  $vg/$lv-rename $vg/$lv
not lvscan $vg
not vgscan $vg

#test vgdisplay -A to select only active VGs
# all LVs active - VG considered active
pvcreate -f "$dev1" "$dev2" "$dev3"

vgcreate $vg1 "$dev1"
lvcreate -l1 $vg1
lvcreate -l1 $vg1

# at least one LV active - VG considered active
vgcreate $vg2 "$dev2"
lvcreate -l1 $vg2
lvcreate -l1 -an -Zn $vg2

# no LVs active - VG considered inactive
vgcreate $vg3 "$dev3"
lvcreate -l1 -an -Zn $vg3
lvcreate -l1 -an -Zn $vg3

vgdisplay -s -A | grep $vg1
vgdisplay -s -A | grep $vg2
vgdisplay -s -A | not grep $vg3

vgremove -ff $vg1 $vg2 $vg3
