
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

aux prepare_devs 1

#
# test lvchange --setautoactivation
#

# default
vgcreate $vg "$dev1"
lvcreate -n $lv1 -l1 -an $vg
check vg_field $vg autoactivation "enabled"
check lv_field $vg/$lv1 autoactivation "enabled"

lvchange -aay $vg/$lv1
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
lvchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
_clear_online_files

# --aa=n
lvchange --setautoactivation n $vg/$lv1
check vg_field $vg autoactivation "enabled"
check lv_field $vg/$lv1 autoactivation ""

lvchange -aay $vg/$lv1
check lv_field $vg/$lv1 lv_active ""
lvchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active ""
_clear_online_files

# --aa=y
lvchange --setautoactivation y $vg/$lv1
check vg_field $vg autoactivation "enabled"
check lv_field $vg/$lv1 autoactivation "enabled"

lvchange -aay $vg/$lv1
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
lvchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
_clear_online_files

vgremove -y $vg

#
# test vgchange --setautoactivation
#

# default
vgcreate $vg "$dev1"
lvcreate -n $lv1 -l1 -an $vg

# --aa=n
vgchange --setautoactivation n $vg
check vg_field $vg autoactivation ""
check lv_field $vg/$lv1 autoactivation "enabled"

lvchange -aay $vg/$lv1
check lv_field $vg/$lv1 lv_active ""
lvchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active ""
_clear_online_files

# --aa=y
vgchange --setautoactivation y $vg
check vg_field $vg autoactivation "enabled"
check lv_field $vg/$lv1 autoactivation "enabled"

lvchange -aay $vg/$lv1
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
lvchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active "active"
lvchange -an $vg/$lv1
_clear_online_files

vgremove -y $vg

#
# test vgcreate --setautoactivation, lvcreate --setautoactivation
#

vgcreate $vg "$dev1"
lvcreate -n $lv1 -l1 -an $vg
lvcreate -n $lv2 -l1 --setautoactivation y -an $vg
lvcreate -n $lv3 -l1 --setautoactivation n -an $vg
check vg_field $vg autoactivation "enabled"
check lv_field $vg/$lv1 autoactivation "enabled"
check lv_field $vg/$lv2 autoactivation "enabled"
check lv_field $vg/$lv3 autoactivation ""
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active "active"
check lv_field $vg/$lv3 lv_active ""
vgchange -an $vg
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
lvchange -aay $vg/$lv3
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active "active"
check lv_field $vg/$lv3 lv_active ""
vgchange -an $vg
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active "active"
check lv_field $vg/$lv3 lv_active ""
vgchange -an $vg
vgremove -y $vg
_clear_online_files

vgcreate --setautoactivation y $vg "$dev1"
lvcreate -n $lv1 -l1 -an $vg
lvcreate -n $lv2 -l1 --setautoactivation y -an $vg
lvcreate -n $lv3 -l1 --setautoactivation n -an $vg
check vg_field $vg autoactivation "enabled"
check lv_field $vg/$lv1 autoactivation "enabled"
check lv_field $vg/$lv2 autoactivation "enabled"
check lv_field $vg/$lv3 autoactivation ""
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active "active"
check lv_field $vg/$lv3 lv_active ""
vgchange -an $vg
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
lvchange -aay $vg/$lv3
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active "active"
check lv_field $vg/$lv3 lv_active ""
vgchange -an $vg
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active "active"
check lv_field $vg/$lv3 lv_active ""
vgchange -an $vg
vgremove -y $vg
_clear_online_files

vgcreate --setautoactivation n $vg "$dev1"
lvcreate -n $lv1 -l1 -an $vg
lvcreate -n $lv2 -l1 --setautoactivation y -an $vg
lvcreate -n $lv3 -l1 --setautoactivation n -an $vg
check vg_field $vg autoactivation ""
check lv_field $vg/$lv1 autoactivation "enabled"
check lv_field $vg/$lv2 autoactivation "enabled"
check lv_field $vg/$lv3 autoactivation ""
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
check lv_field $vg/$lv3 lv_active ""
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
lvchange -aay $vg/$lv3
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
check lv_field $vg/$lv3 lv_active ""
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
check lv_field $vg/$lv3 lv_active ""
vgremove -y $vg
_clear_online_files


#
# test combination of --aa and auto_activation_volume_list
#

vgcreate $vg "$dev1"
lvcreate -n $lv1 -l1 -an $vg
lvcreate -n $lv2 -l1 --setautoactivation n -an $vg
check vg_field $vg autoactivation "enabled"
check lv_field $vg/$lv1 autoactivation "enabled"
check lv_field $vg/$lv2 autoactivation ""

# list prevents all aa, metadata settings don't matter
aux lvmconf "activation/auto_activation_volume_list = [ ]"
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
_clear_online_files

# list allows all vg aa, metadata allows lv1 -> lv1 activated
aux lvmconf "activation/auto_activation_volume_list = [ \"$vg\" ]"
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
_clear_online_files

# list allows lv1, metadata allows lv1 -> lv1 activated
aux lvmconf "activation/auto_activation_volume_list = [ \"$vg/$lv1\" ]"
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
_clear_online_files

# list allows lv2, metadata allows lv1 -> nothing activated
aux lvmconf "activation/auto_activation_volume_list = [ \"$vg/$lv2\" ]"
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
_clear_online_files

vgremove -y $vg

vgcreate --setautoactivation n $vg "$dev1"
lvcreate -n $lv1 -l1 -an $vg
lvcreate -n $lv2 -l1 --setautoactivation n -an $vg
check vg_field $vg autoactivation ""
check lv_field $vg/$lv1 autoactivation "enabled"
check lv_field $vg/$lv2 autoactivation ""

# list prevents all aa, metadata settings don't matter
aux lvmconf "activation/auto_activation_volume_list = [ ]"
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
_clear_online_files

# list allows lv1, metadata disallows vg -> nothing activated
aux lvmconf "activation/auto_activation_volume_list = [ \"$vg/$lv1\" ]"
vgchange -aay $vg
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
lvchange -aay $vg/$lv1
lvchange -aay $vg/$lv2
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
pvscan --cache -aay "$dev1"
check lv_field $vg/$lv1 lv_active ""
check lv_field $vg/$lv2 lv_active ""
vgchange -an $vg
_clear_online_files

vgremove -y $vg

