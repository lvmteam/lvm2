#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux have_raid 1 3 0 || skip

aux prepare_pvs 6 80

vgcreate -s 256K $vg $(cat DEVICES)

for deactivate in true false; do

# Extend and reduce a 2-way RAID1
	lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg

	test $deactivate && lvchange -an $vg/$lv1

	lvresize -l +2 $vg/$lv1

	should lvresize -y -l -2 $vg/$lv1

	#check raid_images_contiguous $vg $lv1

# Extend and reduce 3-striped RAID 4/5/6
	for i in 4 5 6 ; do
		lvcreate --type raid$i -i 3 -l 3 -n $lv2 $vg

		test $deactivate && lvchange -an $vg/$lv2

		lvresize -l +3 $vg/$lv2

		#check raid_images_contiguous $vg $lv1

		should lvresize -y -l -3 $vg/$lv2

		#check raid_images_contiguous $vg $lv1

		lvremove -ff $vg
	done
done

# Bug 1005434
# Ensure extend is contiguous
lvcreate --type raid4 -l 2 -i 2 -n $lv1 $vg "$dev4" "$dev5" "$dev6"
lvextend -l +2 --alloc contiguous $vg/$lv1
check lv_tree_on $vg $lv1 "$dev4" "$dev5" "$dev6"

vgremove -f $vg
