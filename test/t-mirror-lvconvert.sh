# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
# Copyright (C) 2007 NEC Corporation
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. ./test-utils.sh

mimages_are_redundant_ ()
{
  local vg=$1
  local lv=$vg/$2
  local i

  rm -f out
  for i in $(lvs -odevices --noheadings $lv | sed 's/([^)]*)//g; s/,/ /g'); do
    lvs -a -o+devices $vg/$i 
    lvs -a -odevices --noheadings $vg/$i | sed 's/([^)]*)//g; s/,/ /g' | \
      sort | uniq >> out
  done

  # if any duplication is found, it's not redundant
  sort out | uniq -d | grep . && return 1

  return 0
}

lv_is_contiguous_ ()
{
  local lv=$1

  # if the lv has multiple segments, it's not contiguous
  lvs -a --segments $lv
  [ $(lvs -a --segments --noheadings $lv | wc -l) -ne 1 ] && return 1

  return 0
}

mimages_are_contiguous_ ()
{
  local vg=$1
  local lv=$vg/$2
  local i

  for i in $(lvs -odevices --noheadings $lv | sed 's/([^)]*)//g; s/,/ /g'); do
    lv_is_contiguous_ $vg/$i || return 1
  done

  return 0
}

mirrorlog_is_on_()
{
  local lv="$1"_mlog
  shift 1
  if ! lvs -a $lv; then return 0; fi # FIXME?
  lvs -a -o+devices $lv
  lvs -a -odevices --noheadings $lv | sed 's/,/\n/g' > out
  for d in $*; do grep "$d(" out || return 1; done
  for d in $*; do grep -v "$d(" out > out2 || true; mv out2 out; done
  grep . out && return 1
  return 0
}

save_dev_sum_()
{
  mkfs.ext3 $1 > /dev/null &&
  md5sum $1 > md5.$(basename $1)
}

check_dev_sum_()
{
  md5sum $1 > md5.tmp && cmp md5.$(basename $1) md5.tmp
}

check_mirror_count_()
{
  local lv=$1
  local mirrors=$2
  [ "$mirrors" -eq "$(lvs --noheadings -ostripes $lv)" ]
}

check_mirror_log_()
{
  local lv=$1
  local mlog=$(lvs --noheadings -omirror_log $lv | sed -e 's/ //g')
  [ "$(basename $lv)_mlog" == "$mlog" ]
}

wait_conversion_()
{
  local lv=$1
  while (lvs --noheadings -oattr "$lv" | grep -q '^ *c'); do sleep 1; done
}

wait_sync_()
{
  local lv=$1
  while [ `lvs --noheadings -o copy_percent $lv` != "100.00" ]; do sleep 1; done
}

check_no_tmplvs_()
{
  local lv=$1
  lvs -a -oname $(dirname $lv)
  lvs -a --noheadings -oname $(dirname $lv) > out
  ! grep tmp out
}

aux prepare_vg 5 200

# ---------------------------------------------------------------------
# Common environment setup/cleanup for each sub testcases

prepare_lvs_()
{
  lvremove -ff $vg
	if dmsetup table|grep $vg; then
		echo "ERROR: lvremove did leave some some mappings in DM behind!"
		return 1
	fi
	:
}

check_and_cleanup_lvs_()
{
  lvs -a -o+devices $vg
  lvremove -ff $vg
	if dmsetup table|grep $vg; then
		echo "ERROR: lvremove did leave some some mappings in DM behind!"
		return 1
	fi
}

not_sh ()
{
    "$@" && exit 1 || :;
}

# ---------------------------------------------------------------------
# Main repeating test function

log_name_to_count()
{
	if [ $1 == "mirrored" ]; then
		echo 2
	elif [ $1 == "disk" ]; then
		echo 1
	else
		echo 0
	fi
}

#
# FIXME: For test_[up|down]convert, I'd still like to be able
# to specifiy devices - especially if I can do partial PV
# specification for down-converts.  It may even be wise to
# do one round through these tests without specifying the PVs
# to use and one round where we do.
#

