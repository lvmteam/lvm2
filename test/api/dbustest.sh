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

# This allows us to run without installing
# com.redhat.lvmdbus1.conf to /etc/dbus-1/system.d/
# but in normal operation it needs to be on system bus
export LVMDBUSD_USE_SESSION="True"

# Setup the python path so we can run
export PYTHONPATH=$abs_top_builddir/daemons

# Where should we be logging the output of the daemon when not running as
# a systemd service
# Start the dbus service
$abs_top_builddir/daemons/lvmdbusd/lvmdbusd --debug --udev > /tmp/lvmdbusd.log 2>&1 &

# Give the service some time to start before we try to run the
# unit test
sleep 1

LVM_DBUS_PID=$(ps aux | grep lvmdbus[d] |  awk '{print $2}')
if [ "CHK${LVM_DBUS_PID}" == "CHK" ];then
	echo "Failed to start lsmdbusd daemon"
	exit 1
fi

# Run all the unit tests
# Are we already logging stdout & stderror?
$abs_top_builddir/test/dbus/lvmdbustest.py -v > /tmp/lvmdbustest.log 2>&1

# We can run individual unit tests by doing this
# $abs_top_builddir/test/dbus/lvmdbustest.py -v TestDbusService.test_snapshot_merge

# I'm guessing there is a better way to handle this with the built in test env.
if [ $? -eq 0 ]; then
	rm -f /tmp/lvmdbusd.log
	rm -f /tmp/lvmdbustest.log
fi

echo "Stopping service"
kill $LVM_DBUS_PID
