#!/usr/bin/env bash

# Copyright (C) 2009-2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

lvm version

lvm pvmove --version|sed -n "1s/.*: *\([0-9][^ ]*\) .*/\1/p" | tee version

# check LVM_SUPPRESS_FD_WARNINGS supression works
exec 3< version
lvs 2>err
# without suppression command prints message about leaked descriptor
grep "leaked" err
LVM_SUPPRESS_FD_WARNINGS=1 lvs 2>err
# with suppression there should be no such message
not grep "leaked" err
exec 3<&-

# ensure they are the same
diff -u version lib/version-expected

dmstats version |sed -n "1s/.*: *\([0-9][^ ]*\) .*/\1/p" | tee dmstats-version

# ensure dmstats version matches build
diff -u dmstats-version lib/dm-version-expected

# ensure we can create devices (uses dmsetup, etc)
aux prepare_devs 5
get_devs

# ensure we do not crash on a bug in config file
aux lvmconf 'log/prefix = 1""'
not lvs "${DEVICES[@]}"

# validate testing machine with its services is in expected state and will not interfere with tests
if systemctl -a >out 2>/dev/null ; then
	for i in dm-event lvm2-lvmpolld lvm2-monitor ; do
		grep $i out > mout || continue
		grep -v masked mout || continue
		should not echo "Present unmasked $i service/socket may randomize testing results!"
		echo "+++++ Stop & Mask with systemctl +++++"
		touch show_out
	done
	test ! -e show_out || cat out
fi
