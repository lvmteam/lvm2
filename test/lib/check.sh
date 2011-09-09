#!/bin/bash

# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# check.sh: assert various things about volumes

# USAGE
#  check linear VG LV
#  check lv_on VG LV PV

#  check mirror VG LV [LOGDEV|core]
#  check mirror_nonredundant VG LV
#  check mirror_legs VG LV N
#  check mirror_images_on VG LV DEV [DEV...]

# ...

set -e -o pipefail

trim()
{
    trimmed=${1%% }
    trimmed=${trimmed## }

    echo "$trimmed"
}

lvl() {
	lvs -a --noheadings "$@"
}

lvdevices() {
	lvl -odevices "$@" | sed 's/([^)]*)//g; s/,/ /g'
}

mirror_images_redundant()
{
  vg=$1
  lv=$vg/$2

  lvs -a $vg -o+devices
  for i in `lvdevices $lv`; do
	  echo "# $i:"
	  lvdevices $vg/$i | sort | uniq
  done > check.tmp.all

  (grep -v ^# check.tmp.all || true) | sort | uniq -d > check.tmp

  test "`cat check.tmp | wc -l`" -eq 0 || {
	  echo "mirror images of $lv expected redundant, but are not:"
	  cat check.tmp.all
	  exit 1
  }
}

mirror_images_on() {
	vg=$1
	lv=$2

	shift 2

	for i in `lvdevices $lv`; do
		lv_on $vg $lv $1
		shift
	done
}

lv_on()
{
	lv="$1/$2"
	lvdevices $lv | grep -F "$3" || {
		echo "LV $lv expected on $3 but is not:" >&2
		lvdevices $lv >&2
		exit 1
	}
	test `lvdevices $lv | grep -vF "$3" | wc -l` -eq 0 || {
		echo "LV $lv contains unexpected devices:" >&2
		lvdevices $lv >&2
		exit 1
	}
}

mirror_log_on()
{
	vg="$1"
	lv="$2"
	where="$3"
	if test "$where" = "core"; then
		lvl -omirror_log "$vg/$lv" | not grep mlog
	else
		lv_on $vg "${lv}_mlog" "$where"
	fi
}

lv_is_contiguous()
{
	test `lvl --segments $1 | wc -l` -eq 1 || {
		echo "LV $1 expected to be contiguous, but is not:"
		lvl --segments $1
		exit 1
	}
}

lv_is_clung()
{
	test `lvdevices $1 | sort | uniq | wc -l` -eq 1 || {
		echo "LV $1 expected to be clung, but is not:"
		lvdevices $! | sort | uniq
		exit 1
	}
}

mirror_images_contiguous()
{
	for i in `lvdevices $1/$2`; do
		lv_is_contiguous $1/$i
	done
}

mirror_images_clung()
{
	for i in `lvdevices $1/$2`; do
		lv_is_clung $1/$i
	done
}

mirror() {
	mirror_nonredundant "$@"
	mirror_images_redundant "$1" "$2"
}

mirror_nonredundant() {
	lv="$1/$2"
	lvs -oattr "$lv" | grep "^ *m.......$" >/dev/null || {
		if lvs -oattr "$lv" | grep "^ *o.......$" >/dev/null &&
		   lvs -a | fgrep "[${2}_mimage" >/dev/null; then
			echo "TEST WARNING: $lv is a snapshot origin and looks like a mirror,"
			echo "assuming it is actually a mirror"
		else
			echo "$lv expected a mirror, but is not:"
			lvs -a $lv
			exit 1
		fi
	}
	if test -n "$3"; then mirror_log_on "$1" "$2" "$3"; fi
}

mirror_legs() {
	lv="$1/$2"
	expect="$3"
	lvdevices "$lv"
	real=`lvdevices "$lv" | wc -w`
	test "$expect" = "$real"
}

mirror_no_temporaries()
{
	vg=$1
	lv=$2
	lvl -oname $vg | grep $lv | not grep "tmp" || {
		echo "$lv has temporary mirror images unexpectedly:"
		lvl $vg | grep $lv
		exit 1
	}
}

linear() {
	lv="$1/$2"
	lvl -ostripes "$lv" | grep "1" >/dev/null || {
		echo "$lv expected linear, but is not:"
		lvl "$lv" -o+devices
		exit 1
	}
}

active() {
	lv="$1/$2"
	lvl -oattr "$lv" 2> /dev/null | grep "^ *....a...$" >/dev/null || {
		echo "$lv expected active, but lvs says it's not:"
		lvl "$lv" -o+devices 2>/dev/null
		exit 1
	}
	dmsetup table | egrep "$1-$2: *[^ ]+" >/dev/null || {
		echo "$lv expected active, lvs thinks it is but there are no mappings!"
		dmsetup table | grep $1-$2:
		exit 1
	}
}

inactive() {
	lv="$1/$2"
	lvl -oattr "$lv" 2> /dev/null | grep '^ *....[-isd]...$' >/dev/null || {
		echo "$lv expected inactive, but lvs says it's not:"
		lvl "$lv" -o+devices 2>/dev/null
		exit 1
	}
	dmsetup table | not egrep "$1-$2: *[^ ]+" >/dev/null || {
		echo "$lv expected inactive, lvs thinks it is but there are mappings!"
		dmsetup table | grep $1-$2:
		exit 1
	}
}

lv_exists() {
	lv="$1/$2"
	lvl "$lv" >& /dev/null || {
		echo "$lv expected to exist but does not"
		exit 1
	}
}

pv_field()
{
    actual=$(trim $(pvs --noheadings $4 -o $2 $1))

    test "$actual" = "$3" || {
        echo "pv_field: PV=$1, field=$2, actual=$actual, expected=$3"
        exit 1
    }
}

vg_field()
{
    actual=$(trim $(vgs --noheadings $4 -o $2 $1))
    test "$actual" = "$3" || {
        echo "vg_field: vg=$1, field=$2, actual=$actual, expected=$3"
        exit 1
    }
}

lv_field()
{
    actual=$(trim $(lvs --noheadings $4 -o $2 $1))
    test "$actual" = "$3" || {
        echo "lv_field: lv=$1, field=$2, actual=$actual, expected=$3"
        exit 1
    }
}

compare_fields()
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
    test "$val1" = "$val2" || {
        echo "compare_fields $obj1($field1): $val1 $obj2($field2): $val2"
        exit 1
    }
}

compare_vg_field()
{
    local vg1=$1;
    local vg2=$2;
    local field=$3;

    val1=$(vgs --noheadings -o $field $vg1)
    val2=$(vgs --noheadings -o $field $vg2)
    test "$val1" = "$val2" || {
        echo "compare_vg_field: $vg1: $val1, $vg2: $val2"
        exit 1
    }
}

pvlv_counts()
{
	local local_vg=$1
	local num_pvs=$2
	local num_lvs=$3
	local num_snaps=$4

	lvs -a -o+devices $local_vg

	vg_field $local_vg pv_count $num_pvs
	vg_field $local_vg lv_count $num_lvs
	vg_field $local_vg snap_count $num_snaps
}

"$@"
