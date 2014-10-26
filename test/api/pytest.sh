#!/bin/sh
# Copyright (C) 2012-2013 Red Hat, Inc. All rights reserved.
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
python_lvm_unit.py -v TestLvm.test_config_find_bool
python_lvm_unit.py -v TestLvm.test_config_override
python_lvm_unit.py -v TestLvm.test_config_reload
python_lvm_unit.py -v TestLvm.test_dupe_lv_create
python_lvm_unit.py -v TestLvm.test_get_set_extend_size
python_lvm_unit.py -v TestLvm.test_lv_active_inactive
python_lvm_unit.py -v TestLvm.test_lv_property
python_lvm_unit.py -v TestLvm.test_lv_rename
python_lvm_unit.py -v TestLvm.test_lv_resize
python_lvm_unit.py -v TestLvm.test_lv_seg
python_lvm_unit.py -v TestLvm.test_lv_size
python_lvm_unit.py -v TestLvm.test_lv_snapshot
python_lvm_unit.py -v TestLvm.test_lv_suspend
python_lvm_unit.py -v TestLvm.test_lv_tags
python_lvm_unit.py -v TestLvm.test_percent_to_float
python_lvm_unit.py -v TestLvm.test_pv_create
python_lvm_unit.py -v TestLvm.test_pv_empty_listing
python_lvm_unit.py -v TestLvm.test_pv_getters
python_lvm_unit.py -v TestLvm.test_pv_life_cycle
python_lvm_unit.py -v TestLvm.test_pv_lookup_from_vg
python_lvm_unit.py -v TestLvm.test_pv_property
python_lvm_unit.py -v TestLvm.test_pv_resize
python_lvm_unit.py -v TestLvm.test_pv_segs
python_lvm_unit.py -v TestLvm.test_scan
python_lvm_unit.py -v TestLvm.test_version
python_lvm_unit.py -v TestLvm.test_vg_from_pv_lookups
python_lvm_unit.py -v TestLvm.test_vg_getters
python_lvm_unit.py -v TestLvm.test_vg_get_name
python_lvm_unit.py -v TestLvm.test_vg_get_set_prop
python_lvm_unit.py -v TestLvm.test_vg_get_uuid
python_lvm_unit.py -v TestLvm.test_vg_lv_name_validate
python_lvm_unit.py -v TestLvm.test_vg_names
python_lvm_unit.py -v TestLvm.test_vg_reduce
python_lvm_unit.py -v TestLvm.test_vg_remove_restore
python_lvm_unit.py -v TestLvm.test_vg_tags
python_lvm_unit.py -v TestLvm.test_vg_uuids

# CHECKME: not for testing?
#python_lvm_unit.py -v TestLvm.test_listing
#python_lvm_unit.py -v TestLvm.test_pv_methods
