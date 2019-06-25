#!/usr/bin/env bash

# Copyright (C) 2008-2013,2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/inittest

# Check what is and is not permitted on an exported VG/PV

aux prepare_devs 3
get_devs

vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"

lvcreate -l1 -n $lv1 -an $vg1
lvcreate -l1 -n $lv2 -an $vg2

vgchange --addtag aa $vg1
lvchange --addtag bb $vg1/$lv1

# vgexport only when no lvs are active
lvchange -ay $vg1/$lv1
not vgexport $vg1
lvchange -an $vg1/$lv1

vgexport $vg1

lvm fullreport |tee out
grep $vg1 out

lvm fullreport $vg1 |tee out
grep $vg1 out

not lvchange -ay $vg1
not lvchange -ay $vg1/$lv1
not lvchange --addtag bar $vg1/$lv1
not lvchange --monitor y $vg1/$lv1

not lvconvert --type mirror $vg1/$lv1

not lvcreate -l1 $vg1

not lvdisplay $vg1

lvdisplay 2>&1|tee out
not grep $vg1 out

not lvextend -l+1 $vg1/$lv1

lvmdiskscan 2>&1|tee foo
grep "$dev1" foo

not lvreduce -l-1 $vg1/$lv1

not lvremove $vg1
not lvremove $vg1/$lv1

not lvrename $vg1 $lv1 $lv2

not lvresize --size 1M $vg1/$lv1

not lvs $vg1

lvs 2>&1|tee out
not grep $vg1 out

lvscan 2>&1|tee out
not grep $vg1 out

not pvchange --addtag cc "$dev1"
pvs -o+tags "$dev1" 2>&1|tee out
grep "$dev1" out
not grep cc out

pvs -osize "$dev1" > before
not pvresize --setphysicalvolumesize 100M "$dev1"
pvs -osize "$dev1" > after
diff before after

pvck "$dev1"
pvck --dump headers "$dev1" > out
grep "label_header at 512" out

not pvcreate "$dev1"

pvdisplay "$dev1" 2>&1|tee out
grep "$dev1" out

not pvmove "$dev1"

not pvremove "$dev1"

pvs "$dev1" 2>&1|tee out
grep "$dev1" out

pvscan 2>&1|tee out
grep "$dev1" out

pvscan --cache 2>&1|tee out
grep "$dev1" out

vgcfgbackup $vg1

vgcfgrestore $vg1

not vgchange -ay $vg1
not vgchange --addtag asdf $vg1
not vgchange --monitor y $vg1

not vgck $vg1

not vgcreate $vg1 "$dev3"

vgdisplay $vg1 2>&1|tee out
grep $vg1 out

not vgexport $vg1

vgexport $vg2
not lvcreate -l1 -n $lv3 -an $vg2
vgimport $vg2
lvcreate -l1 -n $lv3 -an $vg2
lvremove $vg2/$lv3

not vgextend $vg1 "$dev3"

not vgmerge $vg1 $vg2

not vgmknodes $vg1

not vgreduce --removemissing $vg1

not vgremove $vg1

vgrename $vg1 $vg3
vgrename $vg3 $vg1

vgs $vg1 2>&1|tee out
grep $vg1 out

vgscan 2>&1|tee out
grep $vg1 out

# pvscan --cache tracks online state of exported PVs,
# but autoactivation should not activate LVs.
pvscan --cache -aay "$dev1"
vgimport $vg1
check inactive $vg1 $lv1
vgexport $vg1

# using a tag does not give access to exported vg
lvchange -ay @foo
vgimport $vg1
check inactive $vg1 $lv1
vgexport $vg1

# using select does not give access to exported vg
lvchange -ay --select lvname=$lv1
vgimport $vg1
check inactive $vg1 $lv1
vgexport $vg1

# tag or select do not work with vgremove on exported vg
vgremove @foo
vgs $vg1
vgremove --select vgname=$vg1
vgs $vg1

# exported vg is skipped without error when not named
vgchange -ay
vgimport $vg1
check inactive $vg1 $lv1
vgexport $vg1

# exported vg is skipped without error when not named
vgchange --addtag newtag
vgs -o tags $vg1 > out
not grep newtag out
vgchange --deltag aa
vgs -o tags $vg1 > out
grep aa out

# exported vg is skipped without error when not named
vgchange --monitor y
vgchange --monitor n

vgimport $vg1
vgextend $vg1 "$dev3"
vgexport $vg1

not vgreduce $vg1 "$dev3"

not vgsplit $vg1 "$vg3" "$dev3"

# For vgimportclone see vgimportclone.sh

vgimport $vg1
vgremove -ff $vg1
vgremove -ff $vg2
