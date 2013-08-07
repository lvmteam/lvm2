#!/bin/sh
# Copyright (C) 2010-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux prepare_pvs 5 10
# proper DEVRANGE needs to be set according to extent size
DEVRANGE="0-32"
vgcreate -s 32k $vg $(cat DEVICES)

# convert from linear to 2-way mirror ("mirror" default type)
lvcreate -aey -l2 -n $lv1 $vg "$dev1"
lvconvert -i1 -m+1 $vg/$lv1 "$dev2" "$dev3:0-1" \
	--config 'global { mirror_segtype_default = "mirror" }'
check mirror $vg $lv1 "$dev3"
lvremove -ff $vg

# convert from linear to 2-way mirror (override "raid1" default type)
lvcreate -aey -l2 -n $lv1 $vg "$dev1"
lvconvert -i1 --type mirror -m+1 $vg/$lv1 "$dev2" "$dev3:0-1" \
	--config 'global { mirror_segtype_default = "raid1" }'
check mirror $vg $lv1 "$dev3"
lvremove -ff $vg

# convert from linear to 2-way mirror - with tags and volume_list (bz683270)
lvcreate -aey -l2 -n $lv1 $vg --addtag hello
lvconvert -i1 --type mirror -m+1 $vg/$lv1 \
    --config 'activation { volume_list = [ "@hello" ] }'
lvremove -ff $vg

# convert from 2-way to 3-way mirror - with tags and volume_list (bz683270)
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg --addtag hello
lvconvert -i1 -m+1 $vg/$lv1 \
    --config 'activation { volume_list = [ "@hello" ] }'
lvremove -ff $vg

# convert from 2-way mirror to linear
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0-1"
lvconvert -m-1 $vg/$lv1
check linear $vg $lv1
lvremove -ff $vg
# and now try removing a specific leg (bz453643)
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0-1"
lvconvert -m0 $vg/$lv1 "$dev2"
check lv_on $vg $lv1 "$dev1"
lvremove -ff $vg

# convert from disklog to corelog, active
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0-1"
lvconvert -f --mirrorlog core $vg/$lv1
check mirror $vg $lv1 core
lvremove -ff $vg

# convert from corelog to disklog, active
lvcreate -aey -l2 --type mirror -m1 --mirrorlog core -n $lv1 $vg "$dev1" "$dev2"
lvconvert --mirrorlog disk $vg/$lv1 "$dev3:0-1"
check mirror $vg $lv1 "$dev3"
lvremove -ff $vg

# convert linear to 2-way mirror with 1 PV
lvcreate -aey -l2 -n $lv1 $vg "$dev1"
not lvconvert -m+1 --mirrorlog core $vg/$lv1 "$dev1"
lvremove -ff $vg

# Start w/ 3-way mirror
# Test pulling primary image before mirror in-sync (should fail)
# Test pulling primary image after mirror in-sync (should work)
# Test that the correct devices remain in the mirror
lvcreate -aey -l2 --type mirror -m2 -n $lv1 $vg "$dev1" "$dev2" "$dev4" "$dev3:$DEVRANGE"
# FIXME:
#  This is somewhat timing dependent - sync /could/ finish before
#  we get a chance to have this command fail
should not lvconvert -m-1 $vg/$lv1 "$dev1"

lvconvert $vg/$lv1 # wait
lvconvert -m2 $vg/$lv1 "$dev1" "$dev2" "$dev4" "$dev3:0" # If the above "should" failed...

aux wait_for_sync $vg $lv1
lvconvert -m-1 $vg/$lv1 "$dev1"
check mirror_images_on $lv1 "$dev2" "$dev4"
lvconvert -m-1 $vg/$lv1 "$dev2"
check linear $vg $lv1
check lv_on $vg $lv1 "$dev4"
lvremove -ff $vg

# No parallel lvconverts on a single LV please

lvcreate -aey -l5 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0"
check mirror $vg $lv1
check mirror_legs $vg $lv1 2
lvconvert -m+1 -b $vg/$lv1 "$dev4"

# Next convert should fail b/c we can't have 2 at once
should not lvconvert -m+1 $vg/$lv1 "$dev5"
lvconvert $vg/$lv1 # wait
lvconvert -m2 $vg/$lv1 # In case the above "should" actually failed

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# add 1 mirror to core log mirror, but
#  implicitly keep log as 'core'
lvcreate -aey -l2 --type mirror -m1 --mirrorlog core -n $lv1 $vg "$dev1" "$dev2"
lvconvert -m +1 -i1 $vg/$lv1

check mirror $vg $lv1 core
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# remove 1 mirror from corelog'ed mirror; should retain 'core' log type
lvcreate -aey -l2 --type mirror -m2 --corelog -n $lv1 $vg
lvconvert -m -1 -i1 $vg/$lv1

check mirror $vg $lv1 core
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 2
lvremove -ff $vg

# add 1 mirror then add 1 more mirror during conversion
# FIXME this has been explicitly forbidden?
#lvcreate -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3":0
#lvconvert -m+1 -b $vg/$lv1 "$dev4"
#lvconvert -m+1 $vg/$lv1 "$dev5"
#
#check mirror $vg $lv1 "$dev3"
#check mirror_no_temporaries $vg $lv1
#check mirror_legs $vg $lv1 4
#lvremove -ff $vg

# convert inactive mirror and start polling
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:$DEVRANGE"
lvchange -an $vg/$lv1
lvconvert -m+1 $vg/$lv1 "$dev4"
lvchange -aey $vg/$lv1
lvconvert $vg/$lv1 # wait
check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
lvremove -ff $vg

# ---------------------------------------------------------------------
# removal during conversion

