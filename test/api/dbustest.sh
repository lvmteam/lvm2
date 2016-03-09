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

aux prepare_lvmdbusd
$abs_top_builddir/test/dbus/lvmdbustest.py -v
