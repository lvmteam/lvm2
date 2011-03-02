#!/bin/bash
# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux prepare_vg 5
aux prepare_dmeventd

which mkfs.ext2 || exit 200

lvcreate -m 3 --ig -L 1 -n 4way $vg
lvchange --monitor y $vg/4way
lvcreate -m 2 --ig -L 1 -n 3way $vg
lvchange --monitor y $vg/3way

dmeventd -R -f &
echo "$!" > LOCAL_DMEVENTD

sleep 1 # wait a bit, so we talk to the new dmeventd later

lvchange --monitor y --verbose $vg/3way 2>&1 | tee lvchange.out
grep 'already monitored' lvchange.out
lvchange --monitor y --verbose $vg/4way 2>&1 | tee lvchange.out
grep 'already monitored' lvchange.out

# now try what happens if no dmeventd is running
kill -9 `cat LOCAL_DMEVENTD`
dmeventd -R -f &
echo "$!" > LOCAL_DMEVENTD
sleep 3
lvchange --monitor y --verbose $vg/3way 2>&1 | tee lvchange.out
not grep 'already monitored' lvchange.out
