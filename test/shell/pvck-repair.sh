#!/usr/bin/env bash

# Copyright (C) 2008-2013,2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/inittest

aux prepare_devs 2
get_devs

# One PV, one mda, pv_header zeroed
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate $vg "$dev1"
dd if=/dev/zero of="$dev1" bs=512 count=2
pvck --dump headers "$dev1" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, one mda, mda_header zeroed
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate $vg "$dev1"
dd if=/dev/zero of="$dev1" bs=512 count=1 seek=8
pvck --dump headers "$dev1" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, one mda, pv_header and mda_header zeroed
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate $vg "$dev1"
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=1 seek=8
pvck --dump headers "$dev1" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, one mda, metadata zeroed, use backup
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate $vg "$dev1"
vgcfgbackup
dd if=/dev/zero of="$dev1" bs=512 count=2 seek=9
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata_search "$dev1" || true
pvck --repair -y -f etc/backup/$vg "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, one mda, mda_header and metadata zeroed, use backup
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate $vg "$dev1"
vgcfgbackup
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata_search "$dev1" || true
pvck --repair -y -f etc/backup/$vg "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, one mda, pv_header, mda_header and metadata zeroed, use backup
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate $vg "$dev1"
vgcfgbackup
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata_search "$dev1" || true
pvck --repair -y -f etc/backup/$vg "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, two mdas, pv_header zeroed
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate --pvmetadatacopies 2 $vg "$dev1"
dd if=/dev/zero of="$dev1" bs=512 count=2
pvck --dump headers "$dev1" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, two mdas, mda_header1 zeroed
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate --pvmetadatacopies 2 $vg "$dev1"
pvck --dump headers "$dev1" || true
dd if=/dev/zero of="$dev1" bs=512 count=1 seek=8
pvck --dump headers "$dev1" || true
pvck --dump metadata_search --settings mda_num=1 "$dev1" || true
pvck --dump metadata_search --settings mda_num=2 "$dev1" || true
pvck --dump metadata --settings mda_num=1 "$dev1" || true
pvck --dump metadata --settings mda_num=2 "$dev1" || true
pvck --dump metadata --settings mda_num=2 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, two mdas, pv_header and mda_header1 zeroed
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate --pvmetadatacopies 2 $vg "$dev1"
pvck --dump headers "$dev1" || true
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=1 seek=8
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata --settings mda_num=2 "$dev1" || true
pvck --dump metadata_search "$dev1" || true
pvck --dump metadata_search --settings mda_num=2 "$dev1" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, two mdas, metadata1 zeroed, use mda2
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate --pvmetadatacopies 2 $vg "$dev1"
pvck --dump headers "$dev1" || true
dd if=/dev/zero of="$dev1" bs=512 count=2 seek=9
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata --settings mda_num=2 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, two mdas, mda_header1 and metadata1 zeroed, use mda2
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate --pvmetadatacopies 2 $vg "$dev1"
pvck --dump headers "$dev1" || true
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata --settings mda_num=2 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, two mdas, pv_header, mda_header1 and metadata1 zeroed, use mda2
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate --pvmetadatacopies 2 $vg "$dev1"
pvck --dump headers "$dev1" || true
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata --settings mda_num=2 "$dev1" || true
pvck --dump metadata_search "$dev1" || true
pvck --dump metadata_search --settings mda_num=2 "$dev1" || true
pvck --dump metadata_search --settings "mda_num=2 seqno=1" -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, two mdas, pv_header, both mda_header, and both metadata zeroed, use backup
# only writes mda1 since there's no evidence that mda2 existed
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate --pvmetadatacopies 2 $vg "$dev1"
pvck --dump headers "$dev1" || true
vgcfgbackup
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=67584
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata --settings mda_num=2 "$dev1" || true
pvck --dump metadata_search "$dev1" || true
pvck --dump metadata_search --settings mda_num=2 "$dev1" || true
pvck --repair -y -f etc/backup/$vg "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, two mdas, pv_header, both mda_header, and both metadata zeroed, use backup
# writes mda1 and also mda2 because of the mda2 settings passed to repair
rm meta || true
dd if=/dev/zero of="$dev1" || true
vgcreate --pvmetadatacopies 2 $vg "$dev1"
pvck --dump headers "$dev1" || true
vgcfgbackup
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=67584
pvck --dump headers "$dev1" || true
pvck --dump metadata "$dev1" || true
pvck --dump metadata --settings mda_num=2 "$dev1" || true
pvck --dump metadata_search "$dev1" || true
pvck --dump metadata_search --settings mda_num=2 "$dev1" || true
pvck --repair --settings "mda2_offset=34603008 mda2_size=1048576" -y -f etc/backup/$vg "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda each, pv_header and mda_header zeroed on each
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
vgcreate $vg "$dev1" "$dev2"
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev2" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=1 seek=8
dd if=/dev/zero of="$dev2" bs=512 count=1 seek=8
pvck --dump headers "$dev1"
pvck --dump headers "$dev2"
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --repair -y -f meta "$dev2"
pvck --dump headers "$dev1"
pvck --dump headers "$dev2"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda each, metadata zeroed on each, use backup
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
vgcreate $vg "$dev1" "$dev2"
vgcfgbackup
dd if=/dev/zero of="$dev1" bs=512 count=2 seek=9
dd if=/dev/zero of="$dev2" bs=512 count=2 seek=9
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --repair -y -f etc/backup/$vg "$dev1"
pvck --repair -y -f etc/backup/$vg "$dev2"
pvck --dump headers "$dev1"
pvck --dump headers "$dev2"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda each, pv_header, mda_header and metadata zeroed on each, use backup
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
vgcreate $vg "$dev1" "$dev2"
vgcfgbackup
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev2" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
dd if=/dev/zero of="$dev2" bs=512 count=3 seek=8
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --repair -y -f etc/backup/$vg "$dev1"
pvck --repair -y -f etc/backup/$vg "$dev2"
pvck --dump headers "$dev1"
pvck --dump headers "$dev2"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda each, pv_header and mda_header zeroed on first
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
vgcreate $vg "$dev1" "$dev2"
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=1 seek=8
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --dump metadata -f meta "$dev2"
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda each, metadata zeroed on first
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
vgcreate $vg "$dev1" "$dev2"
dd if=/dev/zero of="$dev1" bs=512 count=2 seek=9
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --dump metadata -f meta "$dev2"
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda each, pv_header, mda_header and metadata zeroed on first
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
vgcreate $vg "$dev1" "$dev2"
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --dump metadata -f meta "$dev2"
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda on first, no mda on second, zero header on first
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
pvcreate "$dev1"
pvcreate --pvmetadatacopies 0 "$dev2"
vgcreate $vg "$dev1" "$dev2"
dd if=/dev/zero of="$dev1" bs=512 count=2
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --dump headers "$dev1"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda on first, no mda on second, zero headers on both
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
pvcreate "$dev1"
pvcreate --pvmetadatacopies 0 "$dev2"
vgcreate $vg "$dev1" "$dev2"
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev2" bs=512 count=2
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
pvck --repair -y --settings "mda_offset=0 mda_size=0" -f meta "$dev2"
pvck --dump headers "$dev1"
pvck --dump headers "$dev2"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda on first, no mda on second, zero all on first
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
pvcreate "$dev1"
pvcreate --pvmetadatacopies 0 "$dev2"
vgcreate $vg "$dev1" "$dev2"
vgcfgbackup
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=3 seek=8
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --repair -y -f etc/backup/$vg "$dev1"
pvck --repair -y --settings "mda_offset=0 mda_size=0" -f etc/backup/$vg "$dev2"
pvck --dump headers "$dev1"
pvck --dump headers "$dev2"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, two mda on each, pv_header and mda_header1 zeroed on both
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
pvcreate --pvmetadatacopies 2 "$dev1"
pvcreate --pvmetadatacopies 2 "$dev2"
vgcreate $vg "$dev1" "$dev2"
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev2" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=1 seek=8
dd if=/dev/zero of="$dev2" bs=512 count=1 seek=8
pvck --dump headers "$dev1"
pvck --dump headers "$dev2"
pvck --dump metadata_search --settings "mda_num=2 seqno=1" -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
rm meta
pvck --dump metadata_search --settings "mda_num=2 seqno=1" -f meta "$dev2" || true
pvck --repair -y -f meta "$dev2"
rm meta
pvck --dump headers "$dev1"
pvck --dump headers "$dev2"
vgs $vg
lvcreate -l1 -an $vg

