#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test secure table is not leaking data in user land

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1

# AES key matching rot13 string from dmsecuretest.c */
SECURE="434e0cbab02ca68ffba9268222c3789d703fe62427b78b308518b3228f6a2122"
SECURE1=${SECURE:0:16}
SECURE2=${SECURE:16:16}
SECURE3=${SECURE:32:16}
SECURE4=${SECURE:48:16}

. lib/inittest

DMTEST="${PREFIX}-test-secure"

# Test needs installed gdb package with gcore app
which gcore || skip

aux driver_at_least 4 6 || skip

# ensure we can create devices (uses dmsetup, etc)
aux prepare_devs 1

# check both code versions - linked libdm  and internal device_mapper version
# there should not be any difference
for i in securetest dmsecuretest ; do

# 1st. try with empty table
# 2nd. retry with already exiting DM node - exercize error path also wipes
for j in empty existing ; do

rm -f cmdout
"$i" "$dev1" "$DMTEST" >cmdout 2>&1 &
PID=$!
for k in $(seq 1 20); do
	sleep .1
	lines=$(wc -l < cmdout 2>/dev/null || true)
	test "${lines:-0}" = "0" || break
done

# 0 8192 crypt aes-xts-plain64 434e0cbab02ca68ffba9268222c3789d703fe62427b78b308518b3228f6a2122 0 253:0 8192
# crypt device should be loaded
dmsetup status "$DMTEST"

# generate core file for running&sleeping binary
gcore "$PID" | tee out || skip

# check we capture core while  dmsecuretest was already sleeping
grep -e "nanosleep\|kernel_vsyscall" out
kill "$PID" || true
wait

cat cmdout

# $SECURE string must NOT be present in core file
for str in "$SECURE" "$SECURE1" "$SECURE2" "$SECURE3" "$SECURE4"; do
	not grep "$str" "core.$PID"
done || {
	## cp "core.$PID" /dev/shm/core
	should dmsetup remove "$DMTEST" # go around weird bugs
	die "!!! Secure string $str found present in core.$PID !!!"
}
rm -f "core.$PID"

if test "$j" = empty ; then
	not grep "Device or resource busy" cmdout
else
	# Device should be already present resulting into error message
	grep "Device or resource busy" cmdout
	dmsetup remove "$DMTEST"
fi

done

done
