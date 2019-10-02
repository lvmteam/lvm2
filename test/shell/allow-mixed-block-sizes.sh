#!/usr/bin/env bash

# Copyright (C) 2019 Red Hat, Inc. All rights reserved.
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

losetup -h | grep sector-size || skip

dd if=/dev/zero of=loopa bs=$((1024*1024)) count=2 2> /dev/null
dd if=/dev/zero of=loopb bs=$((1024*1024)) count=2 2> /dev/null
LOOP1=$(losetup -f loopa --sector-size 4096 --show)
LOOP2=$(losetup -f loopb --show)

echo $LOOP1
echo $LOOP2

aux extend_filter "a|$LOOP1|"
aux extend_filter "a|$LOOP2|"

not vgcreate --config 'devices {allow_mixed_block_sizes=0 scan="/dev"}' $vg $LOOP1 $LOOP2
vgcreate --config 'devices {allow_mixed_block_sizes=1 scan="/dev"}' $vg $LOOP1 $LOOP2
vgs --config 'devices {allow_mixed_block_sizes=1 scan="/dev"}' $vg

aux wipefs_a $LOOP1
aux wipefs_a $LOOP2

vgcreate --config 'devices {allow_mixed_block_sizes=1 scan="/dev"}' $vg $LOOP1
vgs --config 'devices {allow_mixed_block_sizes=1 scan="/dev"}' $vg
not vgextend --config 'devices {allow_mixed_block_sizes=0 scan="/dev"}' $vg $LOOP2
vgextend --config 'devices {allow_mixed_block_sizes=1 scan="/dev"}' $vg $LOOP2

losetup -d $LOOP1
losetup -d $LOOP2
rm loopa
rm loopb

