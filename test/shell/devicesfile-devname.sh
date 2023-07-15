#!/usr/bin/env bash

# Copyright (C) 2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='devices file with devnames'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 7

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
PVS_ONLINE_DIR="$RUNDIR/lvm/pvs_online"
VGS_ONLINE_DIR="$RUNDIR/lvm/vgs_online"
PVS_LOOKUP_DIR="$RUNDIR/lvm/pvs_lookup"

_clear_online_files() {
        # wait till udev is finished
        aux udev_wait
        rm -f "$PVS_ONLINE_DIR"/*
        rm -f "$VGS_ONLINE_DIR"/*
        rm -f "$PVS_LOOKUP_DIR"/*
}

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir "$DFDIR" || true
DF="$DFDIR/system.devices"
ORIG="$DFDIR/orig.devices"

aux lvmconf 'devices/use_devicesfile = 1'

pvcreate "$dev1"
ls "$DF"
grep "$dev1" "$DF"

pvcreate "$dev2"
grep "$dev2" "$DF"

pvcreate "$dev3"
grep "$dev3" "$DF"

vgcreate $vg1 "$dev1" "$dev2"

# PVID with dashes for matching pvs -o+uuid output
OPVID1=`pvs "$dev1" --noheading -o uuid | awk '{print $1}'`
OPVID2=`pvs "$dev2" --noheading -o uuid | awk '{print $1}'`
OPVID3=`pvs "$dev3" --noheading -o uuid | awk '{print $1}'`

# PVID without dashes for matching devices file fields
PVID1=`pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID2=`pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID3=`pvs "$dev3" --noheading -o uuid | tr -d - | awk '{print $1}'`

lvmdevices --deldev "$dev3"

not grep "$dev3" "$DF"
not grep "$PVID3" "$DF"
not pvs "$dev3"

cp "$DF" "$ORIG"

lvcreate -l4 -an -i2 -n $lv1 $vg1

#
# when wrong idname devname is outside DF it's corrected if search_for=1
# by a general cmd, or by lvmdevices --addpvid
#
# when wrong idname devname is outside DF it's not found or corrected if
# search_for=0 by a general cmd, but will be by lvmdevices --addpvid
#
# when wrong idname devname is inside DF it's corrected if search_for=0|1
# by a general cmd, or by lvmdevices --addpvid
# 
# pvscan --cache -aay does not update DF when devname= is wrong
#
# pvscan --cache -aay when idname devname is wrong:
# every dev is read and then skipped if pvid is not in DF
#
# commands still work with incorrect devname=
# . and they automatically correct the devname=
#


#
# idname changes to become incorrect, devname remains unchanged and correct
# . change idname to something outside DF
# . change idname to match another DF entry
# . swap idname of two DF entries
#

# edit DF idname, s/dev1/dev3/, where new dev is not in DF

sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" "$ORIG" > "$DF"
cat "$DF"
# pvs reports correct info 
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
not grep "$OPVID3" out
not grep "$dev3" out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
# pvs fixed the DF
not grep "$PVID3" "$DF"
not grep "$dev3" "$DF"
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
cat "$DF"

sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" "$ORIG" > "$DF"
cat "$DF"
# lvcreate uses correct dev
lvcreate -l1 -n $lv2 -an $vg1 "$dev1"
# lvcreate fixed the DF
not grep "$PVID3" "$DF"
not grep "$dev3" "$DF"
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
# pvs reports correct dev
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
not grep "$OPVID3" out
not grep "$dev3" out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
lvremove $vg1/$lv2
cat "$DF"

sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" "$ORIG" > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
not grep "$PVID3" "$DF"
not grep "$dev3" "$DF"
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
cat "$DF"

# edit DF idname, s/dev1/dev2/, creating two entries with same idname

sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" "$ORIG" > "$DF"
cat "$DF"
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
grep "$OPVID2" out |tee out2
grep "$dev2" out2
# pvs fixed the DF
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "IDNAME=$dev2" out
cat "$DF"

sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" "$ORIG" > "$DF"
cat "$DF"
# lvcreate uses correct dev
lvcreate -l1 -n $lv2 -an $vg1 "$dev1"
# lvcreate fixed the DF
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "IDNAME=$dev2" out
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
grep "$OPVID2" out |tee out2
grep "$dev2" out2
lvremove $vg1/$lv2
cat "$DF"

sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" "$ORIG" > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "IDNAME=$dev2" out
cat "$DF"

# edit DF idname, swap dev1 and dev2

sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" "$ORIG" > tmp1.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp2.devices > "$DF"
cat "$DF"
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
grep "$OPVID2" out |tee out2
grep "$dev2" out2
# pvs fixed the DF
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "IDNAME=$dev2" out
cat "$DF"

sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" "$ORIG" > tmp1.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp2.devices > "$DF"
cat "$DF"
# lvcreate uses correct dev
lvcreate -l1 -n $lv2 -an $vg1 "$dev1"
# lvcreate fixed the DF
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "IDNAME=$dev2" out
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
grep "$OPVID2" out |tee out2
grep "$dev2" out2
lvremove $vg1/$lv2
cat "$DF"

sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" "$ORIG" > tmp1.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp2.devices > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "IDNAME=$dev2" out
cat "$DF"


#
# idname remains correct, devname changes to become incorrect
# . change devname to something outside DF
# . change devname to match another DF entry
# . swap devname of two DF entries
#

# edit DF devname, s/dev1/dev3/, where new dev is not in DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" "$ORIG" > "$DF"
cat "$DF"
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
not grep "$OPVID3" out
not grep "$dev3" out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
# pvs fixed the DF
not grep "$PVID3" "$DF"
not grep "$dev3" "$DF"
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
cat "$DF"

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" "$ORIG" > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
not grep "$PVID3" "$DF"
not grep "$dev3" "$DF"
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
cat "$DF"

# edit DF devname, s/dev1/dev2/, creating two entries with same devname

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" "$ORIG" > "$DF"
cat "$DF"
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
grep "$OPVID2" out |tee out2
grep "$dev2" out2
# pvs fixed the DF
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "DEVNAME=$dev2" out
cat "$DF"

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" "$ORIG" > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "IDNAME=$dev2" out
cat "$DF"

# edit DF devname, swap dev1 and dev2

sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" "$ORIG" > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > "$DF"
cat "$DF"
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
grep "$OPVID2" out |tee out2
grep "$dev2" out2
# pvs fixed the DF
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "DEVNAME=$dev2" out
cat "$DF"

sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" "$ORIG" > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
grep "$PVID1" "$DF" |tee out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "IDNAME=$dev2" out
cat "$DF"


#
# idname and devname change, both become incorrect
# . change idname&devname to something outside DF
# . change idname&devname to match another DF entry
# . swap idname&devname of two DF entries
#

# edit DF idname&devname, s/dev1/dev3/, where new dev is not in DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" "$ORIG" > tmp1.devices
sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" tmp1.devices > "$DF"
cat "$DF"
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
not grep "$OPVID3" out
not grep "$dev3" out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
# pvs fixed the DF
not grep "$PVID3" "$DF"
not grep "$dev3" "$DF"
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "IDNAME=$dev1" out
cat "$DF"

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" "$ORIG" > tmp1.devices
sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" tmp1.devices > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
not grep "$PVID3" "$DF"
not grep "$dev3" "$DF"
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "IDNAME=$dev1" out
cat "$DF"

# edit DF idname&devname, s/dev1/dev2/, creating two entries with same devname

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" tmp1.devices > "$DF"
sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" tmp1.devices > "$DF"
cat "$DF"
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
grep "$OPVID2" out |tee out2
grep "$dev2" out2
# pvs fixed the DF
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "DEVNAME=$dev2" out
grep "IDNAME=$dev2" out
cat "$DF"

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" tmp1.devices > "$DF"
sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" tmp1.devices > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "DEVNAME=$dev2" out
grep "IDNAME=$dev2" out
cat "$DF"

# edit DF devname, swap dev1 and dev2

sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" "$ORIG" > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > tmp3.devices
sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" tmp3.devices > tmp4.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp4.devices > tmp5.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp5.devices > "$DF"
cat "$DF"
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep "$OPVID1" out |tee out2
grep "$dev1" out2
grep "$OPVID2" out |tee out2
grep "$dev2" out2
# pvs fixed the DF
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "DEVNAME=$dev2" out
grep "IDNAME=$dev2" out
cat "$DF"

sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" "$ORIG" > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > tmp3.devices
sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" tmp3.devices > tmp4.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp4.devices > tmp5.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp5.devices > "$DF"
cat "$DF"
# lvmdevices fixes the DF
lvmdevices --update
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "IDNAME=$dev1" out
grep "$PVID2" "$DF" |tee out
grep "DEVNAME=$dev2" out
grep "IDNAME=$dev2" out
cat "$DF"

#
# check that pvscan --cache -aay does the right thing:
#
# idname and devname change, both become incorrect
# . change idname&devname to something outside DF
# . swap idname&devname of two DF entries
#

# edit DF idname&devname, s/dev1/dev3/, where new dev is not in DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" "$ORIG" > tmp1.devices
sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" tmp1.devices > "$DF"
cat "$DF"
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"
pvscan --cache -aay "$dev3"
cat "$DF"
# pvscan does not fix DF
grep "$dev3" "$DF"
not grep "$dev1" "$DF"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"
check lv_field $vg1/$lv1 lv_active "active"
# pvs updates the DF
pvs |tee out
grep "$dev1" out
grep "$dev2" out
not grep "$dev3" out
grep "$dev1" "$DF"
grep "$dev2" "$DF"
not grep "$dev3" "$DF"
vgchange -an $vg1

# edit DF idname&devname, swap dev1 and dev2

vgremove -y $vg1
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
vgcreate $vg2 "$dev2"
lvcreate -n $lv2 -l1 -an $vg2

cat "$DF"
sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" "$ORIG" > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > tmp3.devices
sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" tmp3.devices > tmp4.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp4.devices > tmp5.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp5.devices > "$DF"
cat "$DF"

_clear_online_files

# pvscan creates the correct online files and activates correct vg
pvscan --cache -aay "$dev1"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/vgs_online/$vg1"
not ls "$RUNDIR/lvm/pvs_online/$PVID2"
not ls "$RUNDIR/lvm/vgs_online/$vg2"
# don't use lvs because it would fix DF before we check it
dmsetup status $vg1-$lv1
not dmsetup status $vg2-$lv2

pvscan --cache -aay "$dev2"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/vgs_online/$vg2"
dmsetup status $vg2-$lv2

pvscan --cache -aay "$dev3"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"

# pvscan did not correct DF
cat "$DF"
grep "$PVID1" "$DF" |tee out
grep "$dev2" out
not grep "$dev1" out
grep "$PVID2" "$DF" |tee out
grep "$dev1" out
not grep "$dev2" out

# pvs corrects DF
pvs
grep "$PVID1" "$DF" |tee out
grep "$dev1" out
not grep "$dev2" out
grep "$PVID2" "$DF" |tee out
grep "$dev2" out
not grep "$dev1" out

vgchange -an $vg1
vgchange -an $vg2
vgremove -ff $vg1
vgremove -ff $vg2

# bz 2119473

aux lvmconf "devices/search_for_devnames = \"none\""
sed -e "s|DEVNAME=$dev1|DEVNAME=.|" "$ORIG" > tmp1.devices
sed -e "s|IDNAME=$dev1|IDNAME=.|" tmp1.devices > "$DF"
pvs
lvmdevices
pvcreate -ff --yes --uuid "$PVID1" --norestorefile $dev1
grep "$PVID1" "$DF" |tee out
grep "DEVNAME=$dev1" out
grep "IDNAME=$dev1" out
aux lvmconf "devices/search_for_devnames = \"auto\""

# devnames change so the new devname now refers to a filtered device,
# e.g. an mpath or md component, which is not scanned

wait_md_create() {
        local md=$1

        while :; do
                if ! grep "$(basename $md)" /proc/mdstat; then
                        echo "$md not ready"
                        cat /proc/mdstat
                        sleep 2
                else
                        break
                fi
        done
        echo "$md" > WAIT_MD_DEV
}

aux wipefs_a "$dev1" "$dev2" "$dev3" "$dev4"

rm "$DF"
touch "$DF"
vgcreate $vg1 "$dev1" "$dev2"
cat "$DF"
cp "$DF" "$ORIG"

# PVID with dashes for matching pvs -o+uuid output
OPVID1=`pvs "$dev1" --noheading -o uuid | awk '{print $1}'`
OPVID2=`pvs "$dev2" --noheading -o uuid | awk '{print $1}'`

# PVID without dashes for matching devices file fields
PVID1=`pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID2=`pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}'`

aux mdadm_create --metadata=1.0 --level 1 --raid-devices=2 "$dev3" "$dev4"
mddev=$(< MD_DEV)

wait_md_create "$mddev"

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" "$ORIG" > tmp1.devices
sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" tmp1.devices > "$DF"
cat "$DF"
pvs -o+uuid |tee out
grep "$dev1" out
grep "$dev2" out
grep "$OPVID1" out
grep "$OPVID2" out
not grep "$dev3" out
not grep "$dev4" out

grep "$dev1" "$DF"
grep "$dev2" "$DF"
grep "$PVID1" "$DF"
grep "$PVID2" "$DF"
not grep "$dev3" "$DF"
not grep "$dev4" "$DF"

mdadm --stop "$mddev"
aux udev_wait

vgremove -ff $vg1
