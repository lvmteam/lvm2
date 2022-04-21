#!/usr/bin/env bash

# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='using multipath blacklist'

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1

. lib/inittest

# FIXME: don't run this test by default because it destroys the
# local multipath config, the timing of multipath/dm/lvm interactions
# is fragile, and there's insufficient cleanup after a test fails.
skip

systemctl stop multipathd
multipath -F || true
rm /etc/multipath/wwids || true
rmmod scsi_debug || true
rm /etc/multipath/conf.d/lvmtest.conf || true

modprobe --dry-run scsi_debug || skip
multipath -l || skip
multipath -l | grep scsi_debug && skip
ls /etc/multipath/wwids && skip

# Need to use /dev/mapper/mpath
aux lvmconf 'devices/dir = "/dev"'
aux lvmconf 'devices/scan = "/dev"'
# Could set filter to $MP and the component /dev/sd devs
aux lvmconf "devices/filter = [ \"a|.*|\" ]"
aux lvmconf "devices/global_filter = [ \"a|.*|\" ]"

modprobe scsi_debug dev_size_mb=16 num_tgts=1
sleep 2

# Get scsi device name created by scsi_debug.
# SD = sdh
# SD_DEV = /dev/sdh

SD=$(grep -H scsi_debug /sys/block/sd*/device/model | cut -f4 -d /);
echo $SD
SD_DEV=/dev/$SD
echo $SD_DEV

# if multipath claimed SD, then io will fail
#dd if=$SD_DEV of=/dev/null bs=4k count=1 iflag=direct
#dd if=/dev/zero of=$SD_DEV bs=4k count=1 oflag=direct

# check if multipathd claimed the scsi dev when it appears and create mp dm device
sleep 2
multipath -l
# create the mp dm device
multipath $SD_DEV

# Get mpath device name created by multipath.
# MP = mpatha
# MP_DEV = /dev/maper/mpatha

MP=$(multipath -l | grep scsi_debug | cut -f1 -d ' ')
echo $MP
MP_DEV=/dev/mapper/$MP
echo $MP_DEV

dd if=$MP_DEV of=/dev/null bs=4k count=1 iflag=direct
dd if=/dev/zero of=$MP_DEV bs=4k count=1 oflag=direct

# Get wwid for the mp and sd dev.
WWID=$(multipath -l $MP_DEV | head -1 | awk '{print $2}' | tr -d ')' | tr -d '(')
echo $WWID

grep $WWID /etc/multipath/wwids

pvcreate $MP_DEV
vgcreate $vg1 $MP_DEV

not pvs $SD_DEV
pvs $MP_DEV

# remove mpath dm device then check that SD_DEV is
# filtered based on /etc/multipath/wwids instead of
# based on sysfs holder
multipath -f $MP
sleep 2
not pvs $SD_DEV
multipath $SD_DEV
sleep 2
multipath -l | grep $SD

#
# Add the wwid to the blacklist, then restart multipath
# so the sd dev should no longer be used by multipath,
# but the sd dev wwid is still in /etc/multipath/wwids.
#

mkdir /etc/multipath/conf.d/ || true
rm -f /etc/multipath/conf.d/lvmtest.conf

cat <<EOF > "/etc/multipath/conf.d/lvmtest.conf"
blacklist {
	wwid $WWID
}
EOF

cat /etc/multipath/conf.d/lvmtest.conf

multipath -r
sleep 2

grep $WWID /etc/multipath/wwids

multipath -l |tee out
not grep $SD out
not grep $MP out
not grep $WWID out

not pvs $MP_DEV
pvs $SD_DEV
vgs $vg1

#
# Add the wwid to the blacklist_exceptions, in addition
# to the blacklist, then restart multipath so the
# sd dev should again be used by multipath.
#

rm -f /etc/multipath/conf.d/lvmtest.conf

cat <<EOF > "/etc/multipath/conf.d/lvmtest.conf"
blacklist {
wwid $WWID
}
blacklist_exceptions {
wwid $WWID
}
EOF

cat /etc/multipath/conf.d/lvmtest.conf

multipath -r
sleep 2

grep $WWID /etc/multipath/wwids

multipath -l |tee out
grep $SD out
grep $MP out
grep $WWID out

pvs $MP_DEV
not pvs $SD_DEV
vgs $vg1
lvs $vg1

sleep 2
vgremove -ff $vg1
sleep 2
multipath -f $MP
rm /etc/multipath/conf.d/lvmtest.conf
rm /etc/multipath/wwids
sleep 1
rmmod scsi_debug
