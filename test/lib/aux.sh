#!/usr/bin/env bash
# Copyright (C) 2011-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/utils

run_valgrind() {
	# Execute script which may use $TESTNAME for creating individual
	# log files for each execute command
	exec "${VALGRIND:-valgrind}" "$@"
}

expect_failure() {
        echo "TEST EXPECT FAILURE"
}

prepare_clvmd() {
	rm -f debug.log strace.log
	test "${LVM_TEST_LOCKING:-0}" -ne 3 && return # not needed

	if pgrep clvmd ; then
		echo "Cannot use fake cluster locking with real clvmd ($(pgrep clvmd)) running."
		skip
	fi

	# skip if we don't have our own clvmd...
	(which clvmd 2>/dev/null | grep "$abs_builddir") || skip
	# lvs is executed from clvmd - use our version
	export LVM_BINARY=$(which lvm)

	test -e "$DM_DEV_DIR/control" || dmsetup table # create control node
	# skip if singlenode is not compiled in
	(clvmd --help 2>&1 | grep "Available cluster managers" | grep "singlenode") || skip

#	lvmconf "activation/monitoring = 1"
	local run_valgrind=
	test "${LVM_VALGRIND_CLVMD:-0}" -eq 0 || run_valgrind="run_valgrind"
	rm -f "$CLVMD_PIDFILE"
	$run_valgrind clvmd -Isinglenode -d 1 -f &
	echo $! > LOCAL_CLVMD

	for i in $(seq 1 100) ; do
		test $i -eq 100 && die "Startup of clvmd is too slow."
		test -e "$CLVMD_PIDFILE" && break
		sleep .2
	done
}

prepare_dmeventd() {
	rm -f debug.log strace.log
	if pgrep dmeventd ; then
		echo "Cannot test dmeventd with real dmeventd ($(pgrep dmeventd)) running."
		skip
	fi

	# skip if we don't have our own dmeventd...
	(which dmeventd 2>/dev/null | grep "$abs_builddir") || skip

	lvmconf "activation/monitoring = 1"

	local run_valgrind=
	test "${LVM_VALGRIND_DMEVENTD:-0}" -eq 0 || run_valgrind="run_valgrind"
	$run_valgrind dmeventd -f "$@" &
	echo $! > LOCAL_DMEVENTD

	# FIXME wait for pipe in /var/run instead
	for i in $(seq 1 100) ; do
		test $i -eq 100 && die "Startup of dmeventd is too slow."
		test -e "${DMEVENTD_PIDFILE}" && break
		sleep .2
	done
	echo ok
}

prepare_lvmetad() {
	test $# -eq 0 && default_opts="-l wire,debug"
	rm -f debug.log strace.log
	# skip if we don't have our own lvmetad...
	(which lvmetad 2>/dev/null | grep "$abs_builddir") || skip

	lvmconf "global/use_lvmetad = 1"
	lvmconf "devices/md_component_detection = 0"

	local run_valgrind=
	test "${LVM_VALGRIND_LVMETAD:-0}" -eq 0 || run_valgrind="run_valgrind"

	kill_sleep_kill_ LOCAL_LVMETAD ${LVM_VALGRIND_LVMETAD:-0}

	echo "preparing lvmetad..."
	$run_valgrind lvmetad -f "$@" -s "$TESTDIR/lvmetad.socket" $default_opts "$@" &
	echo $! > LOCAL_LVMETAD
	while ! test -e "$TESTDIR/lvmetad.socket"; do echo -n .; sleep .1; done # wait for the socket
	echo ok
}

lvmetad_talk() {
	local use=nc
	if type -p socat >& /dev/null; then
		use=socat
	elif echo | not nc -U "$TESTDIR/lvmetad.socket" ; then
		echo "WARNING: Neither socat nor nc -U seems to be available." 1>&2
		echo "# failed to contact lvmetad"
		return 1
	fi

	if test "$use" = nc ; then
		nc -U "$TESTDIR/lvmetad.socket"
	else
		socat "unix-connect:$TESTDIR/lvmetad.socket" -
	fi | tee -a lvmetad-talk.txt
}

