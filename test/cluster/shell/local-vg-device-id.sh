#!/bin/bash
set -e

DF="/etc/lvm/devices/system.devices"
DFDIR="/etc/lvm/devices"

wipe_all() {
    node1 wipefs -a $d1
    node1 wipefs -a $d2
    [ -n "${d3:-}" ] && node1 wipefs -a $d3
    return 0
}

DEV_TYPES=""
[ "${CLUSTER_NUM_SCSI:-0}" -ge 2 ]     && DEV_TYPES="$DEV_TYPES scsi"
[ "${CLUSTER_NUM_NVME:-0}" -ge 2 ]     && DEV_TYPES="$DEV_TYPES nvme"
[ "${CLUSTER_NUM_MULTIPATH:-0}" -ge 2 ] && DEV_TYPES="$DEV_TYPES mpath"
[ -z "$DEV_TYPES" ] && { echo "SKIP: no device type with >= 2 devices"; exit 0; }

for devtype in $DEV_TYPES; do
    case $devtype in
        scsi)  d1=$scsi1; d2=$scsi2; d3=${scsi3:-} ;;
        nvme)  d1=$nvme1; d2=$nvme2; d3=${nvme3:-} ;;
        mpath) d1=$mpath1; d2=$mpath2; d3=${mpath3:-} ;;
    esac
    echo "=== Testing device type: $devtype ($d1, $d2${d3:+, $d3}) ==="

#
# Section 1: pvcreate adds devs to devices file with correct fields
#

echo "== Section 1: pvcreate adds devs to devices file =="

wipe_all
node1 "rm -f $DF"
node1 "mkdir -p $DFDIR"
node1 "touch $DF"

for dev in $d1 $d2 ${d3:+$d3}; do
    node1 pvcreate $dev

    pvid=$(node1 "pvs $dev --noheadings -o uuid | tr -d - | awk '{print \$1}'")
    maj=$(node1 "pvs $dev --noheadings -o major | awk '{print \$1}'")
    min=$(node1 "pvs $dev --noheadings -o minor | awk '{print \$1}'")

    node1 "grep PVID=$pvid $DF"
    node1 "grep DEVNAME=$dev $DF"

    idtype=$(node1 "grep DEVNAME=$dev $DF | grep -oP 'IDTYPE=\K\S+'")
    idname=$(node1 "grep DEVNAME=$dev $DF | grep -oP 'IDNAME=\K\S+'")
    echo "  $dev: IDTYPE=$idtype IDNAME=$idname PVID=$pvid"
done

node1 "cp $DF ${DF}.orig"

#
# Section 2: vgcreate from existing PVs already in devices file
#

echo "== Section 2: vgcreate from existing PVs =="

node1 vgcreate testvg $d1 $d2 ${d3:+$d3}
node1 vgs testvg
node1 vgremove -y testvg
node1 "rm $DF"

# vgcreate from existing PVs, adding to devices file
node1 "touch $DF"
node1 vgcreate testvg $d1 $d2 ${d3:+$d3}

node1 "grep IDNAME $DF" > /dev/null
node1 "grep IDNAME ${DF}.orig" > /dev/null

#
# Section 3: device id metadata fields
#

echo "== Section 3: device id metadata fields =="

for dev in $d1 $d2 ${d3:+$d3}; do
    node1 "grep $dev $DF"
    deviceid=$(node1 "pvs $dev --noheadings -o deviceid | awk '{print \$1}'")
    deviceidtype=$(node1 "pvs $dev --noheadings -o deviceidtype | awk '{print \$1}'")
    node1 "grep $dev $DF | grep $deviceid"
    node1 "grep $dev $DF | grep $deviceidtype"
    node1 lvcreate -l1 testvg $dev
done

node1 vgchange -an testvg
node1 vgremove -y testvg

#
# Section 4: pvremove leaves devs in devices file without pvid
#

echo "== Section 4: pvremove leaves devs in devices file =="

node1 "rm $DF"
node1 "touch $DF"
for dev in $d1 $d2 ${d3:+$d3}; do
    node1 pvcreate $dev
