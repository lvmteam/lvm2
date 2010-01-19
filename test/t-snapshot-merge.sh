#!/bin/sh
# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
set -xv

which mkfs.ext3 || exit 200

. ./test-utils.sh

lvdev_()
{
    echo "$G_dev_/$1/$2"
}

snap_lv_name_() {
    echo ${1}_snap
}

setup_merge() {
    local VG_NAME=$1
    local LV_NAME=$2
    local NUM_EXTRA_SNAPS="$3"
    test -z "$NUM_EXTRA_SNAPS" && NUM_EXTRA_SNAPS=0
    local BASE_SNAP_LV_NAME=$(snap_lv_name_ $LV_NAME)

    lvcreate -n $LV_NAME -l 50%FREE $VG_NAME
    lvcreate -s -n $BASE_SNAP_LV_NAME -l 20%FREE ${VG_NAME}/${LV_NAME}
    mkfs.ext3 $(lvdev_ $VG_NAME $LV_NAME)

    if [ $NUM_EXTRA_SNAPS -gt 0 ]; then
	for i in `seq 1 $NUM_EXTRA_SNAPS`; do
	    lvcreate -s -n ${BASE_SNAP_LV_NAME}_${i} -l 20%FREE ${VG_NAME}/${LV_NAME}
	done
    fi
}

aux prepare_vg 1 100


# full merge of a single LV
setup_merge $vg $lv1

# now that snapshot LV is created: test if snapshot-merge target is available
$(dmsetup targets | grep -q snapshot-merge) || exit 200

lvs -a
lvconvert --merge $vg/$(snap_lv_name_ $lv1)
lvremove -f $vg/$lv1


# "onactivate merge" test -- refresh LV while FS is still mounted;
# verify snapshot-origin target is still being used
setup_merge $vg $lv1
lvs -a
mkdir test_mnt
mount $(lvdev_ $vg $lv1) test_mnt
lvconvert --merge $vg/$(snap_lv_name_ $lv1)
lvchange --refresh $vg/$lv1
umount test_mnt
rm -r test_mnt
# an active merge uses the "snapshot-merge" target
dmsetup table ${vg}-${lv1} | grep -q " snapshot-origin "
test $? = 0
lvremove -f $vg/$lv1


# test multiple snapshot merge; tests copy out that is driven by merge
setup_merge $vg $lv1 1
lvs -a
lvconvert --merge $vg/$(snap_lv_name_ $lv1)
lvremove -f $vg/$lv1


# FIXME following tests would need to poll merge progress, via periodic lvs?
# Background processes don't lend themselves to lvm testsuite...

# test: onactivate merge of a single lv

# test: do onactivate, deactivate the origin LV, reactivate the LV, merge should resume

# test: multiple onactivate merge


vgremove -f "$vg"
