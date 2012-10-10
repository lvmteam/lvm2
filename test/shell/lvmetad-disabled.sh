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

. lib/test

test -e LOCAL_LVMETAD || skip
kill $(cat LOCAL_LVMETAD)

test -e $LVMETAD_PIDFILE && skip
lvmetad
test -e $LVMETAD_PIDFILE
cp $LVMETAD_PIDFILE LOCAL_LVMETAD
pvs 2>&1 | not grep "lvmetad is running"
aux lvmconf "global/use_lvmetad = 0"
pvs 2>&1 | grep "lvmetad is running"

kill $(cat $LVMETAD_PIDFILE)
not ls $LVMETAD_PIDFILE