done

for dev in $d1 $d2 ${d3:+$d3}; do
    pvid=$(node1 "pvs $dev --noheadings -o uuid | tr -d - | awk '{print \$1}'")
    node1 pvremove $dev
    node1 "grep $dev $DF"
    node1 "not grep $pvid $DF"
done

#
# Section 5: vgextend/vgreduce with devices file
#

echo "== Section 5: vgextend/vgreduce with devices file =="

wipe_all
node1 "rm $DF"
node1 "touch $DF"

node1 pvcreate $d1
node1 pvcreate $d2
node1 vgcreate testvg $d1
node1 vgextend testvg $d2
node1 "grep $d1 $DF"
node1 "grep $d2 $DF"
id1=$(node1 "pvs $d1 --noheadings -o deviceid | awk '{print \$1}'")
id2=$(node1 "pvs $d2 --noheadings -o deviceid | awk '{print \$1}'")
node1 "grep $id1 $DF"
node1 "grep $id2 $DF"
node1 vgreduce testvg $d2
node1 "grep $d2 $DF"
node1 vgremove -y testvg

#
# Section 6: devs not visible until added to devices file
#

echo "== Section 6: devs not visible until added to devices file =="

wipe_all
node1 "rm $DF"
node1 "touch $DF"

node1 not pvs $d1
node1 not pvs $d2
node1 "pvs -a | not grep $d1"
node1 "pvs -a | not grep $d2"
node1 "not grep $d1 $DF"
node1 "not grep $d2 $DF"

node1 pvcreate $d1

node1 pvs $d1
node1 not pvs $d2
node1 "pvs -a | grep $d1"
node1 "pvs -a | not grep $d2"
node1 "grep $d1 $DF"
node1 "not grep $d2 $DF"

node1 pvcreate $d2

node1 pvs $d1
node1 pvs $d2
node1 "pvs -a | grep $d1"
node1 "pvs -a | grep $d2"
node1 "grep $d1 $DF"
node1 "grep $d2 $DF"

node1 vgcreate testvg $d1
node1 vgextend testvg $d2
node1 pvs $d1
node1 pvs $d2
node1 vgremove -y testvg

#
# Section 7: vgimportdevices
#

echo "== Section 7: vgimportdevices =="

wipe_all
node1 "rm $DF"

node1 vgcreate testvg $d1 $d2 ${d3:+$d3}
node1 "rm -f $DF"
node1 "touch $DF"

for dev in $d1 $d2 ${d3:+$d3}; do
    node1 not pvs $dev
done

node1 vgimportdevices testvg

for dev in $d1 $d2 ${d3:+$d3}; do
    node1 pvs $dev
done

node1 vgremove -y testvg

#
# Section 8: vgimportdevices -a
#

echo "== Section 8: vgimportdevices -a =="

wipe_all
node1 "rm $DF"

node1 vgcreate testvg1 $d1
node1 vgcreate testvg2 $d2

node1 "rm -f $DF"
node1 vgimportdevices -a
node1 "ls $DF"

node1 vgs testvg1
node1 vgs testvg2
node1 pvs $d1
node1 pvs $d2

node1 vgremove -y testvg1
node1 vgremove -y testvg2

#
# Section 9: vgimportclone --importdevices
#

echo "== Section 9: vgimportclone --importdevices =="

wipe_all
node1 "rm $DF"

node1 pvcreate $d1
node1 vgcreate testvg1 $d1
node1 vgimportdevices testvg1

node1 "dd if=$d1 of=$d2 bs=1M count=1"

node1 pvs $d1
node1 not pvs $d2

node1 "grep $d1 $DF"
node1 "not grep $d2 $DF"

node1 not vgimportclone $d2
node1 "not grep $d2 $DF"

node1 vgimportclone --basevgname testvg2 --importdevices $d2

pvid1=$(node1 "pvs $d1 --noheadings -o uuid | tr -d - | awk '{print \$1}'")
pvid2=$(node1 "pvs $d2 --noheadings -o uuid | tr -d - | awk '{print \$1}'")
test "$pvid1" != "$pvid2"

