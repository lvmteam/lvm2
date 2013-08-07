#!/bin/sh
# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#
# tests functionality of lvs, pvs, vgs, *display tools
#

. lib/test

aux prepare_devs 5

pvcreate "$dev1"
pvcreate --metadatacopies 0 "$dev2"
pvcreate --metadatacopies 0 "$dev3"
pvcreate "$dev4"
pvcreate --metadatacopies 0 "$dev5"

#COMM bz195276 -- pvs doesn't show PVs until a VG is created
test $(pvs --noheadings $(cat DEVICES) | wc -l) -eq 5

#COMM pvs with segment attributes works even for orphans
test $(pvs --noheadings -o seg_all,pv_all,lv_all,vg_all $(cat DEVICES) | wc -l) -eq 5

vgcreate $vg $(cat DEVICES)

#COMM pvs and vgs report mda_count, mda_free (bz202886, bz247444)
pvs -o +pv_mda_count,pv_mda_free $(cat DEVICES)
for I in "$dev2" "$dev3" "$dev5"; do
	check pv_field $I pv_mda_count 0
	check pv_field $I pv_mda_free 0
done
vgs -o +vg_mda_count,vg_mda_free $vg
check vg_field $vg vg_mda_count 2

#COMM pvs doesn't display --metadatacopies 0 PVs as orphans (bz409061)
pvdisplay "$dev2"|grep "VG Name.*$vg"
check pv_field "$dev2" vg_name $vg

#COMM lvs displays snapshots (bz171215)
lvcreate -aey -l4 -n $lv1 $vg
lvcreate -l4 -s -n $lv2 $vg/$lv1
test $(lvs --noheadings $vg | wc -l) -eq 2
# should lvs -a display cow && real devices? (it doesn't)
test $(lvs -a --noheadings $vg | wc -l)  -eq 2
dmsetup ls|grep $PREFIX|grep -v "LVMTEST.*pv."
lvremove -f $vg/$lv2

#COMM lvs -a displays mirror legs and log
lvcreate -aey -l4 --type mirror -m2 -n $lv3 $vg
test $(lvs --noheadings $vg | wc -l) -eq 2
test $(lvs -a --noheadings $vg | wc -l) -eq 6
dmsetup ls|grep $PREFIX|grep -v "LVMTEST.*pv."

#COMM vgs with options from pvs still treats arguments as VGs (bz193543)
vgs -o pv_name,vg_name $vg
# would complain if not

#COMM pvdisplay --maps feature (bz149814)
pvdisplay $(cat DEVICES) >out
pvdisplay --maps $(cat DEVICES) >out2
not diff out out2
