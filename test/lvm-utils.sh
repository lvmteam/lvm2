# Put lvm-related utilities here.
# This file is sourced from test-lib.sh.

# Copyright (C) 2007, 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

export LVM_SUPPRESS_FD_WARNINGS=1

ME=$(basename "$0")
warn() { echo >&2 "$ME: $@"; }

trim()
{
    trimmed=${1%% }
    trimmed=${trimmed## }

    echo "$trimmed"
}

compare_two_fields_()
{
    local cmd1=$1;
    local obj1=$2;
    local field1=$3;
    local cmd2=$4;
    local obj2=$5;
    local field2=$6;
    local val1;
    local val2;

    val1=$($cmd1 --noheadings -o $field1 $obj1)
    val2=$($cmd2 --noheadings -o $field2 $obj2)
if test "$verbose" = "t"
then
  echo "compare_two_fields_ $obj1($field1): $val1 $obj2($field2): $val2"
fi
  test "$val1" = "$val2"
}

compare_vg_field_()
{
    local vg1=$1;
    local vg2=$2;
    local field=$3;
    local val1;
    local val2;

    val1=$(vgs --noheadings -o $field $vg1)
    val2=$(vgs --noheadings -o $field $vg2)
if test "$verbose" = "t"
then
  echo "compare_vg_field_ VG1: $val1 VG2: $val2"
fi
  test "$val1" = "$val2"
}


get_pv_field() {
	local pv=$1
	local field=$2
	local value
	pvs --noheading -o $field $pv | sed 's/^ *//'
}

get_vg_field() {
	local vg=$1
	local field=$2
	local value
	vgs --noheading -o $field $vg | sed 's/^ *//'
}

get_lv_field() {
	local lv=$1
	local field=$2
	local value
	lvs --noheading -o $field $lv | sed 's/^ *//'
}

check_vg_field_()
{
    local vg=$1;
    local field=$2;
    local expected=$3;
    local actual;

    actual=$(trim $(vgs --noheadings -o $field $vg))
if test "$verbose" = "t"
then
  echo "check_vg_field_ VG=$vg, field=$field, actual=$actual, expected=$expected"
fi
  test "$actual" = "$expected"
}

check_pv_field_()
{
    local pv=$1;
    local field=$2;
    local expected=$3;
    local pvs_args=$4; # optional
    local actual;

    actual=$(trim $(pvs --noheadings $pvs_args -o $field $pv))
if test "$verbose" = "t"
then
  echo "check_pv_field_ PV=$pv, field=$field, actual=$actual, expected=$expected"
fi
    test "$actual" = "$expected"
}

check_lv_field_()
{
    local lv=$1;
    local field=$2;
    local expected=$3;
    local actual;

    actual=$(trim $(lvs --noheadings -o $field $lv))
if test "$verbose" = "t"
then
  echo "check_lv_field_ LV=$lv, field=$field, actual=$actual, expected=$expected"
fi
  test "$actual" = "$expected"
}

vg_validate_pvlv_counts_()
{
	local local_vg=$1
	local num_pvs=$2
	local num_lvs=$3
	local num_snaps=$4

	lvs -a -o+devices $local_vg

	check_vg_field_ $local_vg pv_count $num_pvs && \
	  check_vg_field_ $local_vg lv_count $num_lvs && \
	  check_vg_field_ $local_vg snap_count $num_snaps
}

dmsetup_has_dm_devdir_support_()
{
  # Detect support for the envvar.  If it's supported, the
  # following command will fail with the expected diagnostic.
  out=$(DM_DEV_DIR=j dmsetup version 2>&1)
  test "$?:$out" = "1:Invalid DM_DEV_DIR envvar value." -o \
       "$?:$out" = "1:Invalid DM_DEV_DIR environment variable value."
}
