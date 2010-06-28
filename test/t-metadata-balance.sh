# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. ./test-utils.sh

aux prepare_devs 5

#  Make sure we can ignore / un-ignore mdas on a per-PV basis
for pv_in_vg in 1 0; do
for mdacp in 1 2; do
	pvcreate --metadatacopies $mdacp $dev1 $dev2
        pvcreate --metadatacopies 0 $dev3
	if [ $pv_in_vg = 1 ]; then
		vgcreate -c n "$vg" $dev1 $dev2 $dev3
	fi
	pvchange --metadataignore y $dev1
	check_pv_field_ $dev1 pv_mda_count $mdacp
	check_pv_field_ $dev1 pv_mda_used_count 0
	check_pv_field_ $dev2 pv_mda_count $mdacp
	check_pv_field_ $dev2 pv_mda_used_count $mdacp
	if [ $pv_in_vg = 1 ]; then
		check_vg_field_ $vg vg_mda_count $(($mdacp * 2))
		check_vg_field_ $vg vg_mda_used_count $mdacp
	fi
	pvchange --metadataignore n $dev1
	check_pv_field_ $dev1 pv_mda_count $mdacp
	check_pv_field_ $dev1 pv_mda_used_count $mdacp
	if [ $pv_in_vg = 1 ]; then
		check_vg_field_ $vg vg_mda_count $(($mdacp * 2))
		check_vg_field_ $vg vg_mda_used_count $(($mdacp * 2))
		vgremove -f $vg
	fi
done
done
