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

aux prepare_vg 4

# Attempt to create snapshot of a mirror origin - should fail
lvcreate -aey --type mirror -m 1 -L 10M -n lv $vg

lvcreate -s $vg/lv -L 10M -n snap

# Down-convert (mirror -> linear) under a snapshot
lvconvert -m0 $vg/lv

# Up-convert (linear -> mirror)
lvconvert --type mirror -m2 $vg/lv

# Down-convert (mirror -> mirror)
lvconvert -m 1 $vg/lv

# Up-convert (mirror -> mirror) -- Not supported!
not lvconvert -m2 $vg/lv

# Log conversion (disk -> core)
lvconvert --mirrorlog core $vg/lv

# Log conversion (core -> mirrored)
# FIXME on cluster
test -e LOCAL_CLVMD && SHOULD=should
$SHOULD lvconvert --mirrorlog mirrored $vg/lv

# Log conversion (mirrored -> core)
lvconvert --mirrorlog core $vg/lv

# Log conversion (core -> disk)
lvconvert --mirrorlog disk $vg/lv

## Clean-up
vgremove -f $vg
