#!/usr/bin/env bash

# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
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

aux prepare_devs 2

pvcreate "$dev1"
pvcreate "$dev2"
vgcreate $vg1 "$dev1" "$dev2"
lvcreate -l1 -an -n $lv1 $vg1 "$dev1"
lvcreate -l1 -an -n $lv2 $vg1 "$dev2"

aux disable_dev "$dev2"

kill "$(< LOCAL_LVMETAD)"
for i in {200..0} ; do
        test -e "$TESTDIR/lvmetad.socket" || break
        test "$i" -eq 0 && die "Too slow closing of lvmetad.socket. Aborting test."
        echo -n .; sleep .1;
done # wait for the socket close
test ! -e "$LVM_LVMETAD_PIDFILE"

lvmetad

while ! test -e "$TESTDIR/lvmetad.socket"; do echo -n .; sleep .1; done # wait for the socket

test -e "$LVM_LVMETAD_PIDFILE"

cp "$LVM_LVMETAD_PIDFILE" LOCAL_LVMETAD

pvscan --cache -aay "$dev1"

check lv_field $vg1/$lv1 lv_active ""
check lv_field $vg1/$lv2 lv_active ""

aux enable_dev "$dev2"

pvscan --cache -aay "$dev2"

check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg1/$lv2 lv_active "active"

vgchange -an $vg1
vgremove -ff $vg1
