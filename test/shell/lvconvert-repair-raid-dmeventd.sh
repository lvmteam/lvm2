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

test -e LOCAL_LVMPOLLD && skip

which mkfs.ext3 || skip
aux have_raid 1 3 0 || skip

aux prepare_dmeventd
aux prepare_vg 5

lvcreate -aey --type raid1 -m 3 --ignoremonitoring -L 1 -n 4way $vg
lvchange --monitor y $vg/4way
lvs -a -o all,lv_modules $vg
lvdisplay --maps $vg
aux disable_dev "$dev2" "$dev4"
mkfs.ext3 "$DM_DEV_DIR/$vg/4way"
sleep 10 # FIXME: need a "poll" utility, akin to "check"
aux enable_dev "$dev2" "$dev4"

vgremove -ff $vg
