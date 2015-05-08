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

# Testing renaming snapshots (had problem in cluster)
# https://bugzilla.redhat.com/show_bug.cgi?id=1136925

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux prepare_vg 1

lvcreate -aey -L1 -n $lv1 $vg
lvcreate -s -L1 -n $lv2 $vg/$lv1
lvrename $vg/$lv2 $vg/$lv3
lvremove -f $vg/$lv1

vgremove -f $vg
