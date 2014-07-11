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

. lib/inittest

aux have_thin 1 0 0 || skip
aux have_raid 1 4 0 || skip

aux prepare_vg 4

# create RAID LVs for data and metadata volumes
lvcreate -aey --nosync -L10M --type raid1 -m1 -n $lv1 $vg
lvcreate -aey --nosync -L8M --type raid1 -m1 -n $lv2 $vg
lvchange -an $vg/$lv1

# conversion fails for internal volumes
invalid lvconvert --thinpool $vg/${lv1}_rimage_0
invalid lvconvert --yes --thinpool $vg/$lv1 --poolmetadata $vg/${lv2}_rimage_0

lvconvert --yes --thinpool $vg/$lv1 --poolmetadata $vg/$lv2

vgremove -ff $vg
