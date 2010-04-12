#!/bin/bash

set -e -o pipefail

lvdevices() {
	lvs -a -odevices --noheadings "$@" | sed 's/([^)]*)//g; s/,/ /g'
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
	test `lvs -a --segments --noheadings $1 | wc -l` -eq 1 || {
		echo "LV $1 expected to be contiguous, but is not:"
		lvs -a --segments --noheadings $1
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
	lvs -oattr "$lv" | grep "m" || {
		echo "$lv expected a mirror, but is not:"
		lvs -a $lv
		exit 1
	}
	mirror_images_redundant "$1" "$2"
	if test -n "$3"; then mirror_log_on "$lv" "$3"; fi
}

linear() {
	lv="$1/$2"
	lvs -ostripes "$lv" | grep -q "1" || {
		echo "$lv expected linear, but is not:"
		lvs -a "$lv" -o+devices
		exit 1
	}
}

"$@"
