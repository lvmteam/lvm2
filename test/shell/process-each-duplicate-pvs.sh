#!/bin/sh
# Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.

test_description='Test duplicate PVs'

. lib/inittest

aux prepare_devs 2

pvcreate "$dev1"
vgcreate $vg1 "$dev1"

# Clone the PV
dd if="$dev1" of="$dev2" bs=256K count=1 iflag=direct oflag=direct
aux notify_lvmetad "$dev2"

# When there are cloned devices (same pvid), one will be referenced in
# lvmcache as pv->dev, and the other will not be referenced from lvmcache,
# it'll only be in device cache.  The one referenced by lvmcache is
# referred to as the "preferred" one, and is the one that is printed by a
# standard 'pvs' command.
#
# We don't know if dev1 or dev2 will be preferred, so we first check that
# and save it as PV1, the other as PV2.
#
# The rules that 'pvs' follows to choose which PVs to display are
# somewhat strange and seem arbitrary from a user perspective;
# the choice is driven largely by what's most practical in the code,
# but also by what vgimportclone needs.
#
# Some of the rules that process_each_pv is using:
# - When no pv arg is specified, print the one preferred dev.
# - When pv args are specified, print one line per specified arg,
#   i.e. don't print all duplicate pvs when one is specified.
# - Always print the preferred duplicate, even if it was not the
#   one specified, e.g. If there are two duplicates on A and B,
#   and A is the preferred device, then 'pvs A' will show A and
#   'pvs B' will show A.
# - If multiple duplicates are specified, then print each, e.g.
#   'pvs A B' will show both A and B.
# - If three duplicates exist on A, B, C, and the preferred is A,
#   and the command 'pvs B C' is run, then the A will be printed
#   first since we always print the preferred device, and then
#   either B or C will be printed.  'pvs A B C' will print all.
# - 'pvs -a' should print all the duplicates and should show
#   the same VG for each.
# - 'pvs -o+size ...' should show the correct size of the
#   devices being printed if they differ among the duplicates.
# - By using 'pvs --config' with a filter, you can filter out
#   the duplicate devs you don't want so that pvs will
#   print the devs you do want to see.
#
# The tests below check these behaviors on up to two duplicates,
# so if the process_each_pv logic changes regarding which
# duplicates are chosen, then this test will need adjusting.

# Verify that there is only one PV printed, i.e. the preferred
pvs --noheading | tee out
test $(wc -l < out) -eq 1

# Set PV1 to the perferred/cached PV, and PV2 to the other.
# Cannot use pvs -o pv_name because that command goes to
# disk and does not represent what lvmetad thinks.
PV1=$(pvs --noheading | awk '{ print $1 }')
echo PV1 is $PV1
if [ $PV1 == $dev1 ]; then
	PV2=$dev2
else
	PV2=$dev1
fi
echo PV2 is $PV2

# check listed pvs
pvs --noheading | tee out
grep $PV1 out
not grep $PV2 out

# check error messages
pvs --noheading 2>&1 | tee out
grep "Found duplicate" out >err
grep "using $PV1 not $PV2" err

# check listed pvs
pvs --noheading "$dev1" | tee out
grep $PV1 out
not grep $PV2 out

# check error messages
pvs --noheading "$dev1" 2>&1 | tee out
grep "Found duplicate" out >err
grep "using $PV1 not $PV2" err

# check listed pvs
pvs --noheading "$dev2" | tee out
grep $PV1 out
not grep $PV2 out

# check error messages
pvs --noheading "$dev2" 2>&1 | tee out
grep "Found duplicate" out >err
grep "using $PV1 not $PV2" err

# check listed pvs
pvs --noheading "$dev1" "$dev2" | tee out
grep $PV1 out
grep $PV2 out

# check error messages
pvs --noheading "$dev1" "$dev2" 2>&1 | tee out
grep "Found duplicate" out >err
grep "using $PV1 not $PV2" err

# check listed pvs
pvs --noheading -a | tee out
grep $PV1 out
grep $PV2 out
grep $PV1 out | grep $vg1
grep $PV2 out | grep $vg1

# check error messages
pvs --noheading -a 2>&1 | tee out
grep "Found duplicate" out >err
grep "using $PV1 not $PV2" err


# TODO: I'd like to test that a subsystem device is preferred
# over a non-subsystem device, but all the devices used here
# are DM devices, i.e. they are already subsystem devices,
# so I can't just wrap a standard block device with a DM
# identity mapping.

