#!/bin/sh
# Copyright (C) 2010-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

extend() {
	lvextend --use-policies --config "activation { snapshot_autoextend_threshold = $1 }" $vg/snap
}

write_() {
	dd if=/dev/zero of="$DM_DEV_DIR/$vg/snap" bs=1k count=$2 seek=$1
}

percent_() {
	get lv_field $vg/snap snap_percent | cut -d. -f1
}

wait_for_change_() {
	# dmeventd only checks every 10 seconds :(
	for i in $(seq 1 15) ; do
		test "$(percent_)" != "$1" && return
		sleep 1
	done

	return 1  # timeout
}

aux prepare_dmeventd
aux prepare_vg 2

lvcreate -aey -L16M -n base $vg
lvcreate -s -L4M -n snap $vg/base

write_ 0 1000
test 24 -eq $(percent_)

lvchange --monitor y $vg/snap

write_ 1000 1700
pre=$(percent_)
wait_for_change_ $pre
test $pre -gt $(percent_)

# check that a second extension happens; we used to fail to extend when the
# utilisation ended up between THRESH and (THRESH + 10)... see RHBZ 754198
# (the utilisation after the write should be 57 %)

write_ 2700 2000
pre=$(percent_)
wait_for_change_ $pre
test $pre -gt $(percent_)

vgremove -f $vg
