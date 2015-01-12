#!/bin/sh
# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/inittest

test -e LOCAL_LVMETAD || skip
which mdadm || skip

test -f /proc/mdstat && grep -q raid0 /proc/mdstat || \
	modprobe raid0 || skip

aux lvmconf 'devices/md_component_detection = 1'
aux extend_filter_LVMTEST
aux extend_filter "a|/dev/md.*|"

aux prepare_devs 2

# TODO factor out the following MD-creation code into lib/

# Have MD use a non-standard name to avoid colliding with an existing MD device
# - mdadm >= 3.0 requires that non-standard device names be in /dev/md/
# - newer mdadm _completely_ defers to udev to create the associated device node
mdadm_maj=$(mdadm --version 2>&1 | perl -pi -e 's|.* v(\d+).*|\1|')
[ $mdadm_maj -ge 3 ] && \
    mddev=/dev/md/md_lvm_test0 || \
    mddev=/dev/md_lvm_test0

cleanup_md() {
    # sleeps offer hack to defeat: 'md: md127 still in use'
    # see: https://bugzilla.redhat.com/show_bug.cgi?id=509908#c25
    aux udev_wait
    mdadm --stop "$mddev" || true
    aux udev_wait
    if [ -b "$mddev" ]; then
        # mdadm doesn't always cleanup the device node
	sleep 2
	rm -f "$mddev"
    fi
}

cleanup_md_and_teardown() {
    cleanup_md
    aux teardown
}

# create 2 disk MD raid0 array (stripe_width=128K)
test -b "$mddev" && skip
mdadm --create --metadata=1.0 "$mddev" --auto=md --level 0 --raid-devices=2 --chunk 64 "$dev1" "$dev2"
trap 'cleanup_md_and_teardown' EXIT # cleanup this MD device at the end of the test
test -b "$mddev" || skip
cp -LR "$mddev" "$DM_DEV_DIR" # so that LVM/DM can see the device
lvmdev="$DM_DEV_DIR/md_lvm_test0"

# TODO end MD-creation code

# maj=$(($(stat -L --printf=0x%t "$dev2")))
# min=$(($(stat -L --printf=0x%T "$dev2")))

pvcreate $lvmdev

pvscan --cache "$lvmdev"

# ensure that lvmetad can only see the toplevel MD device
pvscan --cache "$dev1" 2>&1 | grep "not found"
pvscan --cache "$dev2" 2>&1 | grep "not found"

pvs | grep $lvmdev
pvs | not grep "$dev1"
pvs | not grep "$dev2"
