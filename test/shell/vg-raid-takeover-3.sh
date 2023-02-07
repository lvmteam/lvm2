#!/usr/bin/env bash

# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Test VG takeover with raid LVs'

# test does not apply to lvmlockd
SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

print_lvmlocal() {
	{ echo "local {"; printf "%s\n" "$@"; echo "}"; } >"$LVMLOCAL"
}

. lib/inittest

aux prepare_devs 3 1024

SIDFILE="etc/lvm_test.conf"
LVMLOCAL="etc/lvmlocal.conf"

DFDIR="$LVM_SYSTEM_DIR/devices"
DF="$DFDIR/system.devices"

# Avoid system id validation in the devices file
# which gets in the way of the test switching the
# local system id.
clear_df_systemid() {
	if [[ -f $DF ]]; then
		sed -e "s|SYSTEMID=.||" "$DF" > tmpdf
		cp tmpdf $DF
	fi
}

SID1=sidfoofile1
SID2=sidfoofile2
echo "$SID1" > "$SIDFILE"
clear_df_systemid
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
vgcreate $vg "$dev1" "$dev2" "$dev3"
vgs -o+systemid,partial $vg
check vg_field $vg systemid "$SID1"

lvcreate --type raid1 -L 400 -m1 -n $lv1 $vg "$dev1" "$dev2"
lvcreate --type raid1 -L 400 -m2 -n $lv2 $vg "$dev1" "$dev2" "$dev3"

# give some time for raid init
sleep 2
lvs -a -o+devices $vg

mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv2"

dd if=/dev/urandom of=pattern1 bs=512K count=1

mount_dir="mnt_takeover"
mkdir -p "$mount_dir"

mount "$DM_DEV_DIR/$vg/$lv1" $mount_dir
dd if=/dev/zero of=$mount_dir/file1 bs=4096 count=1024
cp pattern1 $mount_dir/pattern1a
cp pattern1 $mount_dir/pattern1b
umount $mount_dir

mount "$DM_DEV_DIR/$vg/$lv2" $mount_dir
dd if=/dev/zero of=$mount_dir/file1 bs=4096 count=1024
cp pattern1 $mount_dir/pattern1a
cp pattern1 $mount_dir/pattern1b
umount $mount_dir

vgchange -an $vg

# make the vg foreign
vgchange --yes --systemid $SID2 $vg
not vgs $vg

# make one dev missing
aux hide_dev "$dev1"

# take over the vg, like cluster failover would do
vgchange --majoritypvs --config "local/extra_system_ids=[\"${SID2}\"]" --systemid $SID1 $vg
pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg

lvchange -ay --activationmode degraded $vg/$lv1
lvchange -ay --activationmode degraded $vg/$lv2

mount "$DM_DEV_DIR/$vg/$lv1" $mount_dir
dd of=/dev/null if=$mount_dir/file1 bs=4096 count=1024
diff pattern1 $mount_dir/pattern1a
diff pattern1 $mount_dir/pattern1b
rm $mount_dir/pattern1b
rm $mount_dir/file1
cp pattern1 $mount_dir/pattern1c
umount $mount_dir

mount "$DM_DEV_DIR/$vg/$lv2" $mount_dir
dd of=/dev/null if=$mount_dir/file1 bs=4096 count=1024
diff pattern1 $mount_dir/pattern1a
diff pattern1 $mount_dir/pattern1b
rm $mount_dir/pattern1b
rm $mount_dir/file1
cp pattern1 $mount_dir/pattern1c
umount $mount_dir

pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg

# replaces dev1 with dev3
lvconvert -y --repair $vg/$lv1

# no other disk to replace dev1 so remove the leg,
# but that's not allowed until the missing disk is removed from the vg
not lvconvert -y -m-1 $vg/$lv2
vgreduce --removemissing --mirrorsonly --force $vg
pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg
lvconvert -y -m-1 $vg/$lv2

pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg

mount "$DM_DEV_DIR/$vg/$lv1" $mount_dir
diff pattern1 $mount_dir/pattern1a
diff pattern1 $mount_dir/pattern1c
umount $mount_dir

mount "$DM_DEV_DIR/$vg/$lv2" $mount_dir
diff pattern1 $mount_dir/pattern1a
diff pattern1 $mount_dir/pattern1c
umount $mount_dir

aux unhide_dev "$dev1"

# put dev1 back into lv2,
# requires clearing outdated metadata and putting dev1 back in vg
vgck --updatemetadata $vg
pvs -o+missing
vgextend $vg "$dev1"
pvs -o+missing
lvconvert -y -m+1 $vg/$lv2 "$dev1"

pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg

mount "$DM_DEV_DIR/$vg/$lv1" $mount_dir
diff pattern1 $mount_dir/pattern1a
diff pattern1 $mount_dir/pattern1c
umount $mount_dir

mount "$DM_DEV_DIR/$vg/$lv2" $mount_dir
diff pattern1 $mount_dir/pattern1a
diff pattern1 $mount_dir/pattern1c
umount $mount_dir

vgchange -an $vg
vgremove -f $vg
