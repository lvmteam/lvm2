SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1

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

. lib/inittest

aux prepare_devs 4

# skip rhel5 which doesn't seem to have /dev/mapper/LVMTESTpv1
aux driver_at_least 4 15 || skip

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"

# Because mapping devno to devname gets dm name from sysfs
aux lvmconf 'devices/scan = "/dev"'
base1=$(basename $dev1)
base2=$(basename $dev2)
base3=$(basename $dev3)
base4=$(basename $dev4)
bd1=/dev/mapper/$base1
bd2=/dev/mapper/$base2
bd3=/dev/mapper/$base3
bd4=/dev/mapper/$base4
aux extend_filter "a|/dev/mapper/$base1|"
aux extend_filter "a|/dev/mapper/$base2|"
aux extend_filter "a|/dev/mapper/$base3|"
aux extend_filter "a|/dev/mapper/$base4|"

# Changing names will confuse df based on devname
if lvmdevices; then
rm -f "$DF"
touch "$DF"
lvmdevices --adddev "$bd1"
lvmdevices --adddev "$bd2"
lvmdevices --adddev "$bd3"
lvmdevices --adddev "$bd4"
cat "$DF"
fi

# Using $bd instead of $dev because validation of pvid file content
# checks that the devname begins with /dev.

# FIXME: test vgchange aay with pvs_online that includes devname in pvid file
# and the devices file entry uses devname with a stale name.

vgcreate $vg1 "$bd1" "$bd2"
vgcreate $vg2 "$bd3"
pvcreate "$bd4"

lvcreate -l1 -n $lv1 -an $vg1
lvcreate -l1 -n $lv2 -an $vg1
lvcreate -l1 -n $lv1 -an $vg2

# Expected use, with vg name and all online files exist for vgchange.

_clear_online_files

pvscan --cache "$bd1"
pvscan --cache "$bd2"
vgchange -aay --autoactivation event $vg1
check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg1/$lv2 lv_active "active"
check lv_field $vg2/$lv1 lv_active ""

pvscan --cache "$bd3"
vgchange -aay --autoactivation event $vg2
check lv_field $vg2/$lv1 lv_active "active"

# Count io to check the pvs_online optimization 
# is working to limit scanning.

if which strace; then
vgchange -an
_clear_online_files

pvscan --cache "$bd1"
pvscan --cache "$bd2"
strace -e io_submit vgchange -aay --autoactivation event $vg1 2>&1|tee trace.out
test "$(grep io_submit trace.out | wc -l)" -eq 3
rm trace.out

strace -e io_submit pvscan --cache "$bd3" 2>&1|tee trace.out
test "$(grep io_submit trace.out | wc -l)" -eq 1
rm trace.out

strace -e io_submit vgchange -aay --autoactivation event $vg2 2>&1|tee trace.out
test "$(grep io_submit trace.out | wc -l)" -eq 2
rm trace.out
fi

# non-standard usage: no VG name arg, vgchange will only used pvs_online files

vgchange -an
_clear_online_files

vgchange -aay --autoactivation event
check lv_field $vg1/$lv1 lv_active ""
check lv_field $vg1/$lv2 lv_active ""
check lv_field $vg2/$lv1 lv_active ""

pvscan --cache "$bd1"
vgchange -aay --autoactivation event
check lv_field $vg1/$lv1 lv_active ""
check lv_field $vg1/$lv2 lv_active ""
check lv_field $vg2/$lv1 lv_active ""

pvscan --cache "$bd2"
vgchange -aay --autoactivation event
check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg1/$lv2 lv_active "active"
check lv_field $vg2/$lv1 lv_active ""

pvscan --cache "$bd3"
vgchange -aay --autoactivation event
check lv_field $vg2/$lv1 lv_active "active"

# non-standard usage: include VG name arg, but missing or incomplete pvs_online files

vgchange -an
_clear_online_files

# all missing pvs_online, vgchange falls back to full label scan
vgchange -aay --autoactivation event $vg1
check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg1/$lv2 lv_active "active"

vgchange -an
_clear_online_files

# incomplete pvs_online, vgchange falls back to full label scan
pvscan --cache "$bd1"
vgchange -aay --autoactivation event $vg1
check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg1/$lv2 lv_active "active"

vgchange -an
_clear_online_files

# incomplete pvs_online, pvs_online from different vg,
# no pvs_online found for vg arg so vgchange falls back to full label scan

pvscan --cache "$bd3"
vgchange -aay --autoactivation event $vg1
check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg1/$lv2 lv_active "active"
check lv_field $vg2/$lv1 lv_active ""

vgchange -aay --autoactivation event $vg2
check lv_field $vg2/$lv1 lv_active "active"

vgchange -an
_clear_online_files

# same tests but using command options matching udev rule

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event --udevoutput --journal=output "$bd1"
pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event --udevoutput --journal=output "$bd2"
vgchange -aay --autoactivation event $vg1
check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg1/$lv2 lv_active "active"
check lv_field $vg2/$lv1 lv_active ""

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event --udevoutput --journal=output "$bd3"
vgchange -aay --autoactivation event $vg2
check lv_field $vg2/$lv1 lv_active "active"

vgchange -an $vg1
vgchange -an $vg2

# with a full pvscan --cache

_clear_online_files

pvscan --cache
check lv_field $vg1/$lv1 lv_active ""
check lv_field $vg2/$lv1 lv_active ""
vgchange -aay --autoactivation event $vg1
vgchange -aay --autoactivation event $vg2
check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg2/$lv1 lv_active "active"

vgchange -an $vg1
vgchange -an $vg2

vgremove -f $vg1
vgremove -f $vg2

