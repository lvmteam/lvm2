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

aux have_cache 1 3 0 || skip
aux prepare_vg 3

lvcreate --type cache-pool -an -v -L 2 -n cpool $vg
lvcreate -H -L 4 -n corigin --cachepool $vg/cpool
lvcreate -n noncache -l 1 $vg

not lvchange --cachepolicy policy=mq $vg/noncache

lvchange --cachepolicy policy=mq --cachepolicy migration_threshold=333 $vg/corigin
dmsetup status | grep $vg-corigin | grep 'migration_threshold 333'
lvchange --refresh $vg/corigin
dmsetup status | grep $vg-corigin | grep 'migration_threshold 333'
lvchange -an $vg
lvchange -ay $vg
dmsetup status | grep $vg-corigin | grep 'migration_threshold 333'

lvchange --cachepolicy 'migration_threshold = 233 sequential_threshold = 13' $vg/corigin
dmsetup status | grep $vg-corigin | grep 'migration_threshold 233'
dmsetup status | grep $vg-corigin | grep 'sequential_threshold 13'

vgremove -f $vg