lvmetad_dump() {
	(echo 'request="dump"'; echo '##') | lvmetad_talk "$@"
}

notify_lvmetad() {
	if test -e LOCAL_LVMETAD; then
		pvscan --cache "$@" || true
	fi
}

teardown_devs_prefixed() {
	local prefix=$1
	local stray=${2:-0}
	local IFS=$IFS_NL
	local dm

	# Resume suspended devices first
	for dm in $(dm_info suspended,name | grep "^Suspended:.*$prefix"); do
		echo "dmsetup resume \"${dm#Suspended:}\""
		dmsetup clear "${dm#Suspended:}"
		dmsetup resume "${dm#Suspended:}" &
	done

	wait

	local mounts=( $(grep "$prefix" /proc/mounts | cut -d' ' -f1) )
	if test ${#mounts[@]} -gt 0; then
		test "$stray" -eq 0 || echo "Removing stray mounted devices containing $prefix: ${mounts[@]}"
		if umount -fl "${mounts[@]}"; then
			udev_wait
		fi
	fi

	# Remove devices, start with closed (sorted by open count)
	local remfail=no
	local need_udev_wait=0
	init_udev_transaction
	for dm in $(dm_info name --sort open | grep "$prefix"); do
		dmsetup remove "$dm" &>/dev/null || remfail=yes
		need_udev_wait=1
	done
	finish_udev_transaction
	test $need_udev_wait -eq 0 || udev_wait

	if test $remfail = yes; then
		local num_devs
		local num_remaining_devs=999
		while num_devs=$(dm_table | grep "$prefix" | wc -l) && \
		    test $num_devs -lt $num_remaining_devs -a $num_devs -ne 0; do
			test "$stray" -eq 0 || echo "Removing $num_devs stray mapped devices with names beginning with $prefix: "
			for dm in $(dm_info name --sort open | grep "$prefix") ; do
				dmsetup remove -f "$dm" || true
			done
			num_remaining_devs=$num_devs
		done
	fi
}

teardown_devs() {
	# Delete any remaining dm/udev semaphores
	teardown_udev_cookies

	test -z "$PREFIX" || {
		rm -rf "$TESTDIR/dev/$PREFIX"*
		teardown_devs_prefixed "$PREFIX"
	}

	# NOTE: SCSI_DEBUG_DEV test must come before the LOOP test because
	# prepare_scsi_debug_dev() also sets LOOP to short-circuit prepare_loop()
	if test -f SCSI_DEBUG_DEV; then
		test "${LVM_TEST_PARALLEL:-0}" -eq 1 || modprobe -r scsi_debug
	else
		test ! -f LOOP || losetup -d $(< LOOP) || true
		test ! -f LOOPFILE || rm -f $(< LOOPFILE)
	fi
	rm -f DEVICES # devs is set in prepare_devs()
	not diff LOOP BACKING_DEV >/dev/null 2>&1 || rm -f BACKING_DEV
	rm -f LOOP

	# Attempt to remove any loop devices that failed to get torn down if earlier tests aborted
	test "${LVM_TEST_PARALLEL:-0}" -eq 1 -o -z "$COMMON_PREFIX" || {
		teardown_devs_prefixed "$COMMON_PREFIX" 1
		local stray_loops=( $(losetup -a | grep "$COMMON_PREFIX" | cut -d: -f1) )
		test ${#stray_loops[@]} -eq 0 || {
			echo "Removing stray loop devices containing $COMMON_PREFIX: ${stray_loops[@]}"
			for i in "${stray_loops[@]}" ; do losetup -d $i ; done
		}
	}

	# Leave test when udev processed all removed devices
	udev_wait
}

kill_sleep_kill_() {
	pidfile=$1
	slow=$2
	if test -s $pidfile ; then
		pid=$(< $pidfile)
		kill -TERM $pid || return 0
		if test $slow -eq 0 ; then sleep .1 ; else sleep 1 ; fi
		kill -KILL $pid 2>/dev/null || true
		wait=0
		while ps $pid > /dev/null && test $wait -le 10; do
			sleep .5
			wait=$(($wait + 1))
		done
	fi
}

teardown() {
	echo -n "## teardown..."

	kill_sleep_kill_ LOCAL_LVMETAD ${LVM_VALGRIND_LVMETAD:-0}

	dm_table | not egrep -q "$vg|$vg1|$vg2|$vg3|$vg4" || {
		# Avoid activation of dmeventd if there is no pid
		cfg=$(test -s LOCAL_DMEVENTD || echo "--config activation{monitoring=0}")
		vgremove -ff $cfg  \
			$vg $vg1 $vg2 $vg3 $vg4 &>/dev/null || rm -f debug.log strace.log
	}

	kill_sleep_kill_ LOCAL_CLVMD ${LVM_VALGRIND_CLVMD:-0}

	echo -n .

	kill_sleep_kill_ LOCAL_DMEVENTD ${LVM_VALGRIND_DMEVENTD:-0}

	echo -n .

	test -d "$DM_DEV_DIR/mapper" && teardown_devs

	echo -n .

	test -n "$TESTDIR" && {
		cd "$TESTOLDPWD"
		rm -rf "$TESTDIR" || echo BLA
	}

	echo "ok"

	test "${LVM_TEST_PARALLEL:-0}" -eq 1 -o -n "$RUNNING_DMEVENTD" || not pgrep dmeventd #&>/dev/null
}

prepare_loop() {
	local size=${1=32}
	local i
	local slash

	test -f LOOP && LOOP=$(< LOOP)
	echo -n "## preparing loop device..."

	# skip if prepare_scsi_debug_dev() was used
	if test -f SCSI_DEBUG_DEV -a -f LOOP ; then
		echo "(skipped)"
		return 0
	fi

	test ! -e LOOP
	test -n "$DM_DEV_DIR"

	for i in 0 1 2 3 4 5 6 7; do
		test -e "$DM_DEV_DIR/loop$i" || mknod "$DM_DEV_DIR/loop$i" b 7 $i
	done

	echo -n .

	local LOOPFILE="$PWD/test.img"
	dd if=/dev/zero of="$LOOPFILE" bs=$((1024*1024)) count=0 seek=$(($size)) 2> /dev/null
	if LOOP=$(losetup -s -f "$LOOPFILE" 2>/dev/null); then
		:
	elif LOOP=$(losetup -f) && losetup "$LOOP" "$LOOPFILE"; then
		# no -s support
		:
	else
		# no -f support
		# Iterate through $DM_DEV_DIR/loop{,/}{0,1,2,3,4,5,6,7}
		for slash in '' /; do
			for i in 0 1 2 3 4 5 6 7; do
				local dev="$DM_DEV_DIR/loop$slash$i"
				! losetup "$dev" >/dev/null 2>&1 || continue
				# got a free
				losetup "$dev" "$LOOPFILE"
				LOOP=$dev
				break
			done
			test -z "$LOOP" || break
		done
	fi
	test -n "$LOOP" # confirm or fail
	BACKING_DEV="$LOOP"
	echo "$LOOP" > LOOP
	echo "$LOOP" > BACKING_DEV
	echo "ok ($LOOP)"
}

# A drop-in replacement for prepare_loop() that uses scsi_debug to create
# a ramdisk-based SCSI device upon which all LVM devices will be created
# - scripts must take care not to use a DEV_SIZE that will enduce OOM-killer
prepare_scsi_debug_dev() {
	local DEV_SIZE=$1
	local SCSI_DEBUG_PARAMS=${@:2}

	test ! -f "SCSI_DEBUG_DEV" || return 0
	test -z "$LOOP"
	test -n "$DM_DEV_DIR"

	# Skip test if awk isn't available (required for get_sd_devs_)
	which awk || skip

	# Skip test if scsi_debug module is unavailable or is already in use
	modprobe --dry-run scsi_debug || skip
	lsmod | not grep -q scsi_debug || skip

	# Create the scsi_debug device and determine the new scsi device's name
	# NOTE: it will _never_ make sense to pass num_tgts param;
	# last param wins.. so num_tgts=1 is imposed
	modprobe scsi_debug dev_size_mb=$DEV_SIZE $SCSI_DEBUG_PARAMS num_tgts=1 || skip
	sleep 2 # allow for async Linux SCSI device registration

	local DEBUG_DEV="/dev/$(grep -H scsi_debug /sys/block/*/device/model | cut -f4 -d /)"
	test -b "$DEBUG_DEV" || return 1 # should not happen

	# Create symlink to scsi_debug device in $DM_DEV_DIR
	SCSI_DEBUG_DEV="$DM_DEV_DIR/$(basename $DEBUG_DEV)"
	echo "$SCSI_DEBUG_DEV" > SCSI_DEBUG_DEV
	echo "$SCSI_DEBUG_DEV" > BACKING_DEV
	# Setting $LOOP provides means for prepare_devs() override
	test "$DEBUG_DEV" = "$SCSI_DEBUG_DEV" || ln -snf "$DEBUG_DEV" "$SCSI_DEBUG_DEV"
}

cleanup_scsi_debug_dev() {
	teardown_devs
	rm -f SCSI_DEBUG_DEV LOOP
}

prepare_backing_dev() {
	if test -f BACKING_DEV; then 
		BACKING_DEV=$(< BACKING_DEV)
	elif test -b "$LVM_TEST_BACKING_DEVICE"; then
		BACKING_DEV="$LVM_TEST_BACKING_DEVICE"
		echo "$BACKING_DEV" > BACKING_DEV
	else
		prepare_loop "$@"
	fi
}

prepare_devs() {
	local n=${1:-3}
	local devsize=${2:-34}
	local pvname=${3:-pv}

	prepare_backing_dev $(($n*$devsize))
	echo -n "## preparing $n devices..."

	local size=$(($devsize*2048)) # sectors
	local count=0
	init_udev_transaction
	for i in $(seq 1 $n); do
		local name="${PREFIX}$pvname$i"
		local dev="$DM_DEV_DIR/mapper/$name"
		DEVICES[$count]=$dev
		count=$(( $count + 1 ))
		echo 0 $size linear "$BACKING_DEV" $((($i-1)*$size)) > "$name.table"
		if not dmsetup create -u "TEST-$name" "$name" "$name.table" &&
		   test -n "$LVM_TEST_BACKING_DEVICE";
		then # maybe the backing device is too small for this test
		    LVM_TEST_BACKING_DEVICE=
		    rm -f BACKING_DEV
		    prepare_devs "$@"
		    return $?
		fi
	done
	finish_udev_transaction

	# non-ephemeral devices need to be cleared between tests
	test -f LOOP || for d in ${DEVICES[@]}; do
		dd if=/dev/zero of=$d bs=64K count=1
	done

	#for i in `seq 1 $n`; do
	#	local name="${PREFIX}$pvname$i"
	#	dmsetup info -c $name
	#done
	#for i in `seq 1 $n`; do
	#	local name="${PREFIX}$pvname$i"
	#	dmsetup table $name
	#done

	printf "%s\n" "${DEVICES[@]}" > DEVICES
#	( IFS=$'\n'; echo "${DEVICES[*]}" ) >DEVICES
	echo "ok"
}

# Replace linear PV device with its 'delayed' version
# Could be used to more deterministicaly hit some problems.
# Parameters: {device path} [read delay ms] [write delay ms]
# Original device is restored when both delay params are 0 (or missing).
# i.e.  delay_dev "$dev1" 0 200
delay_dev() {
	target_at_least dm-delay 1 2 0 || skip
	local name=$(echo "$1" | sed -e 's,.*/,,')
	local read_ms=${2:-0}
	local write_ms=${3:-0}
	local pos
	local size
	local type
	local pvdev
	local offset

	read pos size type pvdev offset < "$name.table"

	init_udev_transaction
	if test $read_ms -ne 0 -o $write_ms -ne 0 ; then
		echo "0 $size delay $pvdev $offset $read_ms $pvdev $offset $write_ms" | \
			dmsetup load "$name"
	else
		dmsetup load "$name" "$name.table"
	fi
	dmsetup resume "$name"
	finish_udev_transaction
}

disable_dev() {
	local dev
	local silent
	local error
	local notify

	while test -n "$1"; do
	    if test "$1" = "--silent"; then
		silent=1
		shift
	    elif test "$1" = "--error"; then
		error=1
		shift
	    else
		break
	    fi
	done

	udev_wait
	for dev in "$@"; do
		maj=$(($(stat -L --printf=0x%t "$dev")))
		min=$(($(stat -L --printf=0x%T "$dev")))
		echo "Disabling device $dev ($maj:$min)"
		notify="$notify $maj:$min"
		if test -n "$error"; then
		    echo 0 10000000 error | dmsetup load $dev
		    dmsetup resume $dev
		else
		    dmsetup remove -f "$dev" 2>/dev/null || true
		fi
	done

	test -n "$silent" || for num in $notify; do
		notify_lvmetad --major $(echo $num | sed -e "s,:.*,,") \
		               --minor $(echo $num | sed -e "s,.*:,,")
	done
}

enable_dev() {
	local dev
	local silent

	if test "$1" = "--silent"; then
	    silent=1
	    shift
	fi

	rm -f debug.log strace.log
	init_udev_transaction
	for dev in "$@"; do
		local name=$(echo "$dev" | sed -e 's,.*/,,')
		dmsetup create -u "TEST-$name" "$name" "$name.table" 2>/dev/null || \
			dmsetup load "$name" "$name.table"
		# using device name (since device path does not exists yes with udev)
		dmsetup resume "$name"
	done
	finish_udev_transaction

	test -n "$silent" || for dev in "$@"; do
		notify_lvmetad "$dev"
	done
}

#
# Convert device to device with errors
# Takes the list of pairs of error segment from:len
# Original device table is replace with multiple lines
# i.e.  error_dev "$dev1" 8:32 96:8
error_dev() {
	local dev=$1
	local name=$(echo "$dev" | sed -e 's,.*/,,')
	local fromlen
	local pos
	local size
	local type
	local pvdev
	local offset
	local silent

	read pos size type pvdev offset < $name.table

	shift
	rm -f $name.errtable
	for fromlen in "$@"; do
		from=${fromlen%%:*}
		len=${fromlen##*:}
		diff=$(($from - $pos))
		if test $diff -gt 0 ; then
			echo "$pos $diff $type $pvdev $(($pos + $offset))" >>$name.errtable
			pos=$(($pos + $diff))
		elif test $diff -lt 0 ; then
			die "Position error"
		fi
		echo "$from $len error" >>$name.errtable
		pos=$(($pos + $len))
	done
	diff=$(($size - $pos))
	test $diff -gt 0 && echo "$pos $diff $type $pvdev $(($pos + $offset))" >>$name.errtable

	init_udev_transaction
	if dmsetup table $name ; then
		dmsetup load "$name" "$name.errtable"
	else
		dmsetup create -u "TEST-$name" "$name" "$name.errtable"
	fi
	# using device name (since device path does not exists yet with udev)
	dmsetup resume "$name"
	finish_udev_transaction
	test -n "$silent" || notify_lvmetad "$dev"
}

backup_dev() {
	local dev

	for dev in "$@"; do
		dd if="$dev" of="$dev.backup" bs=1024
	done
}

restore_dev() {
	local dev

	for dev in "$@"; do
		test -e "$dev.backup" || \
			die "Internal error: $dev not backed up, can't restore!"
		dd of="$dev" if="$dev.backup" bs=1024
	done
}

prepare_pvs() {
	prepare_devs "$@"
	pvcreate -ff "${DEVICES[@]}"
}

prepare_vg() {
	teardown_devs

	prepare_devs "$@"
	vgcreate -s 512K $vg "${DEVICES[@]}"
}

extend_filter() {
	filter=$(grep ^devices/global_filter CONFIG_VALUES | tail -n 1)
	for rx in "$@"; do
		filter=$(echo $filter | sed -e "s:\[:[ \"$rx\", :")
	done
	lvmconf "$filter"
}

extend_filter_LVMTEST() {
	extend_filter "a|$DM_DEV_DIR/LVMTEST|"
}

hide_dev() {
	filter=$(grep ^devices/global_filter CONFIG_VALUES | tail -n 1)
	for dev in $@; do
		filter=$(echo $filter | sed -e "s:\[:[ \"r|$dev|\", :")
	done
	lvmconf "$filter"
}

unhide_dev() {
	filter=$(grep ^devices/global_filter CONFIG_VALUES | tail -n 1)
	for dev in $@; do
		filter=$(echo $filter | sed -e "s:\"r|$dev|\", ::")
	done
	lvmconf "$filter"
}

mkdev_md5sum() {
	rm -f debug.log strace.log
	mkfs.ext2 "$DM_DEV_DIR/$1/$2" || return 1
	md5sum "$DM_DEV_DIR/$1/$2" > "md5.$1-$2"
}

generate_config() {
	if test -n "$profile_name"; then
		config_values=PROFILE_VALUES_$profile_name
		config=PROFILE_$profile_name
		touch $config_values
	else
		config_values=CONFIG_VALUES
		config=CONFIG
	fi

	LVM_TEST_LOCKING=${LVM_TEST_LOCKING:-1}
	if test "$DM_DEV_DIR" = "/dev"; then
	    LVM_VERIFY_UDEV=${LVM_VERIFY_UDEV:-0}
	else
	    LVM_VERIFY_UDEV=${LVM_VERIFY_UDEV:-1}
	fi
	test -f "$config_values" || {
            cat > "$config_values" <<-EOF
devices/dir = "$DM_DEV_DIR"
devices/scan = "$DM_DEV_DIR"
devices/filter = "a|.*|"
devices/global_filter = [ "a|$DM_DEV_DIR/mapper/.*pv[0-9_]*$|", "r|.*|" ]
devices/cache_dir = "$TESTDIR/etc"
devices/sysfs_scan = 0
devices/default_data_alignment = 1
devices/md_component_detection  = 0
log/syslog = 0
log/indent = 1
log/level = 9
log/file = "$TESTDIR/debug.log"
log/overwrite = 1
log/activation = 1
log/verbose = 0
activation/retry_deactivation = 1
backup/backup = 0
backup/archive = 0
global/abort_on_internal_errors = 1
global/detect_internal_vg_cache_corruption = 1
global/library_dir = "$TESTDIR/lib"
global/locking_dir = "$TESTDIR/var/lock/lvm"
global/locking_type=$LVM_TEST_LOCKING
global/si_unit_consistency = 1
global/fallback_to_local_locking = 0
activation/checks = 1
activation/udev_sync = 1
activation/udev_rules = 1
activation/verify_udev_operations = $LVM_VERIFY_UDEV
activation/polling_interval = 0
activation/snapshot_autoextend_percent = 50
activation/snapshot_autoextend_threshold = 50
activation/monitoring = 0
allocation/wipe_signatures_when_zeroing_new_lvs = 0
EOF
	}

	local v
	for v in "$@"; do
	    echo "$v"
	done >> "$config_values"

	local s
	for s in $(cut -f1 -d/ "$config_values" | sort | uniq); do
		echo "$s {"
		local k
		for k in $(grep ^"$s"/ "$config_values" | cut -f1 -d= | sed -e 's, *$,,' | sort | uniq); do
			grep "^$k" "$config_values" | tail -n 1 | sed -e "s,^$s/,	  ,"
		done
		echo "}"
		echo
	done | tee "$config"
}

lvmconf() {
	unset profile_name
	generate_config "$@"
	mv -f CONFIG etc/lvm.conf
}

profileconf() {
	profile_name="$1"
	shift
	generate_config "$@"
	test -d etc/profile || mkdir etc/profile
	mv -f "PROFILE_$profile_name" "etc/profile/$profile_name.profile"
}

prepare_profiles() {
	test -d etc/profile || mkdir etc/profile
	for profile_name in $@; do
		test -L "$abs_top_builddir/test/lib/$profile_name.profile" || skip
		cp "$abs_top_builddir/test/lib/$profile_name.profile" "etc/profile/$profile_name.profile"
	done
}

apitest() {
	local t=$1
	shift
	test -x "$abs_top_builddir/test/api/$t.t" || skip
	"$abs_top_builddir/test/api/$t.t" "$@" && rm -f debug.log strace.log
}

api() {
	test -x "$abs_top_builddir/test/api/wrapper" || skip
	"$abs_top_builddir/test/api/wrapper" "$@" && rm -f debug.log strace.log
}

mirror_recovery_works() {
	case "$(uname -r)" in
	  3.3.4-5.fc17.i686|3.3.4-5.fc17.x86_64) return 1 ;;
	esac
}

raid456_replace_works() {
# The way kmem_cache aliasing is done in the kernel is broken.
# It causes RAID 4/5/6 tests to fail.
#
# The problem with kmem_cache* is this:
# *) Assume CONFIG_SLUB is set
# 1) kmem_cache_create(name="foo-a")
# - creates new kmem_cache structure
# 2) kmem_cache_create(name="foo-b")
# - If identical cache characteristics, it will be merged with the previously
#   created cache associated with "foo-a".  The cache's refcount will be
#   incremented and an alias will be created via sysfs_slab_alias().
# 3) kmem_cache_destroy(<ptr>)
# - Attempting to destroy cache associated with "foo-a", but instead the
#   refcount is simply decremented.  I don't even think the sysfs aliases are
#   ever removed...
# 4) kmem_cache_create(name="foo-a")
# - This FAILS because kmem_cache_sanity_check colides with the existing
#   name ("foo-a") associated with the non-removed cache.
#
# This is a problem for RAID (specifically dm-raid) because the name used
# for the kmem_cache_create is ("raid%d-%p", level, mddev).  If the cache
# persists for long enough, the memory address of an old mddev will be
# reused for a new mddev - causing an identical formulation of the cache
# name.  Even though kmem_cache_destory had long ago been used to delete
# the old cache, the merging of caches has cause the name and cache of that
# old instance to be preserved and causes a colision (and thus failure) in
# kmem_cache_create().  I see this regularly in testing the following
# kernels:
#
# This seems to be finaly resolved with this patch:
# http://www.redhat.com/archives/dm-devel/2014-March/msg00008.html
# so we need to put here exlusion for kernes which do trace SLUB
#
	case "$(uname -r)" in
	  3.6.*.fc18.i686*|3.6.*.fc18.x86_64) return 1 ;;
	  3.9.*.fc19.i686*|3.9.*.fc19.x86_64) return 1 ;;
	  3.1[0123].*.fc18.i686*|3.1[0123].*.fc18.x86_64) return 1 ;;
	  3.1[01234].*.fc19.i686*|3.1[01234].*.fc19.x86_64) return 1 ;;
	  3.1[123].*.fc20.i686*|3.1[123].*.fc20.x86_64) return 1 ;;
	  3.14.*.fc21.i686*|3.14.*.fc21.x86_64) return 1 ;;
	  3.15.*rc6*.fc21.i686*|3.15.*rc6*.fc21.x86_64) return 1 ;;
	  3.16.*rc4*.fc21.i686*|3.16.*rc4*.fc21.x86_64) return 1 ;;
	esac
}

