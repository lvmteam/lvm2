#!/bin/sh
# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Test pvcreate option values'
PAGESIZE=$(getconf PAGESIZE)

. lib/test

aux prepare_devs 4

#COMM 'pvcreate rejects negative setphysicalvolumesize'
not pvcreate --setphysicalvolumesize -1024 $dev1

#COMM 'pvcreate rejects negative metadatasize'
not pvcreate --metadatasize -1024 $dev1

# x. metadatasize 0, defaults to 255
# FIXME: unable to check default value, not in reporting cmds
# should default to 255 according to code
#   check pv_field pv_mda_size 255 
#COMM 'pvcreate accepts metadatasize 0'
pvcreate --metadatasize 0 $dev1
pvremove $dev1

#Verify vg_mda_size is smaller pv_mda_size
pvcreate --metadatasize 512k $dev1
pvcreate --metadatasize 96k $dev2
vgcreate $vg $dev1 $dev2
check compare_fields vgs $vg vg_mda_size pvs $dev2 pv_mda_size
vgremove -ff $vg

# x. metadatasize too large
# For some reason we allow this, even though there's no room for data?
##COMM  'pvcreate rejects metadatasize too large' 
#not pvcreate --metadatasize 100000000000000 $dev1

#COMM 'pvcreate rejects metadatacopies < 0'
not pvcreate --metadatacopies -1 $dev1

#COMM 'pvcreate accepts metadatacopies = 0, 1, 2'
for j in metadatacopies pvmetadatacopies
do
pvcreate --$j 0 $dev1
pvcreate --$j 1 $dev2
pvcreate --$j 2 $dev3
check pv_field $dev1 pv_mda_count 0
check pv_field $dev2 pv_mda_count 1
check pv_field $dev3 pv_mda_count 2
pvremove $dev1 
pvremove $dev2 
pvremove $dev3
done

#COMM 'pvcreate rejects metadatacopies > 2'
not pvcreate --metadatacopies 3 $dev1

#COMM 'pvcreate rejects invalid device'
not pvcreate $dev1bogus

#COMM 'pvcreate rejects labelsector < 0'
not pvcreate --labelsector -1 $dev1

#COMM 'pvcreate rejects labelsector > 1000000000000'
not pvcreate --labelsector 1000000000000 $dev1

# other possibilites based on code inspection (not sure how hard)
# x. device too small (min of 512 * 1024 KB)
# x. device filtered out
# x. unable to open /dev/urandom RDONLY
# x. device too large (pe_count > UINT32_MAX)
# x. device read-only
# x. unable to open device readonly
# x. BLKGETSIZE64 fails
# x. set size to value inconsistent with device / PE size

#COMM 'pvcreate basic dataalignment sanity checks'
not pvcreate --dataalignment -1 $dev1
not pvcreate -M 1 --dataalignment 1 $dev1
not pvcreate --dataalignment 1e $dev1

#COMM 'pvcreate always rounded up to page size for start of device'
#pvcreate --metadatacopies 0 --dataalignment 1 $dev1
# amuse shell experts
#check pv_field $dev1 pe_start $(($(getconf PAGESIZE)/1024))".00k"

#COMM 'pvcreate sets data offset directly'
pvcreate --dataalignment 512k $dev1
check pv_field $dev1 pe_start 512.00k

#COMM 'vgcreate/vgremove do not modify data offset of existing PV'
vgcreate $vg $dev1  --config 'devices { data_alignment = 1024 }'
check pv_field $dev1 pe_start 512.00k
vgremove $vg --config 'devices { data_alignment = 1024 }'
check pv_field $dev1 pe_start 512.00k

#COMM 'pvcreate sets data offset next to mda area'
pvcreate --metadatasize 100k --dataalignment 100k $dev1
check pv_field $dev1 pe_start 200.00k

# metadata area start is aligned according to pagesize
if [ $PAGESIZE -eq 65536 ] ; then
	pv_align="192.50k"
elif [ $PAGESIZE -eq 8192 ] ; then
	pv_align="136.50k"
else
	pv_align="133.00k"
fi

pvcreate --metadatasize 128k --dataalignment 3.5k $dev1
check pv_field $dev1 pe_start $pv_align

pvcreate --metadatasize 128k --metadatacopies 2 --dataalignment 3.5k $dev1
check pv_field $dev1 pe_start $pv_align

# data area is aligned to 1M by default,
# data area start is shifted by the specified alignment_offset
pv_align="1052160B" # 1048576 + (7*512)
pvcreate --metadatasize 128k --dataalignmentoffset 7s $dev1
check pv_field $dev1 pe_start $pv_align "--units b"

# 2nd metadata area is created without problems when
# data area start is shifted by the specified alignment_offset
pvcreate --metadatasize 128k --metadatacopies 2 --dataalignmentoffset 7s $dev1
check pv_field $dev1 pv_mda_count 2
# FIXME: compare start of 2nd mda with and without --dataalignmentoffset

#COMM 'pv with LVM1 compatible data alignment can be convereted'
#compatible == LVM1_PE_ALIGN == 64k
pvcreate --dataalignment 256k $dev1
vgcreate -s 1m $vg $dev1
vgconvert -M1 $vg
vgconvert -M2 $vg
check pv_field $dev1 pe_start 256.00k
vgremove $vg

#COMM 'pv with LVM1 incompatible data alignment cannot be convereted'
pvcreate --dataalignment 10k $dev1
vgcreate -s 1m $vg $dev1
not vgconvert -M1 $vg
vgremove $vg

#COMM 'vgcfgrestore allows pe_start=0'
#basically it produces nonsense, but it tests vgcfgrestore,
#not that final cfg is usable...
pvcreate --metadatacopies 0 $dev1
pvcreate $dev2
vgcreate $vg $dev1 $dev2
vgcfgbackup -f "$(pwd)/backup.$$" $vg
sed 's/pe_start = [0-9]*/pe_start = 0/' "$(pwd)/backup.$$" > "$(pwd)/backup.$$1"
vgcfgrestore -f "$(pwd)/backup.$$1" $vg
check pv_field $dev1 pe_start 0
check pv_field $dev2 pe_start 0
vgremove $vg

echo test pvcreate --metadataignore
for pv_in_vg in 1 0; do
for mdacp in 1 2; do
for ignore in y n; do
	echo pvcreate --metadataignore has proper mda_count and mda_used_count
	pvcreate --metadatacopies $mdacp --metadataignore $ignore $dev1 $dev2
	check pv_field $dev1 pv_mda_count $mdacp
	check pv_field $dev2 pv_mda_count $mdacp
	if [ $ignore = y ]; then
		check pv_field $dev1 pv_mda_used_count 0
		check pv_field $dev2 pv_mda_used_count 0
	else
		check pv_field $dev1 pv_mda_used_count $mdacp
		check pv_field $dev2 pv_mda_used_count $mdacp
	fi
	echo vgcreate has proper vg_mda_count and vg_mda_used_count
	if [ $pv_in_vg = 1 ]; then
		vgcreate -c n "$vg" $dev1 $dev2
		check vg_field $vg vg_mda_count $(($mdacp * 2))
		if [ $ignore = y ]; then
			check vg_field $vg vg_mda_used_count 1
		else
			check vg_field $vg vg_mda_used_count $(($mdacp * 2))
		fi
		check vg_field $vg vg_mda_copies unmanaged
		vgremove $vg
	fi
done
done
done
