#!/usr/bin/env bash

# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
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

_wait_for_dmeventd() {
	local local=${1-}
	local pid=
	for i in {1..50}; do
		if test -n "$local" ; then
			pid=$(pgrep -o dmeventd) || break;
		fi
		# Check pid and dmeventd readiness to communicate
		test "$pid" = "$local" && dmeventd -i && break
		sleep .2
	done
}

_restart_dmeventd() {
	#rm -f debug.log*

	dmeventd -R -fldddd -e "$PWD/test_nologin" > debug.log_DMEVENTD_$RANDOM 2>&1 &
	local local=$!
	echo "$local" >LOCAL_DMEVENTD

	_wait_for_dmeventd "$local"

	if [ "$i" -eq 50 ]; then
		# Unexpected - we waited over 10 seconds and
		# dmeventd has not managed to restart
		cat /run/dmeventd.pid || true
		pgrep dmeventd || true
		die "dmeventd restart is too slow: $(ps aux)"
	fi
}

aux prepare_dmeventd

aux prepare_vg 5

lvcreate -aey --type mirror -m 3 --nosync --ignoremonitoring -l1 -n 4way $vg
lvchange --monitor y $vg/4way
lvcreate -aey --type mirror -m 2 --nosync --ignoremonitoring -l1 -n 3way $vg
lvchange --monitor y $vg/3way

lvcreate -aey -l1 -n $lv1 $vg
lvcreate -s -l1 -n $lv2 $vg/$lv1

_wait_for_dmeventd
_restart_dmeventd

check lv_field $vg/3way seg_monitor "monitored"
check lv_field $vg/4way seg_monitor "monitored"
lvchange --monitor y --verbose $vg/3way 2>&1 | tee lvchange.out
# only non-cluster tests can check command result
test -e LOCAL_CLVMD || grep 'already monitored' lvchange.out
lvchange --monitor y --verbose $vg/4way 2>&1 | tee lvchange.out
test -e LOCAL_CLVMD || grep 'already monitored' lvchange.out

# now try what happens if no dmeventd is running
pid=$(< LOCAL_DMEVENTD)
kill -9 "$pid"
# TODO/FIXME: it would be surely better, if the wait loop below would
# not be need however ATM the API for communication is not welldetecting
# this highly unusual race case - and things will simply timeout on
# reading failure within 4 seconds.
# Fixing would require to add some handling for losing FIFO connection
for i in {1..10}; do
	# wait here for a while until dmeventd dies....
	# surprisingly it's not instant and we can actually
	# obtain list of monitored devices...
	test -z $(ps -p "$pid" -o comm=) && break
	sleep .1
done
rm LOCAL_DMEVENTD debug.log*

_restart_dmeventd

# now dmeventd should not be running
not pgrep dmeventd
rm LOCAL_DMEVENTD

# First lvs restarts 'dmeventd' (initiate a socket connection to a daemon)
# used explicit 'lvs' avoid using forked 'check' function here as that
# would further for 'get' and actually would be waint till whole process group
# exits - which is not what we want here
#
# FIXME/TODO: lvs should probably not be the way to 'fork dmeventd'
#
lvs --noheadings -o seg_monitor $vg/3way

check lv_field $vg/3way seg_monitor "not monitored"
pgrep -o dmeventd >LOCAL_DMEVENTD
check lv_field $vg/4way seg_monitor "not monitored"

lvchange --monitor y --verbose $vg/3way 2>&1 | tee lvchange.out
test -e LOCAL_CLVMD || not grep 'already monitored' lvchange.out

lvchange --monitor y --verbose $vg/$lv2 2>&1 | tee lvchange.out
test -e LOCAL_CLVMD || not grep 'already monitored' lvchange.out

_restart_dmeventd

kill -INT "$(< LOCAL_DMEVENTD)"
sleep 1

# dmeventd should be still present (although in 'exit-mode')
pgrep -o dmeventd

# Create a file simulating 'shutdown in progress'
touch test_nologin
sleep 2

# Should be now dead (within ~1 second)
not pgrep -o dmeventd
rm -f LOCAL_DMEVENTD

# Do not run dmeventd here again
vgremove -ff --config 'activation/monitoring = 0' $vg
