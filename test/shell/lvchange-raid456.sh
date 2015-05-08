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

TEST_RAID=raid456

. shell/lvchange-raid.sh

test -e LOCAL_LVMPOLLD && skip

aux raid456_replace_works || skip
aux have_raid 1 5 2 || skip

run_types raid4 -i 2 "$dev1" "$dev2" "$dev3" "$dev4"
run_types raid5 -i 2 "$dev1" "$dev2" "$dev3" "$dev4"
run_types raid6 -i 3 "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"

vgremove -ff $vg
