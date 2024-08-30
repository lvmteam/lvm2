#!/usr/bin/env bash

# Copyright (C) 2018-2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Exercise obtaining vdo parameters from various sources
# Either command line or metadata profile or implicit default...


SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_vdo 6 2 0 || skip

PDIR="$LVM_SYSTEM_DIR/profile"
PFILE="vdo-test"

aux prepare_profiles

cat <<EOF > "$PDIR/${PFILE}.profile"
allocation {
	vdo_use_compression = 0
	vdo_use_deduplication = 0
	vdo_slab_size_mb = 128
}
EOF

aux prepare_vg 2 1000000

# Check chunk_size is grabbed from configuration
lvcreate --vdo -L5G --config 'allocation/vdo_use_compression=0' $vg/vdopool
lvdisplay -m $vg/vdopool | tee out
grep "Compression.*no" out
lvremove -f $vg

# Without profile using 128MB slab it would NOT even pass
lvcreate --vdo -L4G --metadataprofile "$PFILE" $vg/vdopool
lvdisplay -m $vg/vdopool | tee out
grep "Compression.*no" out
lvremove -f $vg

lvcreate -L4G --name vdopool $vg
lvconvert --yes --type vdo-pool --metadataprofile "$PFILE" $vg/vdopool
check lv_field $vg/vdopool vdo_compression ""

vgremove -ff $vg
