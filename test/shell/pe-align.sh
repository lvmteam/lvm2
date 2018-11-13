
#!/usr/bin/env bash

# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Test pe alignment and metadata sizes'
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 1

# values depend on page size 4K

# In order of strength:
# --dataalignmentoffset                    (modifies all below)
# --dataalignment                          (overrides all below)
# devices/data_alignment                   (overrides all below)
# devices/data_alignment_offset_detection  (overrides all below)
# devices/md_chunk_alignment               (overrides all below)
# devices/default_data_alignment

pvcreate "$dev1"
check pv_field "$dev1" pe_start 1.00m
check pv_field "$dev1" mda_size 1020.00k
pvremove "$dev1"

# default align at 1m is effective even with smaller requested metadata
pvcreate --metadatasize 100k "$dev1"
check pv_field "$dev1" pe_start 1.00m
check pv_field "$dev1" mda_size 1020.00k
pvremove "$dev1"

# default first pe doesn't depend on on these two settings
pvcreate --config 'devices {default_data_alignment=0 data_alignment=0}' "$dev1"
check pv_field "$dev1" pe_start 1.00m
check pv_field "$dev1" mda_size 1020.00k
pvremove "$dev1"

# same as previous
pvcreate --config 'devices {default_data_alignment=1 data_alignment=0}' "$dev1"
check pv_field "$dev1" pe_start 1.00m
check pv_field "$dev1" mda_size 1020.00k
pvremove "$dev1"

# same as previous
pvcreate --config 'devices {default_data_alignment=0 data_alignment=1024}' "$dev1"
check pv_field "$dev1" pe_start 1.00m
check pv_field "$dev1" mda_size 1020.00k
pvremove "$dev1"

# same as previous
pvcreate --config 'devices {default_data_alignment=1 data_alignment=1024}' "$dev1"
check pv_field "$dev1" pe_start 1.00m
check pv_field "$dev1" mda_size 1020.00k
pvremove "$dev1"

# combine above
pvcreate --metadatasize 100k --config 'devices {default_data_alignment=0 data_alignment=0}' "$dev1"
check pv_field "$dev1" pe_start 1.00m
check pv_field "$dev1" mda_size 1020.00k
pvremove "$dev1"

pvcreate --metadatasize 2048k "$dev1"
check pv_field "$dev1" pe_start 3072.00k --units k
check pv_field "$dev1" mda_size 3068.00k --units k
pvremove "$dev1"

pvcreate --metadatasize 2044k "$dev1"
check pv_field "$dev1" pe_start 2048.00k --units k
check pv_field "$dev1" mda_size 2044.00k --units k
pvremove "$dev1"

pvcreate --metadatasize 2048k --config 'devices {default_data_alignment=2}' "$dev1"
check pv_field "$dev1" pe_start 4.00m
check pv_field "$dev1" mda_size 4092.00k --units k
pvremove "$dev1"

pvcreate --metadatasize 100k --config 'devices {default_data_alignment=2}' "$dev1"
check pv_field "$dev1" pe_start 2048.00k --units k
check pv_field "$dev1" mda_size 2044.00k --units k
pvremove "$dev1"

pvcreate --metadatasize 2048k --dataalignment 128k "$dev1"
check pv_field "$dev1" pe_start 2176.00k --units k
check pv_field "$dev1" mda_size 2172.00k --units k
pvremove "$dev1"

pvcreate --metadatasize 2048k --dataalignment 128k --dataalignmentoffset 2k "$dev1"
check pv_field "$dev1" pe_start 2178.00k --units k
check pv_field "$dev1" mda_size 2174.00k --units k
pvremove "$dev1"

pvcreate --metadatasize 2048k --dataalignment 128k --config 'devices {default_data_alignment=0}' "$dev1"
check pv_field "$dev1" pe_start 2176.00k --units k
check pv_field "$dev1" mda_size 2172.00k --units k
pvremove "$dev1"

pvcreate --metadatasize 2048k --dataalignment 128k --config 'devices {default_data_alignment=2}' "$dev1"
check pv_field "$dev1" pe_start 2176.00k --units k
check pv_field "$dev1" mda_size 2172.00k --units k
pvremove "$dev1"

pvcreate --metadatasize 2048k --config 'devices {default_data_alignment=2 data_alignment=128}' "$dev1"
check pv_field "$dev1" pe_start 2176.00k --units k
check pv_field "$dev1" mda_size 2172.00k --units k
pvremove "$dev1"

pvcreate --bootloaderareasize 256k "$dev1"
check pv_field "$dev1" mda_size 1020.00k --units k
check pv_field "$dev1" ba_start 1024.00k --units k
check pv_field "$dev1" ba_size  1024.00k --units k
check pv_field "$dev1" pe_start 2048.00k --units k
pvremove "$dev1"

pvcreate --dataalignment 128k --bootloaderareasize 256k "$dev1"
check pv_field "$dev1" mda_size 1020.00k --units k
check pv_field "$dev1" ba_start 1024.00k --units k
check pv_field "$dev1" ba_size  256.00k --units k
check pv_field "$dev1" pe_start 1280.00k --units k
pvremove "$dev1"

pvcreate --dataalignment 128k --metadatasize 256k "$dev1"
check pv_field "$dev1" mda_size 380.00k --units k
check pv_field "$dev1" ba_start 0k --units k
check pv_field "$dev1" ba_size  0k --units k
check pv_field "$dev1" pe_start 384.00k --units k
pvremove "$dev1"

pvcreate --dataalignment 128k --metadatasize 256k --bootloaderareasize 256k "$dev1"
check pv_field "$dev1" mda_size 380.00k --units k
check pv_field "$dev1" ba_start 384.00k --units k
check pv_field "$dev1" ba_size  256.00k --units k
check pv_field "$dev1" pe_start 640.00k --units k
pvremove "$dev1"

