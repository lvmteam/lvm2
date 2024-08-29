#!/bin/sh
# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
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

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1
SKIP_WITH_CLVMD=1

. lib/inittest

# Unsupported with valgrind testing
test "${LVM_VALGRIND:-0}" -eq 0 || skip "Unsupported with valgrind"

# NOTE: Some tests, namely anything with vdo, and
# api/dbus_test_lv_interface_cache_lv.sh, require larger PVs
aux prepare_pvs 6 6400

# Required by test_nesting:
aux extend_filter_LVMTEST

# We need the lvmdbusd.profile for the daemon to utilize JSON
# output
aux prepare_profiles "lvmdbusd"

# Keep generating test file within test dir
export TMPDIR=$PWD
aux prepare_lvmdbusd

# Example for testing individual test:
#"$TESTOLDPWD/dbus/lvmdbustest.py" -v TestDbusService.test_lv_interface_cache_lv
#"$TESTOLDPWD/dbus/lvmdbustest.py" -v TestDbusService.test_pv_symlinks

"$TESTOLDPWD/dbus/lvmdbustest.py" -v
