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
	lvs -oattr "$lv" | grep -q "^ *m.....$" || {
		echo "$lv expected a mirror, but is not:"
		lvs -a $lv
		exit 1
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
	lvl -ostripes "$lv" | grep -q "1" || {
		echo "$lv expected linear, but is not:"
		lvl "$lv" -o+devices
		exit 1
	}
}

active() {
	lv="$1/$2"
	lvl -oattr "$lv" 2> /dev/null | grep -q "^ *....a.$" || {
		echo "$lv expected active, but lvs says it's not:"
		lvl "$lv" -o+devices 2>/dev/null
		exit 1
	}
	dmsetup table | egrep -q "$1-$2: *[^ ]+" || {
		echo "$lv expected active, lvs thinks it is but there are no mappings!"
		dmsetup table | grep $1-$2:
		exit 1
	}
}

inactive() {
	lv="$1/$2"
	lvl -oattr "$lv" 2> /dev/null | grep -q '^ *....[-isd].$' || {
		echo "$lv expected inactive, but lvs says it's not:"
		lvl "$lv" -o+devices 2>/dev/null
		exit 1
	}
	dmsetup table | not egrep -q "$1-$2: *[^ ]+" || {
		echo "$lv expected inactive, lvs thinks it is but there are mappings!"
		dmsetup table | grep $1-$2:
		exit 1
	}
}

"$@"
