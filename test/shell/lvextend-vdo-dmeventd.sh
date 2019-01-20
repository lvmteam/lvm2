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

# Test autoextension of VDO pool volume


SKIP_WITH_LVMPOLLD=1

. lib/inittest

percent_() {
	get lv_field $vg/vpool data_percent | cut -d. -f1
}

wait_for_change_() {
	# dmeventd only checks every 10 seconds :(
	for i in $(seq 1 25) ; do
		test "$(percent_)" != "$1" && return
		sleep 1
	done

	return 1  # timeout
}

aux have_vdo 6 2 0 || skip

aux prepare_dmeventd

aux lvmconf "activation/vdo_pool_autoextend_percent = 20" \
	    "activation/vdo_pool_autoextend_threshold = 70" \
	    "allocation/vdo_slab_size_mb = 128"

aux prepare_vg 1 9000
lvcreate --vdo -V2G -L4G -n $lv1 $vg/vpool

pre=$(percent_)
# Check out VDO pool is bellow 70%
test "$pre" -lt 70

# Fill space to be over 70%
dd if=/dev/urandom of="$DM_DEV_DIR/mapper/$vg-$lv1" bs=1M count=80 conv=fdatasync

# Should be now over 70%
pre=$(percent_)
test "$pre" -ge 70

wait_for_change_ $pre

pre=$(percent_)
# Check out VDO pool gets again bellow 70%
test "$pre" -lt 70 || die "Data percentage has not changed bellow 70%!"

# 4G * 1.2   (20%) ->  4.8G
check lv_field $vg/vpool size "4.80g"
check lv_field $vg/$lv1 size "2.00g"

lvs -a $vg

vgremove -f $vg
