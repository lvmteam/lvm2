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

aux prepare_devs 5

pvcreate $dev1
pvcreate --metadatacopies 0 $dev2
pvcreate --metadatacopies 0 $dev3
pvcreate $dev4
pvcreate --metadatacopies 0 $dev5

vgcreate -c n "$vg" $(cat DEVICES)
lvcreate -n $lv -l 1 -i5 -I256 $vg

pvchange -x n $dev1
pvchange -x y $dev1
vgchange -a n $vg
pvchange --uuid $dev1
pvchange --uuid $dev2
vgremove -f $vg

# check that PVs without metadata don't cause too many full device rescans (bz452606)
for mdacp in 1 0; do
	pvcreate --metadatacopies $mdacp $(cat DEVICES)
	pvcreate $dev1
	vgcreate -c n $vg $(cat DEVICES)
	lvcreate -n $lv1 -l 2 -i5 -I256 $vg
	lvcreate -n $lv2 -m2 -l 2  $vg
	#lvchange -an $vg
	lvchange -an $vg/$lv1
	lvchange -an $vg/$lv2
	vgchange -ay $vg
	lvchange -vvvv -an $vg/$lv1 >out$mdacp 2>&1 
	lvchange -vvvv -an $vg/$lv2 >>out$mdacp 2>&1 
	vgremove -f $vg
done
not grep "Cached VG .* incorrect PV list" out0

# some M1 metadata tests
pvcreate -M1 $dev1
pvcreate -M1 $dev2
pvcreate -M1 $dev3
pv3_uuid=$(pvs --noheadings -o pv_uuid $dev3)
vgcreate -M1 -c n $vg $dev1 $dev2 $dev3
pvchange --uuid $dev1

# verify pe_start of all M1 PVs
pv_align="128.00k"
check pv_field $dev1 pe_start $pv_align
check pv_field $dev2 pe_start $pv_align
check pv_field $dev3 pe_start $pv_align

pvs --units k -o name,pe_start,vg_mda_size,vg_name

# upgrade from v1 to v2 metadata
vgconvert -M2 $vg

# verify pe_start of all M2 PVs
check pv_field $dev1 pe_start $pv_align
check pv_field $dev2 pe_start $pv_align
check pv_field $dev3 pe_start $pv_align

pvs --units k -o name,pe_start,vg_mda_size,vg_name

# create backup and then restore $dev3
vgcfgbackup -f $TESTDIR/bak-%s $vg
pvcreate -ff -y --restorefile $TESTDIR/bak-$vg --uuid $pv3_uuid $dev3
vgcfgrestore -f $TESTDIR/bak-$vg $vg

# verify pe_start of $dev3
check pv_field $dev3 pe_start $pv_align
