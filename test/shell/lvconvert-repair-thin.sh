#!/bin/sh

# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Test repairing of broken thin pool metadata

. lib/inittest

which mkfs.ext2 || skip

# By default use tools from configuration (exported through Makefile)
# Allow user to override location of binaries to take tools from different laces
# Maybe check also version of the tools here?
test -n "$LVM_TEST_THIN_CHECK_CMD" || LVM_TEST_THIN_CHECK_CMD=$(which thin_check) || skip
test -n "$LVM_TEST_THIN_DUMP_CMD" || LVM_TEST_THIN_DUMP_CMD=$(which thin_dump) || skip
test -n "$LVM_TEST_THIN_REPAIR_CMD" || LVM_TEST_THIN_REPAIR_CMD=$(which thin_repair) || skip

#
# Main
#
aux have_thin 1 0 0 || skip

aux prepare_vg 4

# Create LV
lvcreate -T -L20 -V10 -n $lv1 $vg/pool  "$dev1" "$dev2"
lvcreate -T -V10 -n $lv2 $vg/pool

mkfs.ext2 "$DM_DEV_DIR/$vg/$lv1"
mkfs.ext2 "$DM_DEV_DIR/$vg/$lv2"

lvcreate -L20 -n repair $vg
lvcreate -L2 -n fixed $vg

lvs -a -o+seg_pe_ranges $vg
#aux error_dev "$dev2" 2050:1

# Make some repairable metadata damage??
vgchange -an $vg

lvconvert --repair $vg/pool

lvs -a $vg

# Test swapping - swap out thin-pool's metadata with our repair volume
lvconvert -y -f --poolmetadata $vg/repair --thinpool $vg/pool

lvchange -aey $vg/repair $vg/fixed

#dd if="$DM_DEV_DIR/$vg/repair" of=back bs=1M

# Make some 'repairable' damage??
dd if=/dev/zero of="$DM_DEV_DIR/$vg/repair" bs=1 seek=40960 count=1

#dd if="$DM_DEV_DIR/$vg/repair" of=back_trashed bs=1M
#not vgchange -ay $vg

#lvconvert --repair $vg/pool

# Using now SHOULD - since thin tools currently do not seem to work
should not "$THIN_CHECK" "$DM_DEV_DIR/$vg/repair"

should not "$LVM_TEST_THIN_DUMP_CMD" "$DM_DEV_DIR/$vg/repair" | tee dump

should "$LVM_TEST_THIN_REPAIR_CMD" -i "$DM_DEV_DIR/$vg/repair" -o "$DM_DEV_DIR/$vg/fixed"

should "$LVM_TEST_THIN_DUMP_CMD" --repair "$DM_DEV_DIR/$vg/repair" | tee repaired_xml

should "$LVM_TEST_THIN_CHECK_CMD" "$DM_DEV_DIR/$vg/fixed"

# Swap repaired metadata back
lvconvert -y -f --poolmetadata $vg/fixed --thinpool $vg/pool

# Activate pool - this should now work
should vgchange -ay $vg

lvs -a -o+devices $vg
dmsetup table
dmsetup info -c
dmsetup ls --tree

lvchange -an $vg

# FIXME: Currently in deep troubles - we can't remove thin volume from broken pool
should lvremove -ff $vg

# let's not block PVs with openned _tdata/_tmeta devices
aux dmsetup remove $vg-pool_tdata || true
aux dmsetup remove $vg-pool_tmeta || true

dmsetup table

# FIXME: needs  also --yes with double force
pvremove --yes -ff "$dev1"
pvremove --yes -ff "$dev2"

# FIXME: pv1 & pv2 are removed so pv3 & pv4 have no real LVs,
# yet vgremove is refusing to do its jobs and suggest --partial??
should vgremove -ff  $vg

# FIXME: stressing even more - there are no pool PV, we do not pass...
should vgreduce --removemissing -f $vg
should vgremove -ff  $vg

# Let's do a final forced cleanup
pvremove --yes -ff "$dev3"
pvremove --yes -ff "$dev4"
