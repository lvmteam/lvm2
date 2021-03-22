#!/usr/bin/env bash

# Copyright (C) 2012-2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# . different PV/VG's that happen to have the same PVID
# . a single PV/VG cloned to another device
# . dm wrapper around a PV
# . a single PV/VG cloned plus a dm wrapper (two separate dups of a PV)


# Reuse same test just use raid level 1
export MD_LEVEL=1
. ./shell/duplicate-pvs-md0.sh
