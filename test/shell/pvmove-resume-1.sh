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

# 2 pvmove LVs in 2 VGs (1 per VG)

. lib/inittest

aux prepare_pvs 4 30

vgcreate -s 128k $vg "$dev1"
vgcreate -s 128k $vg1 "$dev2"
pvcreate --metadatacopies 0 "$dev3"
pvcreate --metadatacopies 0 "$dev4"
vgextend $vg "$dev3"
vgextend $vg1 "$dev4"

# $1 resume fn
test_pvmove_resume() {
	lvcreate -an -Zn -l30 -n $lv1 $vg
	lvcreate -an -Zn -l30 -n $lv1 $vg1

	aux delay_dev "$dev3" 0 1000 $(get first_extent_sector "$dev3"):
	aux delay_dev "$dev4" 0 1000 $(get first_extent_sector "$dev4"):

	pvmove -i5 "$dev1" &
	PVMOVE=$!
	aux wait_pvmove_lv_ready "$vg-pvmove0" 300
	kill -9 $PVMOVE

	pvmove -i5 "$dev2" &
	PVMOVE=$!
	aux wait_pvmove_lv_ready "$vg1-pvmove0" 300
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
	while dmsetup status "$vg1-$lv1"; do dmsetup remove "$vg1-$lv1" || true; done
	while dmsetup status "$vg-pvmove0"; do dmsetup remove "$vg-pvmove0" || true; done
	while dmsetup status "$vg1-pvmove0"; do dmsetup remove "$vg1-pvmove0" || true; done

	check lv_attr_bit type $vg/pvmove0 "p"
	check lv_attr_bit type $vg1/pvmove0 "p"

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
	$1 2

	aux enable_dev "$dev3"
	aux enable_dev "$dev4"

	i=0
	while get lv_field $vg name -a | grep "^\[pvmove"; do
		# wait for 30 secs at max
		test $i -ge 300 && die "Pvmove is too slow or does not progress."
		sleep .1
		i=$((i + 1))
	done
	while get lv_field $vg1 name -a | grep "^\[pvmove"; do
		# wait for 30 secs at max
		test $i -ge 300 && die "Pvmove is too slow or does not progress."
		sleep .1
		i=$((i + 1))
	done

	lvremove -ff $vg $vg1
}

lvchange_single() {
	lvchange -aey $vg/$lv1
	lvchange -aey $vg1/$lv1

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq $1
}

lvchange_all() {
	lvchange -aey $vg/$lv1 $vg1/$lv1

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq $1
}

vgchange_single() {
	vgchange -aey $vg
	vgchange -aey $vg1

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq $1
}

vgchange_all()  {
	vgchange -aey $vg $vg1

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq $1
}

pvmove_fg() {
	# pvmove resume requires LVs active
	vgchange -aey --poll n $vg $vg1

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# vgchange must not spawn (thus finish) background polling
	get lv_field $vg name -a | grep "^\[pvmove0\]"
	get lv_field $vg1 name -a | grep "^\[pvmove0\]"

	# disable delay device
	# fg pvmove would take ages to complete otherwise
	aux enable_dev "$dev3"
	aux enable_dev "$dev4"

	pvmove -i0
}

pvmove_bg() {
	# pvmove resume requires LVs active
	vgchange -aey --poll n $vg $vg1

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# vgchange must not spawn (thus finish) background polling
	get lv_field $vg name -a | grep "^\[pvmove0\]"
	get lv_field $vg1 name -a | grep "^\[pvmove0\]"

	pvmove -b -i0
}

pvmove_fg_single() {
	# pvmove resume requires LVs active
	vgchange -aey --poll n $vg

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# vgchange must not spawn (thus finish) background polling
	get lv_field $vg name -a | grep "^\[pvmove0\]"
	get lv_field $vg1 name -a | grep "^\[pvmove0\]"

	# disable delay device
	# fg pvmove would take ages to complete otherwise
	aux enable_dev "$dev3"
	aux enable_dev "$dev4"

	pvmove -i0 "$dev1"
	pvmove -i0 "$dev2"
}

pvmove_bg_single() {
	# pvmove resume requires LVs active
	vgchange -aey --poll n $vg

	ps h -C lvm | tee out || true
	test $(wc -l < out) -eq 0

	# vgchange must not spawn (thus finish) background polling
	get lv_field $vg name -a | grep "^\[pvmove0\]"
	get lv_field $vg1 name -a | grep "^\[pvmove0\]"

	pvmove -i0 -b "$dev1"
	pvmove -i0 -b "$dev2"
}

test -e LOCAL_CLVMD && skip

test_pvmove_resume lvchange_single
test_pvmove_resume lvchange_all
test_pvmove_resume vgchange_single
test_pvmove_resume vgchange_all
test_pvmove_resume pvmove_fg
test_pvmove_resume pvmove_fg_single
test_pvmove_resume pvmove_bg
test_pvmove_resume pvmove_bg_single

vgremove -ff $vg $vg1