# Two PV, one mda each, pv_header and mda_header zeroed on each,
# non-standard data_offset/mda_size on first
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
pvcreate --metadatasize 2048k --dataalignment 128k "$dev1"
pvcreate "$dev2"
vgcreate $vg "$dev1" "$dev2"
dd if=/dev/zero of="$dev1" bs=512 count=2
dd if=/dev/zero of="$dev1" bs=512 count=1 seek=8
dd if=/dev/zero of="$dev2" bs=512 count=2
dd if=/dev/zero of="$dev2" bs=512 count=1 seek=8
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
pvck --repair -y -f meta "$dev1"
rm meta
pvck --dump metadata_search --settings seqno=1 -f meta "$dev2" || true
pvck --repair -y -f meta "$dev2"
rm meta
pvck --dump headers "$dev1" || true
pvck --dump headers "$dev2" || true
vgs $vg
lvcreate -l1 -an $vg

# One PV, one mda, pv_header zeroed, unmatching dev name requires specified uuid
rm meta || true
dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
vgcreate $vg "$dev1"
pvck --dump headers "$dev1" || true
UUID1=`pvck --dump headers "$dev1" | grep pv_header.pv_uuid | awk '{print $2}'`
echo $UUID1
dd if=/dev/zero of="$dev1" bs=512 count=2
pvck --dump headers "$dev1" || true
pvck --dump metadata_search --settings seqno=1 -f meta "$dev1" || true
sed 's/\/dev\/mapper\/LVMTEST/\/dev\/mapper\/BADTEST/' meta > meta.bad
grep device meta
grep device meta.bad
not pvck --repair -y -f meta.bad "$dev1"
pvck --repair -y -f meta.bad --settings pv_uuid=$UUID1 "$dev1"
pvck --dump headers "$dev1" || true
vgs $vg
lvcreate -l1 -an $vg

