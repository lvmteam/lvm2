#!/bin/bash
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# test restore operation of thin pool metadata

. lib/test

#
# Main
#
aux have_thin 1 0 0 || skip

aux prepare_vg 2

lvcreate -T -L8M $vg/pool -V10M -n $lv1

vgcfgbackup -f backup $vg

# use of --force is mandatory
not vgcfgrestore -f backup $vg

vgcfgrestore -f backup --force $vg

check lv_field $vg/pool transaction_id 1

vgremove -f $vg
