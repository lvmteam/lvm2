#!/bin/sh
# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Check whether all available pvmove resume methods works as expected.
# lvchange is able to resume pvmoves in progress.

# Moving 2 LVs in VG variant

. lib/inittest

aux prepare_pvs 9 30

vgcreate -s 128k $vg "$dev1"
pvcreate --metadatacopies 0 "$dev2"
vgextend $vg "$dev2"

test_pvmove_resume() {
	# 2 LVs on same device
	lvcreate -an -Zn -l15 -n $lv1 $vg "$dev1"
	lvcreate -an -Zn -l15 -n $lv2 $vg "$dev1"

	aux delay_dev "$dev2" 0 1000 $(get first_extent_sector "$dev2"):

	pvmove -i5 "$dev1" &
	PVMOVE=$!
	aux wait_pvmove_lv_ready "$vg-pvmove0" 300
	kill -9 $PVMOVE

	if test -e LOCAL_LVMPOLLD ; then
		# inforestart lvmpolld
		kill $(< LOCAL_LVMPOLLD)
		for i in $(seq 1 100) ; do
			test $i -eq 100 && die "Shutdown of lvmpolld is too slow."
			test -e "$LVM_LVMPOLLD_PIDFILE" || break
			sleep .1
		done # wait for the pid removal
		aux prepare_lvmpolld
	fi

	wait

	while dmsetup status "$vg-$lv1"; do dmsetup remove "$vg-$lv1" || true; done
	while dmsetup status "$vg-$lv2"; do dmsetup remove "$vg-$lv2" || true; done
	while dmsetup status "$vg-pvmove0"; do dmsetup remove "$vg-pvmove0" || true; done

	check lv_attr_bit type $vg/pvmove0 "p"

	if test -e LOCAL_CLVMD ; then
		# giveup all clvmd locks (faster then restarting clvmd)
		# no deactivation happen, nodes are already removed
		#vgchange -an $vg
		# FIXME: However above solution has one big problem
		# as clvmd starts to abort on internal errors on various
		# errors, based on the fact pvmove is killed -9
		# Restart clvmd
		kill $(< LOCAL_CLVMD)
		for i in $(seq 1 100) ; do
			test $i -eq 100 && die "Shutdown of clvmd is too slow."
			test -e "$CLVMD_PIDFILE" || break
			sleep .1
		done # wait for the pid removal
		aux prepare_clvmd
	fi

	if test -e LOCAL_LVMETAD ; then
		# Restart lvmetad
		kill $(< LOCAL_LVMETAD)
		aux prepare_lvmetad
	fi

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# call resume function (see below)
	# with expected number of spawned
	# bg polling as parameter
	$1 1

	aux enable_dev "$dev2"

	i=0
	while get lv_field $vg name -a | grep "^\[pvmove"; do
		# wait for 30 secs at max
		test $i -ge 300 && die "Pvmove is too slow or does not progress."
		sleep .1
		i=$((i + 1))
	done

	lvremove -ff $vg
}

lvchange_single() {
	lvchange -aey $vg/$lv1
	lvchange -aey $vg/$lv2
}

lvchange_all() {
	lvchange -aey $vg/$lv1 $vg/$lv2

	# we don't want to spawn more than $1 background pollings
	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq $1 || should false
}

vgchange_single() {
	vgchange -aey $vg

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq $1
}

pvmove_fg() {
	# pvmove resume requires LVs active
	vgchange -aey --poll n $vg

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# vgchange must not spawn (thus finish) background polling
	get lv_field $vg name -a | grep "^\[pvmove0\]"

	aux enable_dev "$dev2"

	pvmove -i0
}

pvmove_bg() {
	# pvmove resume requires LVs active
	vgchange -aey --poll n $vg

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# vgchange must not spawn (thus finish) background polling
	get lv_field $vg name -a | grep "^\[pvmove0\]"

	pvmove -b -i0
}

pvmove_fg_single() {
	# pvmove resume requires LVs active
	vgchange -aey --poll n $vg

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# vgchange must not spawn (thus finish) background polling
	get lv_field $vg name -a | grep "^\[pvmove0\]"

	aux enable_dev "$dev2"

	pvmove -i0 "$dev1"
}

pvmove_bg_single() {
	# pvmove resume requires LVs active
	vgchange -aey --poll n $vg

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# vgchange must not spawn (thus finish) background polling
	get lv_field $vg name -a | grep "^\[pvmove0\]"

	pvmove -i0 -b "$dev1"
}

test -e LOCAL_CLVMD && skip

test_pvmove_resume lvchange_single
test_pvmove_resume lvchange_all
test_pvmove_resume vgchange_single
test_pvmove_resume pvmove_fg
test_pvmove_resume pvmove_fg_single
test_pvmove_resume pvmove_bg
test_pvmove_resume pvmove_bg_single

vgremove -ff $vg
