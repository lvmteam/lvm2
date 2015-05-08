#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/inittest

test -e LOCAL_LVMETAD || skip
test -e LOCAL_LVMPOLLD && skip

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
