#!/usr/bin/env bash

# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1

. lib/inittest

xxd -v || skip

aux prepare_devs 1 256
get_devs

# Fill with random data so if the space between metadata
# copies are not zeroed the grep for zeros will fail.
dd if=/dev/urandom of="$dev1" bs=1M count=1 || true
dd if=/dev/urandom of="$dev1" bs=1M skip=15 count=1 || true

pvcreate --pvmetadatacopies 2 "$dev1"

vgcreate $SHARED "$vg" "$dev1"

for i in `seq 1 50`; do lvcreate -l1 -an $vg; done

# Check metadata copies are separated by zeroes in the first mda

dd if="$dev1" of=meta.raw bs=1M count=1

xxd meta.raw > meta.txt

# to help debug if the next grep fails
ls -l meta.txt
head -n 100 meta.txt
grep -A4 -B4 '01200:' meta.txt

_vg="$vg "
_vg="${_vg:0:16}"
grep -B1 "$_vg" meta.txt > meta.vg

cat meta.vg

grep -v "$_vg" meta.vg > meta.zeros

cat meta.zeros

grep '0000 0000 0000 0000 0000 0000 0000 0000' meta.zeros > meta.count

cat meta.count | wc -l

# wc will often equal 51, but some natural variability in
# metadata locations/content mean that some lines do not
# require a full line of zero padding, and will not match
# the grep for a full row of zeros.  So, check that more
# than 20 lines match the full row of zeros (this is a
# random choice, and this isn't a perfect way to test for
# zero padding.)

test "$(cat meta.count | wc -l)" -gt 20

rm meta.raw meta.txt meta.vg meta.zeros meta.count

#
# Check metadata copies are separated by zeroes in the second mda
#

dd if="$dev1" of=meta.raw bs=1M seek=15 count=1

xxd meta.raw > meta.txt

grep -B1 "$_vg" meta.txt > meta.vg

cat meta.vg

grep -v "$_vg" meta.vg > meta.zeros

cat meta.zeros

grep '0000 0000 0000 0000 0000 0000 0000 0000' meta.zeros > meta.count

cat meta.count | wc -l

test "$(cat meta.count | wc -l)" -gt 20

vgremove -ff $vg
