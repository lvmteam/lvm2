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

test_description='Test system_id'

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux prepare_devs 1

# with clvm enabled, vgcreate with no -c option creates a clustered vg,
# which should have no system id

if [ -e LOCAL_CLVMD ]; then
SID1=sidfoolocal
SID2=""
LVMLOCAL=etc/lvmlocal.conf
rm -f $LVMLOCAL
echo "local {" > $LVMLOCAL
echo "  system_id = $SID1" >> $LVMLOCAL
echo "}" >> $LVMLOCAL
aux lvmconf "global/system_id_source = lvmlocal"
vgcreate $vg1 "$dev1"
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID2
vgremove $vg1
rm -f $LVMLOCAL
exit 0
fi

# create vg with system_id using each source

## none

SID=""
aux lvmconf "global/system_id_source = none"
vgcreate $vg1 "$dev1"
check vg_field $vg1 systemid $SID
vgremove $vg1

# FIXME - print 'life' config data
eval $(lvmconfig global/etc 2>/dev/null || lvmconfig --type default global/etc)

## machineid
if [ -e $etc/machine-id ]; then
SID=$(cat $etc/machine-id)
aux lvmconf "global/system_id_source = machineid"
vgcreate $vg1 "$dev1"
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
vgremove $vg1
fi

## uname

SID1=$(uname -n)
if [ -n $SID1 ]; then
aux lvmconf "global/system_id_source = uname"
SID2=$(lvm systemid | awk '{ print $3 }')
vgcreate $vg1 "$dev1"
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID2
vgremove $vg1
fi

## lvmlocal

SID=sidfoolocal
LVMLOCAL=etc/lvmlocal.conf
rm -f $LVMLOCAL
echo "local {" > $LVMLOCAL
echo "  system_id = $SID" >> $LVMLOCAL
echo "}" >> $LVMLOCAL
aux lvmconf "global/system_id_source = lvmlocal"
vgcreate $vg1 "$dev1"
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
vgremove $vg1
rm -f $LVMLOCAL

## file

SID=sidfoofile
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
vgcreate $vg1 "$dev1"
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
vgremove $vg1
rm -f $SIDFILE

# override system_id to create a foreign vg, then fail to use the vg

SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg, overriding the local system_id so the vg looks foreign
vgcreate --systemid $SID2 $vg1 "$dev1"
# normal vgs is not an error and does not see the vg
vgs >err
not grep $vg1 err
# vgs on the foreign vg is an error and not displayed
not vgs $vg1 >err
not grep $vg1 err
# fail to remove foreign vg
not vgremove $vg1
# using --foreign we can see foreign vg
vgs --foreign >err
grep $vg1 err
vgs --foreign $vg1 >err
grep $vg1 err
# change the local system_id to the second value, making the vg not foreign
echo "$SID2" > $SIDFILE
# we can now see and remove the vg
vgs $vg1 >err
grep $vg1 err
vgremove $vg1
rm -f $SIDFILE

# create a vg, then change the local system_id, making the vg foreign

SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs >err
grep $vg1 err
# change the local system_id, making the vg foreign
echo "$SID2" > $SIDFILE
# normal vgs doesn't see the vg
vgs >err
not grep $vg1 err
# using --foreign we can see the vg
vgs --foreign >err
grep $vg1 err
# change the local system_id back to the first value, making the vg not foreign
echo "$SID1" > $SIDFILE
vgs >err
grep $vg1 err
vgremove $vg1
rm -f $SIDFILE

# create a vg, then change the vg's system_id, making it foreign

SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs >err
grep $vg1 err
# change the vg's system_id, making the vg foreign
echo "y" | vgchange --systemid $SID2 $vg1
# normal vgs doesn't see the vg
vgs >err
not grep $vg1 err
# using --foreign we can see the vg
vgs --foreign >err
grep $vg1 err
# change the local system_id to the second system_id so we can remove the vg
echo "$SID2" > $SIDFILE
vgs >err
grep $vg1 err
vgremove $vg1
rm -f $SIDFILE

# create a vg, create active lvs in it, change our system_id, making
# the VG foreign, verify that we can still see the foreign VG,
# and can deactivate the LVs

SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l 2 $vg1
# normal vgs sees the vg and lv
vgs >err
grep $vg1 err
check lv_exists $vg1 $lv1
# change our system_id, making the vg foreign, but accessible
echo "$SID2" > $SIDFILE
vgs >err
grep $vg1 err
check lv_exists $vg1 $lv1
# can deactivate the lv
lvchange -an $vg1/$lv1
# now that the foreign vg has no active lvs, we can't access it
not lvremove $vg1/$lv1
not vgremove $vg1
# change our system_id back to match the vg so it's not foreign
echo "$SID1" > $SIDFILE
vgs >err
grep $vg1 err
lvremove $vg1/$lv1
vgremove $vg1
rm -f $SIDFILE

# local system has no system_id, so it can't access a vg with a system_id

SID1=sidfoofile1
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
aux lvmconf "global/system_id_source = none"
vgs >err
not grep $vg1 err
not vgs $vg1 >err
not grep $vg1 err
aux lvmconf "global/system_id_source = file"
vgs >err
grep $vg1 err
vgremove $vg1
rm -f $SIDFILE

# local system has a system_id, and can use a vg without a system_id

SID1=sidfoofile1
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
# create a vg with no system_id
aux lvmconf "global/system_id_source = none"
vgcreate $vg1 "$dev1"
check vg_field $vg1 systemid ""
# set a local system_id
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# check we can see and use the vg with no system_id
vgs >err
grep $vg1 err
vgs $vg1 >err
grep $vg1 err
vgremove $vg1
rm -f $SIDFILE

# vgexport clears system_id, vgimport sets system_id

SID1=sidfoofile1
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs -o+systemid >err
grep $vg1 err
grep $SID1 err
# after vgexport there is no systemid
vgexport $vg1
vgs -o+systemid >err
grep $vg1 err
not grep $SID1 err
# after vgimport there is a systemid
vgimport $vg1
vgs -o+systemid >err
grep $vg1 err
grep $SID1 err
vgremove $vg1
rm -f $SIDFILE

# vgchange -cy clears system_id, vgchange -cn sets system_id

SID1=sidfoofile1
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs -o+systemid >err
grep $vg1 err
grep $SID1 err
# after vgchange -cy there is no systemid
echo "y" | vgchange -cy $vg1
vgs --config 'global { locking_type=0 }' -o+systemid $vg1 >err
grep $vg1 err
not grep $SID1 err
# after vgchange -cn there is a systemid
vgchange --config 'global { locking_type=0 }' -cn $vg1
vgs -o+systemid >err
grep $vg1 err
grep $SID1 err
vgremove $vg1
rm -f $SIDFILE

# Test max system_id length (128) and invalid system_id characters.
# The 128 length limit is imposed before invalid characters are omitted.

SIDFILE=etc/lvm_test.conf

# 120 numbers followed by 8 letters (max len)
SID1=012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789abcdefgh
# 120 numbers followed by 9 letters (too long by 1 character, the last is omitted)
SID2=012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789abcdefghi

# max len system_id should appear normally
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs -o+systemid $vg1 >err
grep $vg1 err
grep $SID1 err
vgremove $vg1
rm -f $SIDFILE

# max+1 len system_id should be missing the last character
rm -f $SIDFILE
echo "$SID2" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs -o+systemid $vg1 >err
grep $vg1 err
grep $SID1 err
not grep $SID2 err
vgremove $vg1
rm -f $SIDFILE

# max len system_id containing an invalid character should appear without
# the invalid character
# 120 numbers followed by invalid '%' character followed by 8 letters (too long by 1 character)
SID1=012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789%abcdefgh
# After the invalid character is omitted from SID1
# The string is truncated to max length (128) before the invalid character is omitted
SID2=012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789abcdefg
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs -o+systemid $vg1 >err
grep $vg1 err
not grep $SID1 err
grep $SID2 err
vgremove $vg1
rm -f $SIDFILE

# contains a bunch of invalid characters
SID1="?%$&A.@1]"
# SID1 without the invalid characters
SID2=A.1

rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs -o+systemid $vg1 >err
grep $vg1 err
not grep $SID1 err
grep $SID2 err
vgremove $vg1
rm -f $SIDFILE


# pvs: pv in a foreign vg not reported
# pvs --foreign: pv in a foreign vg is reported

SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal pvs sees the vg and pv
pvs >err
grep $vg1 err
grep "$dev1" err
# change the local system_id, making the vg foreign
echo "$SID2" > $SIDFILE
# normal pvs does not see the vg or pv
pvs >err
not grep $vg1 err
not grep "$dev1" err
# pvs --foreign does see the vg and pv
pvs --foreign >err
grep $vg1 err
grep "$dev1" err
# change the local system_id back so the vg can be removed
echo "$SID1" > $SIDFILE
vgremove $vg1
rm -f $SIDFILE

# lvs: lvs in a foreign vg not reported
# lvs --foreign: lvs in a foreign vg are reported

SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l 2 $vg1
lvchange -an $vg1/$lv1
# normal lvs sees the vg and lv
lvs >err
grep $vg1 err
grep $lv1 err
# change the local system_id, making the vg foreign
echo "$SID2" > $SIDFILE
# normal lvs does not see the vg or lv
lvs >err
not grep $vg1 err
not grep $lv1 err
# lvs --foreign does see the vg and lv
lvs --foreign >err
grep $vg1 err
grep $lv1 err
# change the local system_id back so the vg can be removed
echo "$SID1" > $SIDFILE
lvremove $vg1/$lv1
vgremove $vg1
rm -f $SIDFILE

# use extra_system_ids to read a foreign VG

SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
LVMLOCAL=etc/lvmlocal.conf
rm -f $LVMLOCAL
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs >err
grep $vg1 err
# change the local system_id, making the vg foreign
echo "$SID2" > $SIDFILE
# normal vgs doesn't see the vg
vgs >err
not grep $vg1 err
# using --foreign we can see the vg
vgs --foreign >err
grep $vg1 err
# add the first system_id to extra_system_ids so we can see the vg
echo "local {" > $LVMLOCAL
echo "  extra_system_ids = [ $SID1" ] >> $LVMLOCAL
echo "}" >> $LVMLOCAL
vgs >err
grep $vg1 err
vgremove $vg1
rm -f $SIDFILE
rm -f $LVMLOCAL

# vgcreate --systemid "" creates a vg without a system_id even if source is set
SID1=sidfoofile1
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate --systemid "" $vg1 "$dev1"
# normal vgs sees the vg
vgs >err
grep $vg1 err
# our system_id is not displayed for the vg
vgs -o+systemid >err
not grep $SID1 err
vgremove $vg1
rm -f $SIDFILE

# vgchange --systemid "" clears the system_id on owned vg
SID1=sidfoofile1
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs >err
grep $vg1 err
# the vg has our system_id
vgs -o+systemid >err
grep $SID1 err
# clear the system_id
vgchange --yes --systemid "" $vg1
# normal vgs sees the vg
vgs >err
grep $vg1 err
# the vg does not have our system_id
vgs -o+systemid >err
not grep $SID1 err
vgremove $vg1
rm -f $SIDFILE

# vgchange --systemid does not set the system_id on foreign vg
SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $LVMLOCAL
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs >err
grep $vg1 err
# change the local system_id, making the vg foreign
echo "$SID2" > $SIDFILE
# normal vgs doesn't see the vg
vgs >err
not grep $vg1 err
# using --foreign we can see the vg
vgs --foreign >err
grep $vg1 err
# cannot clear the system_id of the foreign vg
not vgchange --yes --systemid "" $vg1
# cannot set the system_id of the foreign vg
not vgchange --yes --systemid foo $vg1
# change our system_id back so we can remove the vg
echo "$SID1" > $SIDFILE
vgremove $vg1
rm -f $SIDFILE

# vgcfgbackup backs up foreign vg with --foreign
SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $LVMLOCAL
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg
vgcreate $vg1 "$dev1"
# normal vgs sees the vg
vgs >err
grep $vg1 err
# change the local system_id, making the vg foreign
echo "$SID2" > $SIDFILE
# normal vgs doesn't see the vg
vgs >err
not grep $vg1 err
# using --foreign we can back up the vg
not vgcfgbackup $vg1
vgcfgbackup --foreign $vg1
# change our system_id back so we can remove the vg
echo "$SID1" > $SIDFILE
vgremove $vg1
rm -f $SIDFILE



