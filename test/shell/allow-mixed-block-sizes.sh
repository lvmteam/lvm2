#!/usr/bin/env bash

# Copyright (C) 2019-2021 Red Hat, Inc. All rights reserved.
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
which fallocate || skip

fallocate -l 2M loopa
fallocate -l 2M loopb

# Fight a weird occasional race in losetup usage:
#
# losetup: loopa: failed to set up loop device: Resource temporarily unavailable
# 	loop0: detected capacity change from 0 to 4096
# 	loop_set_block_size: loop0 () has still dirty pages (nrpages=2)
for i in {1..5} ; do
	LOOP1=$(losetup -f loopa --sector-size 4096 --show || true)
	test -n "$LOOP1" && break
done
for i in {1..5} ; do
	LOOP2=$(losetup -f loopb --show || true)
	test -n "$LOOP2" && break
done

# prepare devX mapping so it works for real & fake dev dir
d=1
for i in "$LOOP1" "$LOOP2"; do
	echo "$i"
	m=${i##*loop}
	test -e "$DM_DEV_DIR/loop$m" || mknod "$DM_DEV_DIR/loop$m" b 7 "$m"
	eval "dev$d=\"$DM_DEV_DIR/loop$m\""
	d=$(( d + 1 ))
done

aux extend_filter "a|$dev1|" "a|$dev2|"
aux extend_devices "$dev1" "$dev2"

not vgcreate --config 'devices/allow_mixed_block_sizes=0' $vg "$dev1" "$dev2"
vgcreate --config 'devices/allow_mixed_block_sizes=1' $vg "$dev1" "$dev2"
vgs --config 'devices/allow_mixed_block_sizes=1' $vg

for i in "$dev1" "$dev2" ; do
	aux wipefs_a "$i"
	# FIXME - we are not missing notification for hinting
	# likely in more places - as the test should be able to work without
	# system's udev working only on real /dev dir.
	# aux notify_lvmetad "$i"
done

vgcreate --config 'devices/allow_mixed_block_sizes=1' $vg "$dev1"
vgs --config 'devices/allow_mixed_block_sizes=1' $vg
not vgextend --config 'devices/allow_mixed_block_sizes=0' $vg "$dev2"
vgextend --config 'devices/allow_mixed_block_sizes=1' $vg "$dev2"

losetup -d "$LOOP1"
losetup -d "$LOOP2"
rm loopa
rm loopb