#
# test_upconvert
#   start_mirror_count:  The '-m' argument to create with
#   start_log_type: core|disk|mirrored
#   final_mirror_count: The '-m' argument to convert to
#   final_log_type: core|disk|mirrored
#   active: Whether the LV should be active when the convert happens
#
# Exmaple: Convert 2-way disk-log mirror to
#          3-way disk-log mirror while not active
# -> test_upconvert 1 disk 2 disk 0
test_upconvert()
{
	local start_count=$1
	local start_count_p1=$(($start_count + 1))
	local start_log_type=$2
	local finish_count=$3
	local finish_count_p1=$(($finish_count + 1))
	local finish_log_type=$4
	local dev_array=($dev1 $dev2 $dev3 $dev4 $dev5)
	local create_devs=""
	local convert_devs=""
	local log_devs=""
	local start_log_count
	local finish_log_count
	local max_log_count
	local alloc=""
	local active=true
	local i

	if [ $start_log_type == "disk" ] &&
		[ $finish_log_type == "mirrored" ]; then
		echo "FIXME:  disk -> mirrored log conversion not yet supported by LVM"
		return 0
	fi

	if [ $5 -eq 0 ]; then
		active=false
	fi

	# Do we have enough devices for the mirror images?
	if [ $finish_count_p1 -gt ${#dev_array[@]} ]; then
		echo "Action requires too many devices"
		return 1
	fi

	start_log_count=`log_name_to_count $start_log_type`
	finish_log_count=`log_name_to_count $finish_log_type`
	if [ $finish_log_count -gt $start_log_count ]; then
		max_log_count=$finish_log_count
	else
		max_log_count=$start_log_count
	fi

	# First group of devs for create
#	for i in $(seq 0 $start_count); do
#		create_devs="$create_devs ${dev_array[$i]}"
#	done

	# Second group of devs for convert
#	for i in $(seq $start_count_p1 $finish_count); do
#		convert_devs="$convert_devs ${dev_array[$i]}"
#	done

	# Third (or overlapping) group of devs for log
#	for i in $(seq $((${#dev_array[@]} - $max_log_count)) $((${#dev_array[@]} - 1))); do
#		if [ $i -gt $finish_count ]; then
#			log_devs="$log_devs ${dev_array[$i]}:0"
#		else
#			log_devs="$log_devs ${dev_array[$i]}"			
#		fi
#	done

	prepare_lvs_
	if [ $start_count -gt 0 ]; then
		# Are there extra devices for the log or do we overlap
		if [ $(($start_count_p1 + $start_log_count)) -gt ${#dev_array[@]} ]; then
			alloc="--alloc anywhere"
		fi

		lvcreate -l2 -m $start_count --mirrorlog $start_log_type \
			-n $lv1 $vg $alloc $create_devs $log_devs || return 1
		check_mirror_count_ $vg/$lv1 $start_count_p1
		# FIXME: check mirror log
	else
		lvcreate -l15 -n $lv1 $vg $create_devs || return 1
	fi

	lvs -a -o name,copy_percent,devices $vg
	if ! $active; then
		lvchange -an $vg/$lv1 || return 1
	fi

	# Are there extra devices for the log or do we overlap
	if [ $(($finish_count_p1 + $finish_log_count)) -gt ${#dev_array[@]} ]; then
		alloc="--alloc anywhere"
	fi
	echo y | lvconvert -m $finish_count --mirrorlog $finish_log_type \
		$vg/$lv1 $alloc $convert_devs $log_devs || return 1

	if ! $active; then
		lvchange -ay $vg/$lv1 || return 1
	fi

	wait_conversion_ $vg/$lv1
	lvs -a -o name,copy_percent,devices $vg
	check_no_tmplvs_ $vg/$lv1
	check_mirror_count_ $vg/$lv1 $finish_count_p1
	mimages_are_redundant_ $vg $lv1
	check_and_cleanup_lvs_
}

#
# test_downconvert
#   start_mirror_count:  The '-m' argument to create with
#   start_log_type: core|disk|mirrored
#   final_mirror_count: The '-m' argument to convert to
#   final_log_type: core|disk|mirrored
#   active: Whether the LV should be active when the convert happens
#
# Exmaple: Convert 3-way disk-log mirror to
#          2-way disk-log mirror while not active
# -> test_downconvert 2 disk 3 disk 0
test_downconvert()
{
	local start_count=$1
	local start_count_p1=$(($start_count + 1))
	local start_log_type=$2
	local finish_count=$3
	local finish_count_p1=$(($finish_count + 1))
	local finish_log_type=$4
	local dev_array=($dev1 $dev2 $dev3 $dev4 $dev5)
	local create_devs=""
	local convert_devs=""
	local log_devs=""
	local start_log_count
	local finish_log_count
	local max_log_count
	local alloc=""
	local active=true
	local i

	if [ $start_log_type == "disk" ] &&
		[ $finish_log_type == "mirrored" ]; then
		echo "FIXME:  disk -> mirrored log conversion not yet supported by LVM"
		return 0
	fi

	if [ $5 -eq 0 ]; then
		active=false
	fi

	# Do we have enough devices for the mirror images?
	if [ $start_count_p1 -gt ${#dev_array[@]} ]; then
		echo "Action requires too many devices"
		return 1
	fi

	start_log_count=`log_name_to_count $start_log_type`
	finish_log_count=`log_name_to_count $finish_log_type`
	if [ $finish_log_count -gt $start_log_count ]; then
		max_log_count=$finish_log_count
	else
		max_log_count=$start_log_count
	fi

	# First group of devs for create
#	for i in $(seq 0 $start_count); do
#		create_devs="$create_devs ${dev_array[$i]}"
#	done

	# Same devices for convert (because we are down-converting)
#	for i in $(seq 0 $start_count); do
#		convert_devs="$convert_devs ${dev_array[$i]}"
#	done

	# Third (or overlapping) group of devs for log creation
#	for i in $(seq $((${#dev_array[@]} - $max_log_count)) $((${#dev_array[@]} - 1))); do
#		if [ $i -gt $start_count ]; then
#			log_devs="$log_devs ${dev_array[$i]}:0"
#		else
#			log_devs="$log_devs ${dev_array[$i]}"			
#		fi
#	done

	prepare_lvs_
	if [ $start_count -gt 0 ]; then
		# Are there extra devices for the log or do we overlap
		if [ $(($start_count_p1 + $start_log_count)) -gt ${#dev_array[@]} ]; then
			alloc="--alloc anywhere"
		fi

		lvcreate -l2 -m $start_count --mirrorlog $start_log_type \
			-n $lv1 $vg $alloc $create_devs $log_devs || return 1
		check_mirror_count_ $vg/$lv1 $start_count_p1
		# FIXME: check mirror log
	else
		lvcreate -l15 -n $lv1 $vg $create_devs || return 1
	fi

	lvs -a -o name,copy_percent,devices $vg
	if ! $active; then
		lvchange -an $vg/$lv1 || return 1
	fi

	# Are there extra devices for the log or do we overlap
	if [ $(($finish_count_p1 + $finish_log_count)) -gt ${#dev_array[@]} ]; then
		alloc="--alloc anywhere"
	fi

	echo y | lvconvert -m $finish_count --mirrorlog $finish_log_type \
		$vg/$lv1 $alloc $convert_devs $log_devs || return 1

	if ! $active; then
		lvchange -ay $vg/$lv1 || return 1
	fi

	wait_conversion_ $vg/$lv1
	lvs -a -o name,copy_percent,devices $vg
	check_no_tmplvs_ $vg/$lv1
	check_mirror_count_ $vg/$lv1 $finish_count_p1
	mimages_are_redundant_ $vg $lv1
	check_and_cleanup_lvs_
}

# test_convert
#   start_image_count
#   start_log_type
#   finish_image_count
#   finish_log_type
test_convert()
{
	if [ $1 -lt $3 ]; then
		test_upconvert $1 $2 $3 $4 0 || return 1
		test_upconvert $1 $2 $3 $4 1 || return 1
	else
		test_downconvert $1 $2 $3 $4 0 || return 1
		test_downconvert $1 $2 $3 $4 1 || return 1
	fi
}


prepare_lvs_
check_and_cleanup_lvs_

# ---------------------------------------------------------------------
# mirrored LV tests

# ---
# Test conversion combinations from linear <-> 4-way mirrors
for i in $(seq 0 4); do
	for j in $(seq 0 4); do
		for k in core disk mirrored; do
			for l in core disk mirrored; do
				#############################################
				echo "Testing mirror conversion -m$i/$k -> -m$j/$l"
				test_convert $i $k $j $l
				#############################################
			done
		done
	done
done

# ---
# add mirror to mirror
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l15 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2
check_mirror_log_ $vg/$lv1
lvconvert -m+1 -i 20 -b $vg/$lv1 $dev4
# Next convert should fail b/c we can't have 2 at once
not lvconvert -m+1 -b $vg/$lv1 $dev5
wait_conversion_ $vg/$lv1
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1
check_mirror_count_ $vg/$lv1 3
mimages_are_redundant_ $vg $lv1
mirrorlog_is_on_ $vg/$lv1 $dev3
check_and_cleanup_lvs_

# add 1 mirror
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l15 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2
check_mirror_log_ $vg/$lv1
lvconvert -m+1 -i 20 -b $vg/$lv1 $dev4
# Next convert should fail b/c we can't have 2 at once
not lvconvert -m+1 -b $vg/$lv1 $dev5
wait_conversion_ $vg/$lv1
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1
check_mirror_count_ $vg/$lv1 3
mimages_are_redundant_ $vg $lv1
mirrorlog_is_on_ $vg/$lv1 $dev3
check_and_cleanup_lvs_

# remove 1 mirror from corelog'ed mirror
#  should retain 'core' log type
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l2 -m2 --corelog -n $lv1 $vg
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 3
not_sh check_mirror_log_ $vg/$lv1
lvconvert -m -1 -i1 $vg/$lv1
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1
check_mirror_count_ $vg/$lv1 2
mimages_are_redundant_ $vg $lv1
not_sh check_mirror_log_ $vg/$lv1
check_and_cleanup_lvs_

# add 2 mirrors
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2
check_mirror_log_ $vg/$lv1
lvconvert -m+2 -i1 $vg/$lv1 $dev4 $dev5
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1
check_mirror_count_ $vg/$lv1 4
mimages_are_redundant_ $vg $lv1
mirrorlog_is_on_ $vg/$lv1 $dev3
check_and_cleanup_lvs_

# add 1 mirror to core log mirror,
#  explicitly keep log as 'core'
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg $dev1 $dev2
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2
not_sh check_mirror_log_ $vg/$lv1
lvconvert -m+1 -i1 --mirrorlog core $vg/$lv1 $dev4 
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1 
check_mirror_count_ $vg/$lv1 3 
not_sh check_mirror_log_ $vg/$lv1
mimages_are_redundant_ $vg $lv1 
check_and_cleanup_lvs_

# add 1 mirror to core log mirror, but
#  implicitly keep log as 'core'
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg $dev1 $dev2
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2
not_sh check_mirror_log_ $vg/$lv1
lvconvert -m +1 -i1 $vg/$lv1
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1
check_mirror_count_ $vg/$lv1 3
not_sh check_mirror_log_ $vg/$lv1
mimages_are_redundant_ $vg $lv1
check_and_cleanup_lvs_

# add 2 mirrors to core log mirror" 
prepare_lvs_ 
lvs -a -o+devices $vg
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg $dev1 $dev2 
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2 
not_sh check_mirror_log_ $vg/$lv1 
lvconvert -m+2 -i1 --mirrorlog core $vg/$lv1 $dev4 $dev5 
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1 
check_mirror_count_ $vg/$lv1 4 
not_sh check_mirror_log_ $vg/$lv1
mimages_are_redundant_ $vg $lv1 
check_and_cleanup_lvs_

# ---
# add to converting mirror

# add 1 mirror then add 1 more mirror during conversion
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2
check_mirror_log_ $vg/$lv1
lvconvert -m+1 -b $vg/$lv1 $dev4
lvs -a -o+devices $vg
lvconvert -m+1 -i3 $vg/$lv1 $dev5
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1
check_mirror_count_ $vg/$lv1 4
mimages_are_redundant_ $vg $lv1
mirrorlog_is_on_ $vg/$lv1 $dev3
check_and_cleanup_lvs_

# ---
# core log to mirrored log

# change the log type from 'core' to 'mirrored'
prepare_lvs_
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg $dev1 $dev2
check_mirror_count_ $vg/$lv1 2
not_sh check_mirror_log_ $vg/$lv1
lvconvert --mirrorlog mirrored -i1 $vg/$lv1 $dev3 $dev4
check_no_tmplvs_ $vg/$lv1
check_mirror_log_ $vg/$lv1
mimages_are_redundant_ $vg $lv1

# ---
# mirrored log to core log

# change the log type from 'mirrored' to 'core'
lvconvert --mirrorlog core -i1 $vg/$lv1 $dev3 $dev4
check_no_tmplvs_ $vg/$lv1
not_sh check_mirror_log_ $vg/$lv1
mimages_are_redundant_ $vg $lv1
check_and_cleanup_lvs_

# ---
# Linear to mirror with mirrored log using --alloc anywhere
prepare_lvs_
lvcreate -l2 -n $lv1 $vg $dev1
lvconvert -m +1 --mirrorlog mirrored $vg/$lv1 $dev1 $dev2 --alloc anywhere
# FIXME Disable next check: --alloc anywhere makes *no* guarantees about placement - that's the entire point of it!
#mimages_are_redundant_ $vg $lv1
check_and_cleanup_lvs_


# ---
# check polldaemon restarts

# convert inactive mirror and start polling
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2
lvchange -an $vg/$lv1
lvconvert -m+1 $vg/$lv1 $dev4
lvs -a -o+devices $vg
lvchange -ay $vg/$lv1
wait_conversion_ $vg/$lv1
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1
check_and_cleanup_lvs_

# ---------------------------------------------------------------------
# removal during conversion

# "remove newly added mirror" 
prepare_lvs_ 
lvs -a -o+devices $vg
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2 
check_mirror_log_ $vg/$lv1 
lvconvert -m+1 -b $vg/$lv1 $dev4 
lvs -a -o+devices $vg
lvconvert -m-1 $vg/$lv1 $dev4 
lvs -a -o+devices $vg
wait_conversion_ $vg/$lv1 
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1 
check_mirror_count_ $vg/$lv1 2 
mimages_are_redundant_ $vg $lv1 
mirrorlog_is_on_ $vg/$lv1 $dev3 
check_and_cleanup_lvs_

# "remove one of newly added mirrors" 
prepare_lvs_ 
lvs -a -o+devices $vg
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2 
check_mirror_log_ $vg/$lv1 
lvconvert -m+2 -b $vg/$lv1 $dev4 $dev5 
lvs -a -o+devices $vg
lvconvert -m-1 $vg/$lv1 $dev4 
lvs -a -o+devices $vg
lvconvert -i1 $vg/$lv1 
lvs -a -o+devices $vg
wait_conversion_ $vg/$lv1 
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1 
check_mirror_count_ $vg/$lv1 3 
mimages_are_redundant_ $vg $lv1 
mirrorlog_is_on_ $vg/$lv1 $dev3 
check_and_cleanup_lvs_

# "remove from original mirror (the original is still mirror)"
prepare_lvs_ 
lvs -a -o+devices $vg
lvcreate -l2 -m2 -n $lv1 $vg $dev1 $dev2 $dev5 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 3 
check_mirror_log_ $vg/$lv1 
lvconvert -m+1 -b $vg/$lv1 $dev4 
lvs -a -o+devices $vg
lvconvert -m-1 $vg/$lv1 $dev2 
lvs -a -o+devices $vg
lvconvert -i1 $vg/$lv1 
lvs -a -o+devices $vg
wait_conversion_ $vg/$lv1 
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1 
check_mirror_count_ $vg/$lv1 3 
mimages_are_redundant_ $vg $lv1 
mirrorlog_is_on_ $vg/$lv1 $dev3 
check_and_cleanup_lvs_

# "remove from original mirror (the original becomes linear)"
prepare_lvs_ 
lvs -a -o+devices $vg
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2 
check_mirror_log_ $vg/$lv1 
lvconvert -m+1 -b $vg/$lv1 $dev4 
lvs -a -o+devices $vg
lvconvert -m-1 $vg/$lv1 $dev2 
lvs -a -o+devices $vg
lvconvert -i1 $vg/$lv1 
lvs -a -o+devices $vg
wait_conversion_ $vg/$lv1 
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1 
check_mirror_count_ $vg/$lv1 2 
mimages_are_redundant_ $vg $lv1 
mirrorlog_is_on_ $vg/$lv1 $dev3 
check_and_cleanup_lvs_

# ---------------------------------------------------------------------

# "rhbz440405: lvconvert -m0 incorrectly fails if all PEs allocated"
prepare_lvs_ 
lvs -a -o+devices $vg
lvcreate -l`pvs --noheadings -ope_count $dev1` -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0 
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2 
check_mirror_log_ $vg/$lv1 
wait_sync_ $vg/$lv1 # cannot pull primary unless mirror in-sync
lvconvert -m0 $vg/$lv1 $dev1 
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1 
check_mirror_count_ $vg/$lv1 1 
check_and_cleanup_lvs_

# "rhbz264241: lvm mirror doesn't lose it's "M" --nosync attribute after being down and the up converted"
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l2 -m1 -n$lv1 --nosync $vg 
lvs -a -o+devices $vg
lvconvert -m0 $vg/$lv1
lvs -a -o+devices $vg
lvconvert -m1 $vg/$lv1
lvs -a -o+devices $vg
lvs --noheadings -o attr $vg/$lv1 | grep '^ *m'
check_and_cleanup_lvs_

# lvconvert from linear (on multiple PVs) to mirror
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l 8 -n $lv1 $vg $dev1:0-3 $dev2:0-3
lvs -a -o+devices $vg
lvconvert -m1 $vg/$lv1
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2
check_mirror_log_ $vg/$lv1
check_and_cleanup_lvs_

# BZ 463272: disk log mirror convert option is lost if downconvert option is also given
prepare_lvs_
lvs -a -o+devices $vg
lvcreate -l1 -m2 --corelog -n $lv1 $vg
lvs -a -o+devices $vg
lvconvert -m1 --mirrorlog disk $vg/$lv1
lvs -a -o+devices $vg
check_mirror_log_ $vg/$lv1

# ---
# add mirror and disk log

# "add 1 mirror and disk log" 
prepare_lvs_ 
lvs -a -o+devices $vg
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg $dev1 $dev2
lvs -a -o+devices $vg
check_mirror_count_ $vg/$lv1 2 
not_sh check_mirror_log_ $vg/$lv1 
# FIXME on next line, specifying $dev3:0 $dev4 (i.e log device first) fails (!)
lvconvert -m+1 --mirrorlog disk -i1 $vg/$lv1 $dev4 $dev3:0
lvs -a -o+devices $vg
check_no_tmplvs_ $vg/$lv1 
check_mirror_count_ $vg/$lv1 3 
check_mirror_log_ $vg/$lv1 
mimages_are_redundant_ $vg $lv1 
mirrorlog_is_on_ $vg/$lv1 $dev3 
check_and_cleanup_lvs_
