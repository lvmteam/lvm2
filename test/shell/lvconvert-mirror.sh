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

. lib/test

aux prepare_pvs 5 10
# FIXME - test fails with extent size < 512k
vgcreate -c n -s 512k $vg $(cat DEVICES)

# convert from linear to 2-way mirror
lvcreate -l2 -n $lv1 $vg "$dev1"
lvconvert -i1 -m+1 $vg/$lv1 "$dev2" "$dev3:0-1"
check mirror $vg $lv1 "$dev3"
lvremove -ff $vg

# convert from linear to 2-way mirror - with tags and volume_list (bz683270)
lvcreate -l2 -n $lv1 $vg --addtag hello
lvconvert -i1 -m+1 $vg/$lv1 \
    --config 'activation { volume_list = [ "@hello" ] }'
lvremove -ff $vg

# convert from 2-way to 3-way mirror - with tags and volume_list (bz683270)
lvcreate -l2 -m1 -n $lv1 $vg --addtag hello
lvconvert -i1 -m+1 $vg/$lv1 \
    --config 'activation { volume_list = [ "@hello" ] }'
lvremove -ff $vg

# convert from 2-way mirror to linear
lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0-1"
lvconvert -m-1 $vg/$lv1
check linear $vg $lv1
lvremove -ff $vg
# and now try removing a specific leg (bz453643)
lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0-1"
lvconvert -m0 $vg/$lv1 "$dev2"
check lv_on $vg $lv1 "$dev1"
lvremove -ff $vg

# convert from disklog to corelog, active
lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0-1"
lvconvert -f --mirrorlog core $vg/$lv1
check mirror $vg $lv1 core
lvremove -ff $vg

# convert from corelog to disklog, active
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg "$dev1" "$dev2"
lvconvert --mirrorlog disk $vg/$lv1 "$dev3:0-1"
check mirror $vg $lv1 "$dev3"
lvremove -ff $vg

# bz192865: lvconvert log of an inactive mirror lv
# convert from disklog to corelog, inactive
lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0-1"
lvchange -an $vg/$lv1
echo y | lvconvert -f --mirrorlog core $vg/$lv1
check mirror $vg $lv1 core
lvremove -ff $vg

# convert from corelog to disklog, inactive
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg "$dev1" "$dev2"
lvchange -an $vg/$lv1
lvconvert --mirrorlog disk $vg/$lv1 "$dev3:0-1"
check mirror $vg $lv1 "$dev3"
lvremove -ff $vg

# convert linear to 2-way mirror with 1 PV
lvcreate -l2 -n $lv1 $vg "$dev1"
not lvconvert -m+1 --mirrorlog core $vg/$lv1 "$dev1"
lvremove -ff $vg

# Start w/ 3-way mirror
# Test pulling primary image before mirror in-sync (should fail)
# Test pulling primary image after mirror in-sync (should work)
# Test that the correct devices remain in the mirror
lvcreate -l2 -m2 -n $lv1 $vg "$dev1" "$dev2" "$dev4" "$dev3:0"
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

lvcreate -l5 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0"
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
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg "$dev1" "$dev2"
lvconvert -m +1 -i1 $vg/$lv1

check mirror $vg $lv1 core
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# remove 1 mirror from corelog'ed mirror; should retain 'core' log type
lvcreate -l2 -m2 --corelog -n $lv1 $vg
lvconvert -m -1 -i1 $vg/$lv1

check mirror $vg $lv1 core
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 2
lvremove -ff $vg

# add 1 mirror then add 1 more mirror during conversion
# FIXME this has been explicitly forbidden?
#lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3":0
#lvconvert -m+1 -b $vg/$lv1 "$dev4"
#lvconvert -m+1 $vg/$lv1 "$dev5"
#
#check mirror $vg $lv1 "$dev3"
#check mirror_no_temporaries $vg $lv1
#check mirror_legs $vg $lv1 4
#lvremove -ff $vg

# Linear to mirror with mirrored log using --alloc anywhere
lvcreate -l2 -n $lv1 $vg "$dev1"
lvconvert -m +1 --mirrorlog mirrored --alloc anywhere $vg/$lv1 "$dev1" "$dev2"
should check mirror $vg $lv1
lvremove -ff $vg

