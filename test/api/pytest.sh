#!/bin/sh
# Copyright (C) 2012-2015 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

. lib/inittest

#
# TODO:
# lvm2app is not yet capable to respect many lvm.conf options
# since a lot of them is set in /tools/liblvmline
# Until fixed - testing always runs with enabled monitoring
# thus it needs dmeventd
#

# Example of using 'gdb' with python:
# gdb -ex r --args python FULL_PATH/lvm2/test/api/python_lvm_unit.py -v TestLvm.test_lv_active_inactive

#Locate the python binding library to use.
python_lib=$(find $abs_top_builddir -name lvm.so)
# Unable to test python bindings if library not available
test -n "$python_lib" || skip

test -e LOCAL_CLVMD && skip
test -e LOCAL_LVMETAD && skip

aux prepare_dmeventd

#If you change this change the unit test case too.
aux prepare_pvs 6

export PYTHONPATH=$(dirname $python_lib):$PYTHONPATH

#Setup which devices the unit test can use.
export PY_UNIT_PVS=$(cat DEVICES)

#python_lvm_unit.py -v -f

# Run individual tests for shorter error trace
for i in \
 lv_persistence \
 config_find_bool \
 config_override \
 config_reload  \
 dupe_lv_create \
 get_set_extend_size \
 lv_active_inactive \
 lv_property \
 lv_rename \
 lv_resize \
 lv_seg \
 lv_size \
 lv_snapshot \
 lv_suspend \
 lv_tags \
 percent_to_float \
 pv_create \
 pv_empty_listing \
 pv_getters \
 pv_life_cycle \
 pv_lookup_from_vg \
 pv_property \
 pv_resize \
 pv_segs \
 scan \
 version \
 vg_from_pv_lookups \
 vg_getters \
 vg_get_name \
 vg_get_set_prop \
 vg_get_uuid \
 vg_lv_name_validate \
 vg_names \
 vg_reduce \
 vg_remove_restore \
 vg_tags \
 vg_uuids
do
	python_lvm_unit.py -v TestLvm.test_$i
	rm -f debug.log_DEBUG*
done

# CHECKME: not for testing?
#python_lvm_unit.py -v TestLvm.test_listing
#python_lvm_unit.py -v TestLvm.test_pv_methods