udev_wait() {
	pgrep udev >/dev/null || return 0
	which udevadm &>/dev/null || return 0
	if test -n "$1" ; then
		udevadm settle --exit-if-exists="$1" || true
	else
		udevadm settle --timeout=15 || true
	fi
}

# wait_for_sync <VG/LV>
wait_for_sync() {
	local i
	for i in {1..100} ; do
		check in_sync $1 $2 && return
		sleep .2
	done

	echo "Sync is taking too long - assume stuck"
	return 1
}

# Check if tests are running on 64bit architecture
can_use_16T() {
	test "$(getconf LONG_BIT)" -eq 64
}

# Check if major.minor.revision' string is 'at_least'
version_at_least() {
	local major
	local minor
	local revision
	IFS=. read -r major minor revision <<< "$1"
	shift

	test -z "$1" && return 0
	test -n "$major" || return 1
	test "$major" -gt "$1" && return 0
	test "$major" -eq "$1" || return 1

	test -z "$2" && return 0
	test -n "$minor" || return 1
	test "$minor" -gt "$2" && return 0
	test "$minor" -eq "$2" || return 1

	test -z "$3" && return 0
	test "$revision" -ge "$3" 2>/dev/null || return 1
}
#
# Check wheter kernel [dm module] target exist
# at least in expected version
#
# [dm-]target-name major minor revision
#
# i.e.   dm_target_at_least  dm-thin-pool  1 0
target_at_least() {
	rm -f debug.log strace.log
	case "$1" in
	  dm-*) modprobe "$1" || true ;;
	esac

	if test "$1" = dm-raid; then
		case "$(uname -r)" in
		  3.12.0*) return 1 ;;
		esac
	fi

	local version=$(dmsetup targets 2>/dev/null | grep "${1##dm-} " 2>/dev/null)
	version=${version##* v}
	shift

	version_at_least "$version" "$@"
}