# convert inactive mirror and start polling
lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0"
lvchange -an $vg/$lv1
lvconvert -m+1 $vg/$lv1 "$dev4"
lvchange -ay $vg/$lv1
lvconvert $vg/$lv1 # wait
check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
lvremove -ff $vg

# ---------------------------------------------------------------------
# removal during conversion

# "remove newly added mirror"
lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0"
lvconvert -m+1 -b $vg/$lv1 "$dev4"
lvconvert -m-1 $vg/$lv1 "$dev4"
lvconvert $vg/$lv1 # wait

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 2
lvremove -ff $vg

# "remove one of newly added mirrors"
lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0"
lvconvert -m+2 -b $vg/$lv1 "$dev4" "$dev5"
lvconvert -m-1 $vg/$lv1 "$dev4"
lvconvert $vg/$lv1 # wait

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# "remove from original mirror (the original is still mirror)"
lvcreate -l2 -m2 -n $lv1 $vg "$dev1" "$dev2" "$dev5" "$dev3:0"
lvconvert -m+1 -b $vg/$lv1 "$dev4"
lvconvert -m-1 $vg/$lv1 "$dev2"
lvconvert $vg/$lv1

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# "remove from original mirror (the original becomes linear)"
lvcreate -l2 -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0"
lvconvert -m+1 -b $vg/$lv1 "$dev4"
lvconvert -m-1 $vg/$lv1 "$dev2"
lvconvert $vg/$lv1

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 2
lvremove -ff $vg

# ---------------------------------------------------------------------

# "rhbz440405: lvconvert -m0 incorrectly fails if all PEs allocated"
lvcreate -l`pvs --noheadings -ope_count "$dev1"` -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3:0"
aux wait_for_sync $vg $lv1
lvconvert -m0 $vg/$lv1 "$dev1"
check linear $vg $lv1
lvremove -ff $vg

# "rhbz264241: lvm mirror doesn't lose it's "M" --nosync attribute after being down and the up converted"
lvcreate -l2 -m1 -n$lv1 --nosync $vg
lvconvert -m0 $vg/$lv1
lvconvert -m1 $vg/$lv1
lvs --noheadings -o attr $vg/$lv1 | grep '^ *m'
lvremove -ff $vg

# lvconvert from linear (on multiple PVs) to mirror
lvcreate -l 8 -n $lv1 $vg "$dev1:0-3" "$dev2:0-3"
lvconvert -m1 $vg/$lv1

should check mirror $vg $lv1
check mirror_legs $vg $lv1 2
lvremove -ff $vg

# BZ 463272: disk log mirror convert option is lost if downconvert option is also given
lvcreate -l1 -m2 --corelog -n $lv1 $vg "$dev1" "$dev2" "$dev3"
aux wait_for_sync $vg $lv1
lvconvert -m1 --mirrorlog disk $vg/$lv1
check mirror $vg $lv1
not check mirror $vg $lv1 core
lvremove -ff $vg

# ---
# add mirror and disk log

# "add 1 mirror and disk log"
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg "$dev1" "$dev2"

# FIXME on next line, specifying $dev3:0 $dev4 (i.e log device first) fails (!)
lvconvert -m+1 --mirrorlog disk -i1 $vg/$lv1 "$dev4" "$dev3:0"

check mirror $vg $lv1 "$dev3"
check mirror_no_temporaries $vg $lv1
check mirror_legs $vg $lv1 3
lvremove -ff $vg

# simple mirrored stripe
lvcreate -i2 -l10 -n $lv1 $vg
lvconvert -m1 -i1 $vg/$lv1
lvreduce -f -l1 $vg/$lv1
lvextend -f -l10 $vg/$lv1
lvremove -ff $vg/$lv1

# extents must be divisible
lvcreate -l15 -n $lv1 $vg
not lvconvert -m1 --corelog --stripes 2 $vg/$lv1
lvremove -ff $vg

# Should not be able to add images to --nosync mirror
# but should be able to after 'lvchange --resync'
lvcreate -m 1 -l1 -n $lv1 $vg --nosync
not lvconvert -m +1 $vg/$lv1
lvchange --resync -y $vg/$lv1
lvconvert -m +1 $vg/$lv1
lvremove -ff $vg

lvcreate -m 1 --corelog -l1 -n $lv1 $vg --nosync
not lvconvert -m +1 $vg/$lv1
lvchange --resync -y $vg/$lv1
lvconvert -m +1 $vg/$lv1
lvremove -ff $vg
