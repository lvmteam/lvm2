#!/bin/sh
# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_CLVMD=1

. lib/inittest

# Will default to skip until we can get this reviewed
skip

aux prepare_pvs 6

# Copy the needed file to run on the system bus if it doesn't
# already exist
if [ ! -f /etc/dbus-1/system.d/com.redhat.lvmdbus1.conf ]; then
	install -m 644 $abs_top_builddir/scripts/com.redhat.lvmdbus1.conf /etc/dbus-1/system.d/.
fi

# Setup the python path so we can run
export PYTHONPATH=$abs_top_builddir/daemons

if true; then

aux prepare_lvmdbusd
$abs_top_builddir/test/dbus/lvmdbustest.py -v

else

# Start the dbus service
$abs_top_builddir/daemons/lvmdbusd/lvmdbusd --debug --udev > debug.log_lvmdbusd 2>&1 &

# Give the service some time to start before we try to run the
# unit test
sleep 1

LVM_DBUS_PID=$(ps aux | grep lvmdbus[d] |  awk '{print $2}')
if [ "CHK${LVM_DBUS_PID}" == "CHK" ];then
	echo "Failed to start lvmdbusd daemon"
	exit 1
fi
END

# Run all the unit tests
$abs_top_builddir/test/dbus/lvmdbustest.py -v || fail=$?

# We can run individual unit tests by doing this
# $abs_top_builddir/test/dbus/lvmdbustest.py -v TestDbusService.test_snapshot_merge

echo "Stopping service"
kill $LVM_DBUS_PID || {
	sleep 1
        kill -9 $LVM_DBUS_PID
}
wait

exit ${fail:-"0"}

fi
