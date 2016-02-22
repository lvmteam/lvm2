#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITHOUT_LVMETAD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

kill $(< LOCAL_LVMETAD)
while test -e "$TESTDIR/lvmetad.socket"; do echo -n .; sleep .1; done # wait for the socket close
test ! -e "$LVM_LVMETAD_PIDFILE"

lvmetad
while ! test -e "$TESTDIR/lvmetad.socket"; do echo -n .; sleep .1; done # wait for the socket
test -e "$LVM_LVMETAD_PIDFILE"
cp "$LVM_LVMETAD_PIDFILE" LOCAL_LVMETAD

pvs 2>&1 | not grep "lvmetad is running"
aux lvmconf "global/use_lvmetad = 0"
pvs 2>&1 | grep "lvmetad is running"

kill $(< "$LVM_LVMETAD_PIDFILE")
not ls "$LVM_LVMETAD_PIDFILE"