# "remove newly added mirror"
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:$DEVRANGE"
lvconvert -m+1 -b $vg/$lv1 "$dev4"
lvconvert -m-1 $vg/$lv1 "$dev4"
lvconvert $vg/$lv1 # wait

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 2
lvremove -ff $vg

# "remove one of newly added mirrors"
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:$DEVRANGE"
lvconvert -m+2 -b $vg/$lv1 "$dev4" "$dev5"
lvconvert -m-1 $vg/$lv1 "$dev4"
lvconvert $vg/$lv1 # wait

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# "remove from original mirror (the original is still mirror)"
lvcreate -aey -l2 --type mirror -m2 -n $lv1 $vg "$dev1" "$dev2" "$dev5" "$dev3:$DEVRANGE"
lvconvert -m+1 -b $vg/$lv1 "$dev4"
lvconvert -m-1 $vg/$lv1 "$dev2"
lvconvert $vg/$lv1

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# "remove from original mirror (the original becomes linear)"
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:$DEVRANGE"
lvconvert -m+1 -b $vg/$lv1 "$dev4"
lvconvert -m-1 $vg/$lv1 "$dev2"
lvconvert $vg/$lv1

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 2
lvremove -ff $vg

# ---------------------------------------------------------------------

# "rhbz440405: lvconvert -m0 incorrectly fails if all PEs allocated"
lvcreate -aey -l$(pvs --noheadings -ope_count "$dev1") --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:$DEVRANGE"
aux wait_for_sync $vg $lv1
lvconvert -m0 $vg/$lv1 "$dev1"
check linear $vg $lv1
lvremove -ff $vg

# "rhbz264241: lvm mirror doesn't lose it's "M" --nosync attribute after being down and the up converted"
lvcreate -aey -l2 --type mirror -m1 -n $lv1 --nosync $vg
lvconvert -m0 $vg/$lv1
lvconvert -m1 $vg/$lv1
lvs --noheadings -o attr $vg/$lv1 | grep '^ *m'
lvremove -ff $vg

# lvconvert from linear (on multiple PVs) to mirror
lvcreate -aey -l 8 -n $lv1 $vg "$dev1:0-3" "$dev2:0-3"
lvconvert -m1 $vg/$lv1

should check mirror $vg $lv1
check mirror_legs $vg $lv1 2
lvremove -ff $vg

# BZ 463272: disk log mirror convert option is lost if downconvert option is also given
lvcreate -aey -l1 --type mirror -m2 --corelog -n $lv1 $vg "$dev1" "$dev2" "$dev3"
aux wait_for_sync $vg $lv1
lvconvert -m1 --mirrorlog disk $vg/$lv1
check mirror $vg $lv1
not check mirror $vg $lv1 core
lvremove -ff $vg

# ---
# add mirror and disk log

# "add 1 mirror and disk log"
lvcreate -aey -l2 --type mirror -m1 --mirrorlog core -n $lv1 $vg "$dev1" "$dev2"

# FIXME on next line, specifying $dev3:0 $dev4 (i.e log device first) fails (!)
lvconvert -m+1 --mirrorlog disk -i1 $vg/$lv1 "$dev4" "$dev3:$DEVRANGE"

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# simple mirrored stripe
lvcreate -aey -i2 -l10 -n $lv1 $vg
lvconvert -m1 -i1 $vg/$lv1
lvreduce -f -l1 $vg/$lv1
lvextend -f -l10 $vg/$lv1
lvremove -ff $vg/$lv1

# extents must be divisible
lvcreate -aey -l15 -n $lv1 $vg
not lvconvert -m1 --corelog --stripes 2 $vg/$lv1
lvremove -ff $vg

test -e LOCAL_CLVMD && exit 0

# FIXME - cases which needs to be fixed to work in cluster
# Linear to mirror with mirrored log using --alloc anywhere
lvcreate -aey -l2 -n $lv1 $vg "$dev1"
lvconvert -m +1 --mirrorlog mirrored --alloc anywhere $vg/$lv1 "$dev1" "$dev2"
should check mirror $vg $lv1
lvremove -ff $vg

# Should not be able to add images to --nosync mirror
# but should be able to after 'lvchange --resync'
lvcreate -aey --type mirror -m 1 -l1 -n $lv1 $vg --nosync
not lvconvert -m +1 $vg/$lv1
lvchange -aey --resync -y $vg/$lv1
lvconvert -m +1 $vg/$lv1
lvremove -ff $vg

lvcreate -aey --type mirror -m 1 --corelog -l1 -n $lv1 $vg --nosync
not lvconvert -m +1 $vg/$lv1
lvchange -aey --resync -y $vg/$lv1
lvconvert -m +1 $vg/$lv1
lvremove -ff $vg

# FIXME: Cluster exclusive activation does not work here
# unsure why  lib/metadata/mirror.c
# has this code:
#
#       } else if (vg_is_clustered(vg)) {
#                log_error("Unable to convert the log of an inactive "
#                          "cluster mirror, %s", lv->name);
#                return 0;
# disabling this in the code passes this test

# bz192865: lvconvert log of an inactive mirror lv
# convert from disklog to corelog, inactive
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0-1"
lvchange -an $vg/$lv1
lvconvert -y -f --mirrorlog core $vg/$lv1
check mirror $vg $lv1 core
lvremove -ff $vg

# convert from corelog to disklog, inactive
lvcreate -aey -l2 --type mirror -m1 --mirrorlog core -n $lv1 $vg "$dev1" "$dev2"
lvchange -an $vg/$lv1
lvconvert --mirrorlog disk $vg/$lv1 "$dev3:0-1"
check mirror $vg $lv1 "$dev3"
lvremove -ff $vg
