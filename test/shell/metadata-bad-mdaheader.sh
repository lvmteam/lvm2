#!/usr/bin/env bash

# Copyright (C) 2008-2013,2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/inittest

xxd -v || skip

aux prepare_devs 3
get_devs

#
# Test corrupted mda_header.version field, which also
# causes the mda_header checksum to be bad.
#
# FIXME: if a VG has only a single PV, this repair
# doesn't work since there's no good PV to get
# metadata from.  A more advanced repair capability
# is needed.
#

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

pvs

# read mda_header which is 4k from start of disk
dd if="$dev1" of=meta1 bs=4k count=1 skip=1

# convert binary to text
xxd meta1 > meta1.txt

# Corrupt mda_header by changing the version field from 0100 to 0200
sed 's/0000010:\ 304e\ 2a3e\ 0100\ 0000\ 0010\ 0000\ 0000\ 0000/0000010:\ 304e\ 2a3e\ 0200\ 0000\ 0010\ 0000\ 0000\ 0000/' meta1.txt > meta1-bad.txt

# convert text to binary
xxd -r meta1-bad.txt > meta1-bad

# write bad mda_header back to disk
dd if=meta1-bad of="$dev1" bs=4k seek=1

# pvs reports bad metadata header
pvs 2>&1 | tee out
grep "bad metadata header" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# bad metadata in one mda doesn't prevent using
# the VG since other mdas are fine and usable
lvcreate -l1 $vg


vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "bad metadata header" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

vgchange -an $vg
vgremove -ff $vg