id1=$(node1 "pvs $d1 --noheadings -o deviceid | awk '{print \$1}'")
id2=$(node1 "pvs $d2 --noheadings -o deviceid | awk '{print \$1}'")
test "$id1" != "$id2"

node1 "grep $d1 $DF"
node1 "grep $d2 $DF"
node1 "grep $pvid1 $DF"
node1 "grep $pvid2 $DF"
node1 "grep $id1 $DF"
node1 "grep $id2 $DF"

node1 vgs testvg1
node1 vgs testvg2

node1 vgremove -y testvg1
node1 vgremove -y testvg2

#
# Section 10: lvmdevices --adddev / --deldev
#

echo "== Section 10: lvmdevices --adddev / --deldev =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"

for dev in $d1 $d2 ${d3:+$d3}; do
    node1 pvcreate $dev
done

PVID1=$(node1 "grep $d1 $DF | grep -oP 'PVID=\K\S+'")
PVID2=$(node1 "grep $d2 $DF | grep -oP 'PVID=\K\S+'")
PVID3=""
DID1=$(node1 "grep $d1 $DF | grep -oP 'IDNAME=\K\S+'")
DID2=$(node1 "grep $d2 $DF | grep -oP 'IDNAME=\K\S+'")
DID3=""
if [ -n "$d3" ]; then
    PVID3=$(node1 "grep $d3 $DF | grep -oP 'PVID=\K\S+'")
    DID3=$(node1 "grep $d3 $DF | grep -oP 'IDNAME=\K\S+'")
fi

node1 "rm $DF"
node1 "touch $DF"
node1 lvmdevices

# adddev
for dev in $d1 $d2 ${d3:+$d3}; do
    node1 not pvs $dev
    node1 lvmdevices --adddev $dev
    node1 pvs $dev
    node1 "grep $dev $DF"
done

node1 "lvmdevices | grep $DID1"
node1 "lvmdevices | grep $DID2"
[ -n "$DID3" ] && node1 "lvmdevices | grep $DID3"

# deldev
for dev in $d1 $d2 ${d3:+$d3}; do
    node1 pvs $dev
    node1 lvmdevices --deldev $dev
    node1 "not grep $dev $DF"
    node1 not pvs $dev
done

#
# Section 11: lvmdevices --addpvid / --delpvid
#

echo "== Section 11: lvmdevices --addpvid / --delpvid =="

node1 "rm -f $DF"
node1 "touch $DF"

# addpvid
for pvid in $PVID1 $PVID2 ${PVID3:+$PVID3}; do
    node1 lvmdevices --addpvid $pvid
    node1 "grep $pvid $DF"
done
node1 pvs $d1
node1 pvs $d2
[ -n "$d3" ] && node1 pvs $d3

# delpvid
for pvid in $PVID1 $PVID2 ${PVID3:+$PVID3}; do
    node1 lvmdevices --delpvid $pvid
    node1 "not grep $pvid $DF"
done
node1 not pvs $d1
node1 not pvs $d2
[ -n "$d3" ] && node1 not pvs $d3

#
# Section 12: wrong pvid in devices file, --check and --update
#

echo "== Section 12: wrong pvid in devices file =="

node1 "rm -f $DF"
node1 "touch $DF"
node1 lvmdevices --adddev $d1
node1 lvmdevices --adddev $d2
node1 "cp $DF ${DF}.orig2"

node1 "rm $DF"
node1 "sed 's/$PVID1/badpvid/' ${DF}.orig2 > $DF"
node1 "not grep $PVID1 $DF"
node1 "grep $DID1 $DF"

node1 "not lvmdevices --check 2>&1 | grep $d1"
node1 "not lvmdevices --check 2>&1 | grep badpvid"

node1 lvmdevices --update

node1 "grep $PVID1 $DF"
node1 "grep $DID1 $DF"
node1 "not grep badpvid $DF"

#
# Section 13: wrong devname in devices file, --check and --update
#

