#######################################################################
# This series of tests is meant to validate the correctness of
# 'dmsetup status' for RAID LVs - especially during various sync action
# transitions, like: recover, resync, check, repair, idle, reshape, etc
#######################################################################
SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

export LVM_TEST_LVMETAD_DEBUG_OPTS=${LVM_TEST_LVMETAD_DEBUG_OPTS-}

. lib/inittest

# check for version 1.9.0
# - it is the point at which linear->raid1 uses "recover"
aux have_raid 1 9 0 || skip

aux prepare_pvs 9
vgcreate -s 2m $vg $(cat DEVICES)

###########################################
# Upconverted RAID1 should never have all 'a's in status output
###########################################
aux delay_dev $dev2 0 100
lvcreate -aey -l 2 -n $lv1 $vg $dev1
lvconvert --type raid1 -y -m 1 $vg/$lv1 $dev2
while ! check in_sync $vg $lv1; do
        a=( $(dmsetup status $vg-$lv1) ) || die "Unable to get status of $vg/$lv1"
	[ ${a[5]} != "aa" ]
        sleep .1
done
aux enable_dev $dev2
lvremove -ff $vg

vgremove -ff $vg
