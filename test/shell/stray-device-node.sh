#!/usr/bin/env bash

# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

aux prepare_devs 3
get_devs

# Avoid manipulation with real /dev dir
test "$DM_DEV_DIR" = "/dev" && skip "Skipping stray test on real /dev dir"

cp -r "$dev1" "$DM_DEV_DIR/stray"

vgcreate $SHARED "$vg" "${DEVICES[@]}"
lvcreate -an -Zn --type mirror -m 1 -l 1 -n mirror $vg
aux disable_dev "$dev1"
# FIXME:
# for the .cache use case we need to run pvscan
# to keep clvmd in sync.
pvscan
vgreduce --removemissing --force $vg
aux enable_dev "$dev1"

rm -f "$DM_DEV_DIR/stray"
