#!/bin/sh
# Copyright (C) 2012,2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# 'Exercise some lvcreate diagnostics'

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

# FIXME  update test to make something useful on <16T
aux can_use_16T || skip

aux have_raid 1 3 0 || skip

aux prepare_vg 5 32

# Fake 5 PiB volume group $vg1 via snapshot LVs
for device in "$lv1" "$lv2" "$lv3" "$lv4" "$lv5"
do
	lvcreate --type snapshot -s -l 20%FREE -n $device $vg --virtualsize 1P
done

#FIXME this should be 1024T
#check lv_field $vg/$lv size "128.00m"

aux extend_filter_LVMTEST

pvcreate "$DM_DEV_DIR"/$vg/$lv[12345]
vgcreate -s 2M $vg1 "$DM_DEV_DIR"/$vg/$lv[12345]

# Delay PVs so that resynchronization doesn't fill
# the snapshots before removal of the RaidLV
for device in "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
do
	aux delay_dev "$device" 0 1
done

# bz837927 START

#
# Create large RAID LVs
#

# 200 TiB raid1
lvcreate --type raid1 -m 1 -L 200T -n $lv1 $vg1 --nosync
check lv_field $vg1/$lv1 size "200.00t"
aux check_status_chars $vg1 $lv1 "AA"
lvremove -ff $vg1

# 1 PiB raid1
lvcreate --type raid1 -m 1 -L 1P -n $lv1 $vg1 --nosync
check lv_field $vg1/$lv1 size "1.00p"
aux check_status_chars $vg1 $lv1 "AA"
lvremove -ff $vg1

# 750 TiB raid4/5
for segtype in raid4 raid5; do
        lvcreate --type $segtype -i 3 -L 750T -n $lv1 $vg1 --nosync
        check lv_field $vg1/$lv1 size "750.00t"
        aux check_status_chars $vg1 $lv1 "AAAA"
        lvremove -ff $vg1
done

# 750 TiB raid6 (with --nosync rejection check)
[ aux have_raid 1 9 0 ] && not lvcreate --type raid6 -i 3 -L 750T -n $lv1 $vg1 --nosync
lvcreate --type raid6 -i 3 -L 750T -n $lv1 $vg1
check lv_field $vg1/$lv1 size "750.00t"
aux check_status_chars $vg1 $lv1 "aaaaa"
lvremove -ff $vg1

# 1 PiB raid6 (with --nosync rejection check), then extend up to 2 PiB
[ aux have_raid 1 9 0 ] && not lvcreate --type raid6 -i 3 -L -L 1P -n $lv1 $vg1 --nosync
lvcreate --type raid6 -i 3 -L 1P -n $lv1 $vg1
check lv_field $vg1/$lv1 size "1.00p"
aux check_status_chars $vg1 $lv1 "aaaaa"
lvextend -L +1P $vg1/$lv1
check lv_field $vg1/$lv1 size "2.00p"
aux check_status_chars $vg1 $lv1 "aaaaa"
lvremove -ff $vg1

#
# Convert large 200 TiB linear to RAID1 (belong in different test script?)
#
lvcreate -aey -L 200T -n $lv1 $vg1
lvconvert --type raid1 -m 1 $vg1/$lv1
check lv_field $vg1/$lv1 size "200.00t"
aux check_status_chars $vg1 $lv1 "aa"
lvremove -ff $vg1

#
# Extending large 200 TiB RAID LV to 400 TiB (belong in different script?)
#
lvcreate --type raid1 -m 1 -L 200T -n $lv1 $vg1 --nosync
check lv_field $vg1/$lv1 size "200.00t"
aux check_status_chars $vg1 $lv1 "AA"
lvextend -L +200T $vg1/$lv1
check lv_field $vg1/$lv1 size "400.00t"
aux check_status_chars $vg1 $lv1 "AA"
lvremove -ff $vg1

# bz837927 END

vgremove -ff $vg1
vgremove -ff $vg
