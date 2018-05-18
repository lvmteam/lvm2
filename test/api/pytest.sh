#!/bin/bash
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
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMETAD=1
SKIP_WITH_CLVMD=1

. lib/inittest

aux prepare_dmeventd

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
if [[ -n "${abs_top_builddir+varset}" ]]; then
  # For python2 look for  lvm.so, python3 uses some lengthy names
  case "$(head -1 $(which python_lvm_unit.py) )" in
  *2) python_lib=($(find "$abs_top_builddir" -name lvm.so)) ;;
  *) python_lib=($(find "$abs_top_builddir" -name lvm*gnu.so)) ;;
  esac
  if [[ ${#python_lib[*]} -ne 1 ]]; then
    if [[ ${#python_lib[*]} -gt 1 ]]; then
      # Unable to test python bindings if multiple libraries found:
      echo "Found left over lvm.so: ${python_lib[*]}"
      false
    else
      # Unable to test python bindings if library not available
      skip "lvm2-python-libs not built"
    fi
  fi

  PYTHONPATH=$(dirname "${python_lib[*]}"):${PYTHONPATH-}
  export PYTHONPATH
elif rpm -q lvm2-python-libs &>/dev/null; then
  true
else
  skip "lvm2-python-libs neither built nor installed"
fi

#If you change this change the unit test case too.
aux prepare_pvs 6

#Setup which devices the unit test can use.
PY_UNIT_PVS=$(cat DEVICES)
export PY_UNIT_PVS

#When needed to run 1 single individual python test
#python_lvm_unit.py -v -f TestLvm.test_lv_persistence
#exit

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
