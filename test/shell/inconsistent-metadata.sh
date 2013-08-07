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

. lib/test

aux prepare_vg 3 12

lvcreate -aye --type mirror -m 1 -l 1 -n mirror $vg
lvcreate -l 1 -n resized $vg
lvchange -a n $vg/mirror

aux backup_dev $(cat DEVICES)

init() {
	aux restore_dev $(cat DEVICES)
	not check lv_field $vg/resized lv_size "8.00m"
	lvresize -L 8192K $vg/resized
	aux restore_dev "$dev1"
}

# vgscan fixes up metadata (needs --cache option for direct scan if lvmetad is used)
test -e LOCAL_LVMETAD && cache="--cache"
init
vgscan $cache 2>&1 | tee cmd.out
grep "Inconsistent metadata found for VG $vg" cmd.out
test -e LOCAL_LVMETAD && vgrename $vg foo && vgrename foo $vg # trigger a write
vgscan $cache 2>&1 | tee cmd.out
not grep "Inconsistent metadata found for VG $vg" cmd.out
check lv_field $vg/resized lv_size "8.00m"

# only vgscan would have noticed metadata inconsistencies when lvmetad is active
if test ! -e LOCAL_LVMETAD; then
	# vgdisplay fixes
	init
	vgdisplay $vg 2>&1 | tee cmd.out
	grep "Inconsistent metadata found for VG $vg" cmd.out
	vgdisplay $vg 2>&1 | tee cmd.out
	not grep "Inconsistent metadata found for VG $vg" cmd.out
	check lv_field $vg/resized lv_size "8.00m"

	# lvs fixes up
	init
	lvs $vg 2>&1 | tee cmd.out
	grep "Inconsistent metadata found for VG $vg" cmd.out
	vgdisplay $vg 2>&1 | tee cmd.out
	not grep "Inconsistent metadata found for VG $vg" cmd.out
	check lv_field $vg/resized lv_size "8.00m"

	# vgs fixes up as well
	init
	vgs $vg 2>&1 | tee cmd.out
	grep "Inconsistent metadata found for VG $vg" cmd.out
	vgs $vg 2>&1 | tee cmd.out
	not grep "Inconsistent metadata found for VG $vg" cmd.out
	check lv_field $vg/resized lv_size "8.00m"
fi

echo Check auto-repair of failed vgextend - metadata written to original pv but not new pv
vgremove -f $vg
pvremove -ff $(cat DEVICES)
pvcreate $(cat DEVICES)
aux backup_dev "$dev2"
vgcreate $vg "$dev1"
vgextend $vg "$dev2"
aux restore_dev "$dev2"
vgscan $cache
should check compare_fields vgs $vg vg_mda_count pvs "$dev2" vg_mda_count
