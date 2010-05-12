#!/bin/bash

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

  lvs -a $vg
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
	lv=$1

	for i in `lvdevices $lv`; do
		shift
		lv_on $lv $1
	done
}

lv_on()
{
	lv="$1"
	lvdevices $lv | grep -F "$2" || {
		echo "LV $lv expected on $2 but is not:" >&2
		lvdevices $lv >&2
		exit 1
	}
	test `lvdevices $lv | grep -vF "$2" | wc -l` -eq 0 || {
		echo "LV $lv contains unexpected devices:" >&2
		lvdevices $lv >&2
		exit 1
	}
}

mirror_log_on()
{
	lv_on "${1}_mlog" "$2"
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
	lv="$1/$2"
	lvl -oattr "$lv" | grep "m" || {
		echo "$lv expected a mirror, but is not:"
		lvl -a $lv
		exit 1
	}
	mirror_images_redundant "$1" "$2"
	if test -n "$3"; then mirror_log_on "$lv" "$3"; fi
}

mirror_legs() {
	lv="$1/$2"
	expect="$3"
	lvdevices "$lv"
	real=`lvdevices "$lv" | wc -w`
	test "$expect" = "$real"
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
