# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#
# Exercise various vgextend commands
#

. ./test-utils.sh

aux prepare_devs 5

for mdatype in 1 2
do

# Explicit pvcreate
pvcreate -M$mdatype $dev1 $dev2 $dev3 $dev4 $dev5
vgcreate -M$mdatype $vg1 $dev1 $dev2
vgextend $vg1 $dev3 $dev4 $dev5
vgremove -ff $vg1

# Implicit pvcreate
pvremove $dev1 $dev2 $dev3 $dev4 $dev5
vgcreate -M$mdatype $vg1 $dev1 $dev2
vgextend -M$mdatype $vg1 $dev3 $dev4 $dev5
vgremove -ff $vg1
pvremove $dev1 $dev2 $dev3 $dev4 $dev5

done

# Implicit pvcreate tests, test pvcreate options on vgcreate
# --force, --yes, --metadata{size|copies|type}, --zero
# --dataalignment[offset]
vgcreate $vg $dev2
vgextend --force --yes --zero y $vg $dev1
vgreduce $vg $dev1
pvremove -f $dev1

for i in 0 1 2 3
do
# vgcreate (lvm2) succeeds writing LVM label at sector $i
    vgextend --labelsector $i $vg $dev1
    dd if=$dev1 bs=512 skip=$i count=1 2>/dev/null | strings | grep -q LABELONE;
    vgreduce $vg $dev1
    pvremove -f $dev1
done

# pvmetadatacopies
for i in 0 1 2
do
    vgextend --pvmetadatacopies $i $vg $dev1
    check_pv_field_ $dev1 pv_mda_count $i
    vgreduce $vg $dev1
    pvremove -f $dev1
done

# metadatasize, dataalignment, dataalignmentoffset
#COMM 'pvcreate sets data offset next to mda area'
vgextend --metadatasize 100k --dataalignment 100k $vg $dev1
check_pv_field_ $dev1 pe_start 200.00k
vgreduce $vg $dev1
pvremove -f $dev1

# data area is aligned to 64k by default,
# data area start is shifted by the specified alignment_offset
pv_align="195.50k"
vgextend --metadatasize 128k --dataalignmentoffset 7s $vg $dev1
check_pv_field_ $dev1 pe_start $pv_align
vgremove -f $vg
pvremove -f $dev1

