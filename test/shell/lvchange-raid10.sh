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

TEST_RAID=raid10

. shell/lvchange-raid.sh

test -e LOCAL_LVMPOLLD && skip

aux have_raid 1 5 2 || skip

run_types raid10 -m 1 -i 2 "$dev1" "$dev2" "$dev3" "$dev4"

vgremove -ff $vg
