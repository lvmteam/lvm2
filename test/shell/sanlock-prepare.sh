#!/bin/sh
# Copyright (C) 2008-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Set up things to run tests with sanlock'

. lib/utils
. lib/inittest

[ -z "$LVM_TEST_LOCK_TYPE_SANLOCK" ] && skip;

SANLOCK_CONF="/etc/sysconfig/sanlock"
create_sanlock_conf() {
	if test -a $SANLOCK_CONF; then
		if ! grep "created by lvm test suite" $SANLOCK_CONF; then
			rm $SANLOCK_CONF
		else
			mv $SANLOCK_CONF $SANLOCK_CONF.prelvmtest
		fi
	fi

	cp lib/test-sanlock-conf $SANLOCK_CONF
	echo "created new $SANLOCK_CONF"
}

prepare_lvmlockd_sanlock() {
	if pgrep lvmlockd ; then
		echo "Cannot run while existing lvmlockd process exists"
		exit 1
	fi

	if pgrep sanlock ; then
		echo "Cannot run while existing sanlock process exists"
		exit 1
	fi

	create_sanlock_conf

	# FIXME: use 'systemctl start sanlock' once we can pass options
	sanlock daemon -U sanlock -G sanlock -w 0 -e testhostname
	sleep 1
	if ! pgrep sanlock; then
		echo "Failed to start sanlock"
		exit 1
	fi

	# FIXME: use 'systemctl start lvm2-lvmlockd' once we can pass -o 2
	lvmlockd -o 2
	sleep 1
	if ! pgrep lvmlockd; then
		echo "Failed to start lvmlockd"
		exit 1
	fi
}

# Create a device and a VG that are both outside the scope of
# the standard lvm test suite so that they will not be removed
# and will remain in place while all the tests are run.
#
# Use this VG to hold the sanlock global lock which will be used
# by lvmlockd during other tests.
#
# This script will be run before any standard tests are run.
# After all the tests are run, another script will be run
# to remove this VG and device.

GL_DEV="/dev/mapper/GL_DEV"
GL_FILE="$PWD/gl_file.img"
dmsetup remove GL_DEV || true
rm -f "$GL_FILE"
dd if=/dev/zero of="$GL_FILE" bs=$((1024*1024)) count=1024 2> /dev/null
GL_LOOP=$(losetup -f "$GL_FILE" --show)
echo "0 `blockdev --getsize $GL_LOOP` linear $GL_LOOP 0" | dmsetup create GL_DEV

prepare_lvmlockd_sanlock

vgcreate --config 'devices { global_filter=["a|GL_DEV|", "r|.*|"] filter=["a|GL_DEV|", "r|.*|"]}' --lock-type sanlock glvg $GL_DEV

vgs --config 'devices { global_filter=["a|GL_DEV|", "r|.*|"] filter=["a|GL_DEV|", "r|.*|"]}' -o+locktype,lockargs glvg