echo "== Section 13: wrong devname in devices file =="

node1 "rm $DF"
D1=$(node1 "basename $d1")
WRONGNAME="fakename"
node1 "sed 's/$D1/$WRONGNAME/' ${DF}.orig2 > $DF"
node1 "not lvmdevices --check 2>&1 | grep $d1"

node1 lvmdevices --update

node1 "lvmdevices | grep $d1 | grep $PVID1 | grep $DID1"
node1 "lvmdevices | grep $d2 | grep $PVID2 | grep $DID2"

#
# Section 14: swapped devnames in devices file
#

echo "== Section 14: swapped devnames in devices file =="

D1=$(node1 "basename $d1")
D2=$(node1 "basename $d2")
node1 "sed 's/$D1/tmp/' ${DF}.orig2 > ${DF}_1"
node1 "sed 's/$D2/$D1/' ${DF}_1 > ${DF}_2"
node1 "sed 's/tmp/$D2/' ${DF}_2 > $DF"
node1 "rm ${DF}_1 ${DF}_2"
node1 "not lvmdevices --check 2>&1 | grep $d1"
node1 "not lvmdevices --check 2>&1 | grep $d2"

node1 lvmdevices --update

node1 "lvmdevices | grep $d1 | grep $PVID1 | grep $DID1"
node1 "lvmdevices | grep $d2 | grep $PVID2 | grep $DID2"

#
# Section 15: ordinary command fixes wrong devname in devices file
#

echo "== Section 15: ordinary command fixes wrong devname =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"
node1 pvcreate $d1
node1 pvcreate $d2
node1 vgcreate testvg $d1 $d2

PVID1=$(node1 "grep $d1 $DF | grep -oP 'PVID=\K\S+'")
PVID2=$(node1 "grep $d2 $DF | grep -oP 'PVID=\K\S+'")
DID1=$(node1 "grep $d1 $DF | grep -oP 'IDNAME=\K\S+'")
DID2=$(node1 "grep $d2 $DF | grep -oP 'IDNAME=\K\S+'")
node1 "cp $DF ${DF}.orig3"

D1=$(node1 "basename $d1")
WRONGNAME="fakename"
node1 "rm $DF"
node1 "sed 's/$D1/$WRONGNAME/' ${DF}.orig3 > $DF"

# pvs should work correctly and fix the devname in DF
node1 "pvs -o+uuid,deviceid | grep testvg | grep $d1"
node1 "pvs -o+uuid,deviceid | grep testvg | grep $d2"
node1 "pvs -o+uuid,deviceid | grep testvg | not grep $WRONGNAME"

# verify DF is fixed
node1 "grep $d1 $DF"
node1 "grep $d2 $DF"
node1 "not grep $WRONGNAME $DF"

node1 vgremove -y testvg

#
# Section 16: lvmdevices --listids
#

echo "== Section 16: lvmdevices --listids =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"
node1 pvcreate $d1

IDTYPE1=$(node1 "grep $d1 $DF | grep -oP 'IDTYPE=\K\S+'")
IDNAME1=$(node1 "grep $d1 $DF | grep -oP 'IDNAME=\K\S+'")

node1 "lvmdevices --listids $d1 | grep $IDTYPE1"
node1 "lvmdevices --listids $d1 --deviceidtype $IDTYPE1 | grep $IDNAME1"
node1 not lvmdevices --listids $d1 --deviceidtype badtype

node1 pvremove $d1

#
# Section 17: lvmdevices --addid / --delid
#

echo "== Section 17: lvmdevices --addid / --delid =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"
node1 pvcreate $d1

DID1=$(node1 "grep $d1 $DF | grep -oP 'IDNAME=\K\S+'")
DIT1=$(node1 "grep $d1 $DF | grep -oP 'IDTYPE=\K\S+'")

node1 lvmdevices --deldev $d1
node1 "not grep $d1 $DF"
node1 not pvs $d1

node1 lvmdevices --addid $DID1 --deviceidtype $DIT1
node1 "grep $d1 $DF"
node1 "grep $DID1 $DF"
node1 pvs $d1

