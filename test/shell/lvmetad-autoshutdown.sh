#!/usr/bin/env bash

# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITHOUT_LVMETAD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

kill -0 "$(< LOCAL_LVMETAD)" || die "lvmetad is already dead"

lvmetad_timeout=3

aux prepare_pvs 1

vgcreate $vg1 "$dev1"

kill "$(< LOCAL_LVMETAD)"
aux prepare_lvmetad -t $lvmetad_timeout

sleep $lvmetad_timeout

# lvmetad should die after timeout, but give it some time to do so
i=0
while kill -0 "$(< LOCAL_LVMETAD)" 2>/dev/null; do
	test $i -ge $((lvmetad_timeout*10)) && die "lvmetad didn't shutdown with optional timeout: $lvmetad_timeout seconds"
	sleep .1
	i=$((i+1))
done

aux prepare_lvmetad -t 0
sleep 1
# lvmetad must not die with -t 0 option
kill -0 "$(< LOCAL_LVMETAD)" || die "lvmetad died"

kill "$(< LOCAL_LVMETAD)"
aux prepare_lvmetad -t $lvmetad_timeout

sleep 1
vgs
sleep 1
vgs
sleep 1
vgs

# check that connection to lvmetad resets the timeout
kill -0 "$(< LOCAL_LVMETAD)" || die "lvmetad died too soon"

vgremove -ff $vg1
