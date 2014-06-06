#!/bin/sh
# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# test activation race for raid's --syncaction check

. lib/inittest

# Proper mismatch count 1.5.2+ upstream, 1.3.5 < x < 1.4.0 in RHEL6
aux have_raid 1 3 5 &&
  ! aux have_raid 1 4 0 ||
  aux have_raid 1 5 2 || skip
aux prepare_vg 3

lvcreate -n $lv1 $vg -l1 --type raid1

START=$(get pv_field "$dev2" pe_start --units 1k)
METASIZE=$(get lv_field $vg/${lv1}_rmeta_1 size -a --units 1k)
SEEK=$((${START%\.00k} + ${METASIZE%\.00k}))
# Overwrite some portion of  _rimage_1
dd if=/dev/urandom of="$dev2" bs=1K count=1 seek=$SEEK oflag=direct

lvchange --syncaction check $vg/$lv1
check lv_field $vg/$lv1 raid_mismatch_count "128"

# Let's deactivate
lvchange -an $vg/$lv1

# Slow down write by 100ms
aux delay_dev "$dev2" 0 100
lvchange -ay $vg/$lv1
# noone has it open and target is read & running
dmsetup info -c

#sleep 10 < "$DM_DEV_DIR/$vg/$lv1" &
# "check" should find discrepancies but not change them
# 'lvs' should show results

# FIXME
# this looks like some race with 'write' during activation
# and syncaction...
# For now it fails with:
# device-mapper: message ioctl on  failed: Device or resource busy
#
lvchange --syncaction check $vg/$lv1

aux enable_dev "$dev2"
lvs -o+raid_mismatch_count -a $vg

vgremove -ff $vg