have_thin() {
	test "$THIN" = shared -o "$THIN" = internal || return 1
	target_at_least dm-thin-pool "$@" || return 1

	# disable thin_check if not present in system
	test -x "$LVM_TEST_THIN_CHECK_CMD" || LVM_TEST_THIN_CHECK_CMD=""
	test -x "$LVM_TEST_THIN_DUMP_CMD" || LVM_TEST_THIN_DUMP_CMD=""
	test -x "$LVM_TEST_THIN_REPAIR_CMD" || LVM_TEST_THIN_REPAIR_CMD=""
	lvmconf "global/thin_check_executable = \"$LVM_TEST_THIN_CHECK_CMD\"" \
		"global/thin_dump_executable = \"$LVM_TEST_THIN_DUMP_CMD\"" \
		"global/thin_repair_executable = \"$LVM_TEST_THIN_REPAIR_CMD\""
}

have_raid() {
	test "$RAID" = shared -o "$RAID" = internal || return 1
	target_at_least dm-raid "$@"
}

have_cache() {
	test "$CACHE" = shared -o "$CACHE" = internal || return 1
	target_at_least dm-cache "$@"

	test -x "$LVM_TEST_CACHE_CHECK_CMD" || LVM_TEST_CACHE_CHECK_CMD=""
	test -x "$LVM_TEST_CACHE_DUMP_CMD" || LVM_TEST_CACHE_DUMP_CMD=""
	test -x "$LVM_TEST_CACHE_REPAIR_CMD" || LVM_TEST_CACHE_REPAIR_CMD=""
	lvmconf "global/cache_check_executable = \"$LVM_TEST_CACHE_CHECK_CMD\"" \
		"global/cache_dump_executable = \"$LVM_TEST_CACHE_DUMP_CMD\"" \
		"global/cache_repair_executable = \"$LVM_TEST_CACHE_REPAIR_CMD\""
}

have_tool_at_least() {
	local version=$($1 -V 2>/dev/null)
	version=${version%%-*}
	shift

	version_at_least "$version" "$@"
}

# check if lvm shell is build-in  (needs readline)
have_readline() {
	echo version | lvm &>/dev/null
}

dmsetup_wrapped() {
	udev_wait
	dmsetup "$@"
}

test -f DEVICES && devs=$(< DEVICES)

if test "$1" = dmsetup; then
    shift
    dmsetup_wrapped "$@"
else
    "$@"
fi
