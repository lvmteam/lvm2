#!/bin/sh
# Copyright (C) 2010-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

which mkfs.ext3 || skip

lvdev_() {
    echo "$DM_DEV_DIR/$1/$2"
}

snap_lv_name_() {
    echo ${1}_snap
}

setup_merge_() {
    local VG_NAME=$1
    local LV_NAME=$2
    local NUM_EXTRA_SNAPS=${3:-0}
    local BASE_SNAP_LV_NAME=$(snap_lv_name_ $LV_NAME)

    lvcreate -aey -n $LV_NAME -l 50%FREE $VG_NAME
    lvcreate -s -n $BASE_SNAP_LV_NAME -l 20%FREE ${VG_NAME}/${LV_NAME}
    mkfs.ext3 "$(lvdev_ $VG_NAME $LV_NAME)"

    if [ $NUM_EXTRA_SNAPS -gt 0 ]; then
	for i in $(seq 1 $NUM_EXTRA_SNAPS); do
	    lvcreate -s -n ${BASE_SNAP_LV_NAME}_${i} -l 20%FREE ${VG_NAME}/${LV_NAME}
	done
    fi
}

aux prepare_vg 1 100
mkdir test_mnt

# test full merge of a single LV
setup_merge_ $vg $lv1
# now that snapshot LV is created: test if snapshot-merge target is available
aux target_at_least snapshot-merge 1 0 0 || skip

# make sure lvconvert --merge requires explicit LV listing
not lvconvert --merge
lvconvert --merge $vg/$(snap_lv_name_ $lv1)
lvremove -f $vg/$lv1


# test that an actively merging snapshot may not be removed
setup_merge_ $vg $lv1
lvconvert -i+100 --merge --background $vg/$(snap_lv_name_ $lv1)
not lvremove -f $vg/$(snap_lv_name_ $lv1)
lvremove -f $vg/$lv1


# "onactivate merge" test
setup_merge_ $vg $lv1
mount "$(lvdev_ $vg $lv1)" test_mnt
lvconvert --merge $vg/$(snap_lv_name_ $lv1)
# -- refresh LV while FS is still mounted (merge must not start),
#    verify 'snapshot-origin' target is still being used
lvchange --refresh $vg/$lv1
umount test_mnt
dm_table $vg-$lv1 | grep " snapshot-origin "

# -- refresh LV to start merge (now that FS is unmounted),
#    an active merge uses the 'snapshot-merge' target
lvchange --refresh $vg/$lv1
# check whether it's still merging - or maybe got already merged (slow test)
dm_table $vg-$lv1 | grep " snapshot-merge " || dm_table $vg-$lv1 | grep " linear "
# -- don't care if merge is still active; lvremove at this point
#    may test stopping an active merge
lvremove -f $vg/$lv1

# "onactivate merge" test
# -- deactivate/remove after disallowed merge attempt, tests
#    to make sure preload of origin's metadata is _not_ performed
setup_merge_ $vg $lv1
mount "$(lvdev_ $vg $lv1)" test_mnt
lvconvert --merge $vg/$(snap_lv_name_ $lv1)
# -- refresh LV while FS is still mounted (merge must not start),
#    verify 'snapshot-origin' target is still being used
lvchange --refresh $vg/$lv1
umount test_mnt
dm_table $vg-$lv1 | grep " snapshot-origin " >/dev/null
lvremove -f $vg/$lv1


# test multiple snapshot merge; tests copy out that is driven by merge
setup_merge_ $vg $lv1 1
lvconvert --merge $vg/$(snap_lv_name_ $lv1)
lvremove -f $vg/$lv1


# test merging multiple snapshots that share the same tag
setup_merge_ $vg $lv1
setup_merge_ $vg $lv2
lvchange --addtag this_is_a_test $vg/$(snap_lv_name_ $lv1)
lvchange --addtag this_is_a_test $vg/$(snap_lv_name_ $lv2)
lvconvert --merge @this_is_a_test
lvs $vg | tee out
not grep $(snap_lv_name_ $lv1) out
not grep $(snap_lv_name_ $lv2) out
lvremove -f $vg/$lv1 $vg/$lv2

# FIXME following tests would need to poll merge progress, via periodic lvs?
# Background processes don't lend themselves to lvm testsuite...

# test: onactivate merge of a single lv

# test: do onactivate, deactivate the origin LV, reactivate the LV, merge should resume

# test: multiple onactivate merge

vgremove -f $vg