node1 lvmdevices --delid $DID1 --deviceidtype $DIT1
node1 "not grep $DID1 $DF"
node1 not pvs $d1

node1 not lvmdevices --addid bogus_id_value --deviceidtype $DIT1

#
# Section 18: lvmdevices --deldev with --deviceidtype
#

echo "== Section 18: lvmdevices --deldev with --deviceidtype =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"
node1 pvcreate $d1
node1 pvcreate $d2

DID1=$(node1 "grep $d1 $DF | grep -oP 'IDNAME=\K\S+'")
DIT1=$(node1 "grep $d1 $DF | grep -oP 'IDTYPE=\K\S+'")
DID2=$(node1 "grep $d2 $DF | grep -oP 'IDNAME=\K\S+'")
DIT2=$(node1 "grep $d2 $DF | grep -oP 'IDTYPE=\K\S+'")

node1 lvmdevices --deldev $DID1 --deviceidtype $DIT1
node1 "not grep $DID1 $DF"
node1 "grep $DID2 $DF"

node1 lvmdevices --deldev $DID2 --deviceidtype $DIT2
node1 "not grep $DID2 $DF"

#
# Section 19: lvmdevices --adddev with --deviceidtype override
#

echo "== Section 19: lvmdevices --adddev with --deviceidtype override =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"
node1 pvcreate $d1

DIT1=$(node1 "grep $d1 $DF | grep -oP 'IDTYPE=\K\S+'")

node1 lvmdevices --deldev $d1
node1 lvmdevices --adddev $d1 --deviceidtype devname
node1 "grep $d1 $DF | grep IDTYPE=devname"
node1 pvs $d1

node1 lvmdevices --deldev $d1
node1 lvmdevices --adddev $d1
node1 "grep $d1 $DF | grep IDTYPE=$DIT1"

node1 pvremove $d1

#
# Section 20: lvmdevices --devicesfile (custom devices file)
#

echo "== Section 20: lvmdevices --devicesfile =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"
node1 pvcreate $d1
node1 pvcreate $d2

node1 "touch $DFDIR/test.devices"
node1 lvmdevices --devicesfile test.devices --adddev $d1

node1 "grep $d1 $DFDIR/test.devices"
node1 "not grep $d2 $DFDIR/test.devices"

node1 "grep $d1 $DF"
node1 "grep $d2 $DF"

node1 pvs --devicesfile test.devices $d1
node1 not pvs --devicesfile test.devices $d2

node1 pvs $d1
node1 pvs $d2

node1 vgcreate --devicesfile test.devices testvg $d1
node1 vgs --devicesfile test.devices testvg
node1 vgremove --devicesfile test.devices -y testvg

node1 pvremove $d1
node1 pvremove $d2
node1 "rm -f $DFDIR/test.devices"

#
# Section 21: lvmdevices --update --delnotfound
#

echo "== Section 21: lvmdevices --update --delnotfound =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"
node1 pvcreate $d1
node1 pvcreate $d2

node1 "echo 'IDTYPE=devname IDNAME=/dev/sdxyz DEVNAME=/dev/sdxyz PVID=aaaa1234aaaa1234aaaa1234aaaa1234' >> $DF"

node1 "grep /dev/sdxyz $DF"
node1 "grep $d1 $DF"
node1 "grep $d2 $DF"

node1 lvmdevices --update --delnotfound

node1 "not grep /dev/sdxyz $DF"
node1 "grep $d1 $DF"
node1 "grep $d2 $DF"
node1 pvs $d1
node1 pvs $d2

node1 pvremove $d1
node1 pvremove $d2

#
# Section 22: lvmdevices --test (dry run mode)
#

echo "== Section 22: lvmdevices --test (dry run mode) =="

wipe_all
node1 "rm -f $DF"
node1 "touch $DF"
node1 pvcreate $d1
node1 pvcreate $d2

PVID1=$(node1 "grep $d1 $DF | grep -oP 'PVID=\K\S+'")
DID1=$(node1 "grep $d1 $DF | grep -oP 'IDNAME=\K\S+'")

