#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Check pvmove behavior when it's progress and machine is rebooted

. lib/inittest

aux prepare_pvs 3 60

vgcreate -s 128k $vg "$dev1" "$dev2"
pvcreate --metadatacopies 0 "$dev3"
vgextend $vg "$dev3"

for mode in "--atomic" ""
do

# Create multisegment LV
lvcreate -an -Zn -l5 -n $lv1 $vg "$dev1"
lvextend -l+10 $vg/$lv1 "$dev2"
lvextend -l+5 $vg/$lv1 "$dev1"
lvextend -l+10 $vg/$lv1 "$dev2"

# Slowdown writes
aux delay_dev "$dev3" 0 100

pvmove -i0 -n $vg/$lv1 "$dev1" "$dev3" $mode &
PVMOVE=$!
# Let's wait a bit till pvmove starts and kill it
while not dmsetup status "$vg-pvmove0"; do sleep .1; done
kill -9 $PVMOVE
wait

# Simulate reboot - forcibly remove related devices

# First take down $lv1 then it's pvmove0
while dmsetup status "$vg-$lv1"; do dmsetup remove "$vg-$lv1" || true; done
while dmsetup status "$vg-pvmove0"; do dmsetup remove "$vg-pvmove0" || true; done

# Check we really have pvmove volume
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

# Only PVs should be left in table...
dmsetup table

# Restart pvmove
# use exclusive activation to have usable pvmove without cmirrord
vgchange -aey $vg
#sleep 2
#pvmove

pvmove --abort

# Restore delayed device back
aux delay_dev "$dev3"

lvs -a -o+devices $vg

lvremove -ff $vg
done

vgremove -ff $vg
