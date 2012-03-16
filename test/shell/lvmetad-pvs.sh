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

aux prepare_pvs 1 20000
pvs $(cat DEVICES) | grep "$dev1"

# check for PV size overflows
pvs $(cat DEVICES) | grep 19.53g
pvs $(cat DEVICES) | not grep 16.00e
