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

aux prepare_devs 4
get_devs

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true
dd if=/dev/zero of="$dev4" || true

pvcreate "$dev1"
pvcreate "$dev2"
pvcreate --pvmetadatacopies 2 "$dev3"
pvcreate --pvmetadatacopies 0 "$dev4"

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

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

