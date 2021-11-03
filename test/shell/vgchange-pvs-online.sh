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

vgcreate $vg1 "$dev1" "$dev2"
vgcreate $vg2 "$dev3"
pvcreate "$dev4"

lvcreate -l1 -n $lv1 -an $vg1
lvcreate -l1 -n $lv1 -an $vg2

# With no pv online files, vgchange that uses online files
# will find no PVs to activate from.

_clear_online_files

not vgchange -aay --autoactivation event $vg1
not vgchange -aay --autoactivation event $vg2
vgchange -aay --autoactivation event

check lv_field $vg1/$lv1 lv_active ""
check lv_field $vg2/$lv1 lv_active ""

# incomplete vg will not be activated

pvscan --cache "$dev1"
vgchange -aay --autoactivation event $vg1
# VG foo is incomplete
check lv_field $vg1/$lv1 lv_active ""

# complete vg is activated

pvscan --cache "$dev3"
vgchange -aay --autoactivation event $vg2
check lv_field $vg2/$lv1 lv_active "active"

pvscan --cache "$dev2"
vgchange -aay --autoactivation event $vg1
check lv_field $vg1/$lv1 lv_active "active"

vgchange -an $vg1
vgchange -an $vg2

# the same tests but using command options matching udev rule

_clear_online_files

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event --udevoutput --journal=output "$dev1"
vgchange -aay --autoactivation event $vg1
# VG foo is incomplete
check lv_field $vg1/$lv1 lv_active ""

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event --udevoutput --journal=output "$dev3"
vgchange -aay --autoactivation event $vg2
check lv_field $vg2/$lv1 lv_active "active"

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event --udevoutput --journal=output "$dev2"
vgchange -aay --autoactivation event $vg1
check lv_field $vg1/$lv1 lv_active "active"

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