# Test handling of bad system_id source configurations
# The commands should proceed without a system_id.
# Look at the warning/error messages.

# vgcreate with source machineid, where no $etc/machine-id file exists
if [ ! -e $etc/machine-id ]; then
SID=""
aux lvmconf "global/system_id_source = machineid"
vgcreate $vg1 "$dev1" 2>&1 | tee err
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
grep "No system ID found from system_id_source" err
vgremove $vg1
fi

# vgcreate with source uname, but uname is localhost
# TODO: don't want to change the hostname on the test machine...

# vgcreate with source lvmlocal, but no lvmlocal.conf file
SID=""
rm -f $LVMLOCAL
aux lvmconf "global/system_id_source = lvmlocal"
vgcreate $vg1 "$dev1" 2>&1 | tee err
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
grep "No system ID found from system_id_source" err
vgremove $vg1
rm -f $LVMLOCAL

# vgcreate with source lvmlocal, but no system_id = "x" entry
SID=""
LVMLOCAL=etc/lvmlocal.conf
rm -f $LVMLOCAL
echo "local {" > $LVMLOCAL
# echo "  system_id = $SID" >> $LVMLOCAL
echo "}" >> $LVMLOCAL
aux lvmconf "global/system_id_source = lvmlocal"
vgcreate $vg1 "$dev1" 2>&1 | tee err
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
grep "No system ID found from system_id_source" err
vgremove $vg1
rm -f $LVMLOCAL

# vgcreate with source lvmlocal, and empty string system_id = ""
SID=""
LVMLOCAL=etc/lvmlocal.conf
rm -f $LVMLOCAL
echo "local {" > $LVMLOCAL
echo "  system_id = \"\"" >> $LVMLOCAL
echo "}" >> $LVMLOCAL
aux lvmconf "global/system_id_source = lvmlocal"
vgcreate $vg1 "$dev1" 2>&1 | tee err
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
grep "No system ID found from system_id_source" err
vgremove $vg1
rm -f $LVMLOCAL

# vgcreate with source file, but no system_id_file config
SID=""
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
aux lvmconf "global/system_id_source = file"
vgcreate $vg1 "$dev1" 2>&1 | tee err
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
grep "No system ID found from system_id_source" err
vgremove $vg1
rm -f $SIDFILE

# vgcreate with source file, but system_id_file does not exist
SID=""
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
vgcreate $vg1 "$dev1" 2>&1 | tee err
vgs -o+systemid $vg1
check vg_field $vg1 systemid $SID
grep "No system ID found from system_id_source" err
vgremove $vg1
rm -f $SIDFILE


# Test cases where lvmetad cache of a foreign VG are out of date
# because the foreign owner has changed the VG.

test ! -e LOCAL_LVMETAD && exit 0

# When a foreign vg is newer on disk than in lvmetad, using --foreign
# should find the newer version.  This simulates a foreign host changing
# foreign vg by turning off lvmetad when we create an lv in the vg.
SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg with an lv
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l 2 -an $vg1
# normal vgs sees the vg and lv
vgs >err
grep $vg1 err
check lv_exists $vg1 $lv1
# go around lvmetad to create another lv in the vg,
# forcing the lvmetad copy to be older than on disk.
aux lvmconf 'global/use_lvmetad = 0'
lvcreate -n $lv2 -l 2 -an $vg1
aux lvmconf 'global/use_lvmetad = 1'
# verify that the second lv is not in lvmetad
lvs $vg1 >err
grep $lv1 err
not grep $lv2 err
# change our system_id, making the vg foreign
echo "$SID2" > $SIDFILE
vgs >err
not grep $vg1 err
# using --foreign, we will get the latest vg from disk
lvs --foreign $vg1 >err
grep $vg1 err
grep $lv1 err
grep $lv2 err
# change our system_id back to match the vg so it's not foreign
echo "$SID1" > $SIDFILE
lvremove $vg1/$lv1
lvremove $vg1/$lv2
vgremove $vg1
rm -f $SIDFILE

