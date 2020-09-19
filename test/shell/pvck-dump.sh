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

SIZE_MB=80
# 4 devs each $SIZE_MB
aux prepare_devs 4 $SIZE_MB
get_devs

dd if=/dev/zero of="$dev1" bs=1M count=2 oflag=direct
dd if=/dev/zero of="$dev2" bs=1M count=2 oflag=direct
# clear entire dev to cover mda2
dd if=/dev/zero of="$dev3" bs=1M count=$SIZE_MB oflag=direct
dd if=/dev/zero of="$dev4" bs=1M count=2 oflag=direct

pvcreate "$dev1"
pvcreate "$dev2"
pvcreate --pvmetadatacopies 2 "$dev3"
pvcreate --pvmetadatacopies 0 "$dev4"

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

pvs

pvck --dump headers "$dev1" > h1
pvck --dump headers "$dev2" > h2
pvck --dump headers "$dev3" > h3
pvck --dump headers "$dev4" > h4

grep "label_header at 512" h1
grep "label_header at 512" h2
grep "label_header at 512" h3
grep "label_header at 512" h4

grep "pv_header at 544" h1
grep "pv_header at 544" h2
grep "pv_header at 544" h3
grep "pv_header at 544" h4

grep "pv_header.disk_locn\[0\].offset 1048576" h1
grep "pv_header.disk_locn\[0\].offset 1048576" h2
grep "pv_header.disk_locn\[0\].offset 1048576" h3

grep "pv_header.disk_locn\[2\].offset 4096" h1
grep "pv_header.disk_locn\[2\].offset 4096" h2
grep "pv_header.disk_locn\[2\].offset 4096" h3

grep "pv_header.disk_locn\[2\].size 1044480" h1
grep "pv_header.disk_locn\[2\].size 1044480" h2
grep "pv_header.disk_locn\[2\].size 1044480" h3

not grep "pv_header.disk_locn\[3\].size" h4
not grep "pv_header.disk_locn\[4\].size" h4
not grep "mda_header" h4

grep "mda_header_1 at 4096" h1
grep "mda_header_1 at 4096" h2
grep "mda_header_1 at 4096" h3

grep "mda_header_1.start 4096" h1
grep "mda_header_1.start 4096" h2
grep "mda_header_1.start 4096" h3

grep "mda_header_1.size 1044480" h1
grep "mda_header_1.size 1044480" h2
grep "mda_header_1.size 1044480" h3

grep "mda_header_2 at " h3
grep "mda_header_2.start " h3

grep "metadata text at " h1
grep "metadata text at " h2
grep "metadata text at " h3

not grep CHECK h1
not grep CHECK h2
not grep CHECK h3

pvck --dump metadata "$dev1" > m1
pvck --dump metadata "$dev2" > m2
pvck --dump metadata "$dev3" > m3
pvck --dump metadata "$dev4" > m4
pvck --dump metadata --pvmetadatacopies 2 "$dev3" > m3b

grep "zero metadata copies" m4

diff m1 m2
diff m1 m3

not diff m1 m3b > tmp
grep "metadata text at" tmp

lvcreate -an -l1 $vg

pvck --dump metadata_all -f all1 "$dev1" > out1
pvck --dump metadata_all -f all2 "$dev2" > out2
pvck --dump metadata_all -f all3 "$dev3" > out3
pvck --dump metadata_all --pvmetadatacopies 2 -f all3b "$dev3" > out3b

diff out1 out2
diff out1 out3

grep "seqno 1" out1
grep "seqno 1" out3b
grep "seqno 2" out1
grep "seqno 2" out3b

diff all1 all2
diff all1 all3
diff all1 all3b

grep "seqno = 1" all1
grep "seqno = 2" all1


pvck --dump metadata_area -f area1 "$dev1"
pvck --dump metadata_area -f area2 "$dev2"
pvck --dump metadata_area -f area3 "$dev3"
pvck --dump metadata_area -f area3b "$dev3"

diff area1 area2
diff area1 area3
diff area1 area3b

vgremove -ff $vg


# clear entire dev to cover mda2
dd if=/dev/zero of="$dev1" bs=1M count=$SIZE_MB oflag=direct
dd if=/dev/zero of="$dev2" bs=1M count=32 oflag=direct
dd if=/dev/zero of="$dev3" bs=1M count=32 oflag=direct
dd if=/dev/zero of="$dev4" bs=1M count=32 oflag=direct

pvcreate --pvmetadatacopies 2 --metadatasize 32M "$dev1"

vgcreate $SHARED -s 64K --metadatasize 32M $vg "$dev1" "$dev2" "$dev3" "$dev4"

for i in $(seq 1 500); do echo "lvcreate -an -n lv$i -l1 $vg"; done | lvm

pvck --dump headers "$dev1" > h1

pvck --dump metadata_search "$dev1" > m1
grep "seqno 500" m1

# When metadatasize is 32M, headers/rounding can mean that
# we need more than the first 32M of the dev to get all the
# metadata.
dd if="$dev1" of=dev1dd bs=1M count=34

# Clear the header so that we force metadata_search to use
# the settings instead of getting the mda_size/mda_offset
# from the headers.
dd if=/dev/zero of="$dev1" bs=4K count=1 oflag=direct

# Warning: these checks are based on copying specific numbers
# seen when running these commands, but these numbers could
# change as side effects of other things.  That makes this
# somewhat fragile, and we might want to remove some of the
# these checks if they are hard to keep working.

# by experimentation, mda_size for mda1 is 34598912
pvck --dump metadata_search --settings "mda_num=1 mda_size=34598912" "$dev1" > m1b
# by experimentation, metadata 484 is the last in the mda1 buffer
grep "seqno 484" m1b
# by experimentation, metadata 485 is the last in the mda1 buffer
grep "seqno 485" m1b
grep "seqno 500" m1b

# same results when using file as on device
pvck --dump metadata_search --settings "mda_num=1 mda_size=34598912" dev1dd > m1c
# by experimentation, metadata 484 is the last in the mda1 buffer
grep "seqno 484" m1b
# by experimentation, metadata 485 is the last in the mda1 buffer
grep "seqno 485" m1b
grep "seqno 500" m1b

# by experimentation, mda_size for mda2 is 33554432
pvck --dump metadata_search --settings "mda_num=2 mda_size=33554432" "$dev1" > m2
# by experimentation, metadata 477 is the last in the mda2 buffer
grep "seqno 477" m1b
# by experimentation, metadata 478 is the last in the mda2 buffer
grep "seqno 478" m1b
grep "seqno 500" m2

dd if=dev1dd of="$dev1" bs=4K count=1 oflag=direct

vgremove -ff $vg