node1 lvmdevices --deldev $d2
node1 "not grep $d2 $DF"
node1 "cp $DF ${DF}.saved"

node1 lvmdevices --adddev $d2 --test
node1 "not grep $d2 $DF"
node1 "diff $DF ${DF}.saved"

node1 lvmdevices --adddev $d2
node1 "grep $d2 $DF"

node1 "cp $DF ${DF}.good"
node1 "sed 's/$PVID1/badpvidtest/' $DF > ${DF}.tmp && mv ${DF}.tmp $DF"
node1 "grep badpvidtest $DF"
node1 "cp $DF ${DF}.corrupt"

node1 lvmdevices --update --test
node1 "grep badpvidtest $DF"
node1 "diff $DF ${DF}.corrupt"

node1 lvmdevices --update
node1 "not grep badpvidtest $DF"
node1 "grep $PVID1 $DF"

node1 pvremove $d1
node1 pvremove $d2
node1 "rm -f ${DF}.saved ${DF}.good ${DF}.corrupt"

echo "== Cleanup ($devtype) =="

wipe_all
node1 "rm -f $DF ${DF}.orig ${DF}.orig2 ${DF}.orig3"
node1 "rm -f $DFDIR/test.devices"
node1 "rm -f ${DF}.saved ${DF}.good ${DF}.corrupt"
node1 "rm -f /run/lvm/hints"
node1 "touch $DF"

done  # end devtype loop

#
# Section 23: mixed device types in one VG
#

num_types=$(echo $DEV_TYPES | wc -w)
if [ "$num_types" -ge 2 ]; then

echo "== Section 23: mixed device types in one VG =="

mixdevs=""
[ -n "${scsi1:-}" ]  && mixdevs="$mixdevs $scsi1"
[ -n "${nvme1:-}" ]  && mixdevs="$mixdevs $nvme1"
[ -n "${mpath1:-}" ] && mixdevs="$mixdevs $mpath1"

node1 "rm -f $DF"
node1 "touch $DF"

node1 vgcreate testvg $mixdevs

for dev in $mixdevs; do
    idtype=$(node1 "grep DEVNAME=$dev $DF | grep -oP 'IDTYPE=\K\S+'")
    idname=$(node1 "grep DEVNAME=$dev $DF | grep -oP 'IDNAME=\K\S+'")
    pvid=$(node1 "grep DEVNAME=$dev $DF | grep -oP 'PVID=\K\S+'")
    echo "  $dev: IDTYPE=$idtype IDNAME=$idname PVID=$pvid"
done

num_idtypes=$(node1 "grep IDTYPE $DF | grep -oP 'IDTYPE=\K\S+' | sort -u | wc -l")
test "$num_idtypes" -ge 2

# vgextend with a device of a different type than the first
extdev=""
first_type=$(echo $DEV_TYPES | awk '{print $1}')
for t in $DEV_TYPES; do
    if [ "$t" != "$first_type" ]; then
        case $t in
            scsi)  extdev=$scsi2 ;;
            nvme)  extdev=$nvme2 ;;
            mpath) extdev=$mpath2 ;;
        esac
        break
    fi
done

if [ -n "$extdev" ]; then
    node1 pvcreate $extdev
    node1 vgextend testvg $extdev
    node1 "grep DEVNAME=$extdev $DF"
    node1 vgreduce testvg $extdev
    node1 pvremove $extdev
fi

# vgimportdevices round-trip with mixed types
node1 "rm $DF"
node1 "touch $DF"
for dev in $mixdevs; do
    node1 not pvs $dev
done

node1 vgimportdevices testvg

for dev in $mixdevs; do
    node1 pvs $dev
    node1 "grep DEVNAME=$dev $DF"
done

node1 lvmdevices --check

node1 vgremove -y testvg
for dev in $mixdevs; do
    node1 wipefs -a $dev
done
node1 "rm -f $DF"
node1 "touch $DF"

fi  # num_types >= 2

echo "== All device-id tests passed =="

exit 0
