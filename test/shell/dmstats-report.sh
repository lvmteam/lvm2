#!/bin/sh
# Copyright (C) 2009-2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/inittest

# ensure we can create devices (uses dmsetup, etc)
aux prepare_devs 1

# prepare a stats region with a histogram
dmstats create --bounds 10ms,20ms,30ms "$dev1"

# basic dmstats report commands
dmstats report
dmstats report --count 1
dmstats report --histogram

