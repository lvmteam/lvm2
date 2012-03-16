#!/bin/sh
# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#
# Testcase for bugzilla #621173 
# excercises partition table scanning code path
#


LVM_TEST_CONFIG_DEVICES="types = [\"device-mapper\", 142]"

. lib/test

which sfdisk || skip

aux prepare_pvs 1 30

pvs "$dev1"

# create small partition table
echo "1 2" | sfdisk "$dev1"

pvs "$dev1"