# vgimport should find the exported vg on disk even though
# lvmetad's copy of the vg shows it's foreign.
SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg with an lv
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l 2 -an $vg1
# normal vgs sees the vg and lv
vgs >err
grep $vg1 err
check lv_exists $vg1 $lv1
# go around lvmetad to export the vg so that lvmetad still
# has the original vg owned by SID1 in its cache
aux lvmconf 'global/use_lvmetad = 0'
vgexport $vg1
aux lvmconf 'global/use_lvmetad = 1'
# change the local system_id so the lvmetad copy of the vg is foreign
echo "$SID2" > $SIDFILE
# verify that lvmetad thinks the vg is foreign
# (don't use --foreign to verify this because that will cause
# the lvmetad cache to be updated, which we don't want yet)
not vgs $vg1
# attempt to import the vg that has been exported, but
# which lvmetad thinks is foreign
vgimport $vg1
# verify that the imported vg has our system_id
vgs -o+systemid $vg1 >err
grep $vg1 err
grep $SID2 err
check lv_exists $vg1 $lv1
lvremove $vg1/$lv1
vgremove $vg1
rm -f $SIDFILE

# pvscan --cache should cause the latest version of a foreign VG to be
# cached in lvmetad.  Without the --cache option, pvscan will see the old
# version of the VG.
SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg with an lv
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l 2 -an $vg1
# normal vgs sees the vg and lv
vgs >err
grep $vg1 err
check lv_exists $vg1 $lv1
# go around lvmetad to create another lv in the vg,
# forcing the lvmetad copy to be older than on disk.
aux lvmconf 'global/use_lvmetad = 0'
lvcreate -n $lv2 -l 2 -an $vg1
aux lvmconf 'global/use_lvmetad = 1'
# verify that the second lv is not in lvmetad
lvs $vg1 >err
grep $lv1 err
not grep $lv2 err
# verify that after pvscan without --cache, lvmetad still
# reports the old version
pvscan
lvs $vg1 >err
grep $lv1 err
not grep $lv2 err
# change our system_id, making the vg foreign
echo "$SID2" > $SIDFILE
not vgs $vg1 >err
not grep $vg1 err
# use pvscan --cache to update the foreign vg in lvmetad
pvscan --cache
not vgs $vg1 >err
not grep $vg1 err
# change our system_id back to SID1 so we can check that
# lvmetad has the latest copy of the vg (without having
# to use --foreign to check)
echo "$SID1" > $SIDFILE
vgs $vg1 >err
grep $vg1 err
lvs $vg1 >err
grep $lv1 err
grep $lv2 err
lvremove $vg1/$lv1
lvremove $vg1/$lv2
vgremove $vg1
rm -f $SIDFILE

# repeat the same test for vgscan instead of pvscan
SID1=sidfoofile1
SID2=sidfoofile2
SIDFILE=etc/lvm_test.conf
rm -f $SIDFILE
echo "$SID1" > $SIDFILE
aux lvmconf "global/system_id_source = file" \
	    "global/system_id_file = \"$SIDFILE\""
# create a vg with an lv
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l 2 -an $vg1
# normal vgs sees the vg and lv
vgs >err
grep $vg1 err
check lv_exists $vg1 $lv1
# go around lvmetad to create another lv in the vg,
# forcing the lvmetad copy to be older than on disk.
aux lvmconf 'global/use_lvmetad = 0'
lvcreate -n $lv2 -l 2 -an $vg1
aux lvmconf 'global/use_lvmetad = 1'
# verify that the second lv is not in lvmetad
lvs $vg1 >err
grep $lv1 err
not grep $lv2 err
# verify that after vgscan without --cache, lvmetad still
# reports the old version
vgscan
lvs $vg1 >err
grep $lv1 err
not grep $lv2 err
# change our system_id, making the vg foreign
echo "$SID2" > $SIDFILE
not vgs $vg1 >err
not grep $vg1 err
# use vgscan --cache to update the foreign vg in lvmetad
vgscan --cache
not vgs $vg1 >err
not grep $vg1 err
# change our system_id back to SID1 so we can check that
# lvmetad has the latest copy of the vg (without having
# to use --foreign to check)
echo "$SID1" > $SIDFILE
vgs $vg1 >err
grep $vg1 err
lvs $vg1 >err
grep $lv1 err
grep $lv2 err
lvremove $vg1/$lv1
lvremove $vg1/$lv2
vgremove $vg1
rm -f $SIDFILE


