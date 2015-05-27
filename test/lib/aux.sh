#!/usr/bin/env bash
# Copyright (C) 2011-2015 Red Hat, Inc. All rights reserved.
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
	if test -z "${installed_testsuite+varset}"; then
		(which clvmd 2>/dev/null | grep -q "$abs_builddir") || skip
	fi

	test -e "$DM_DEV_DIR/control" || dmsetup table >/dev/null # create control node
	# skip if singlenode is not compiled in
	(clvmd --help 2>&1 | grep "Available cluster managers" | grep -q "singlenode") || skip

#	lvmconf "activation/monitoring = 1"
	local run_valgrind=
	test "${LVM_VALGRIND_CLVMD:-0}" -eq 0 || run_valgrind="run_valgrind"
	rm -f "$CLVMD_PIDFILE"
	echo "<======== Starting CLVMD ========>"
	# lvs is executed from clvmd - use our version
	LVM_LOG_FILE_EPOCH=CLVMD LVM_BINARY=$(which lvm) $run_valgrind clvmd -Isinglenode -d 1 -f &
	echo $! > LOCAL_CLVMD

	for i in {1..100} ; do
		test $i -eq 100 && die "Startup of clvmd is too slow."
		test -e "$CLVMD_PIDFILE" -a -e "${CLVMD_PIDFILE%/*}/lvm/clvmd.sock" && break
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
	if test -z "${installed_testsuite+varset}"; then
		(which dmeventd 2>/dev/null | grep -q "$abs_builddir") || skip
	fi
	lvmconf "activation/monitoring = 1"

	local run_valgrind=
	test "${LVM_VALGRIND_DMEVENTD:-0}" -eq 0 || run_valgrind="run_valgrind"
	LVM_LOG_FILE_EPOCH=DMEVENTD $run_valgrind dmeventd -f "$@" &
	echo $! > LOCAL_DMEVENTD

	# FIXME wait for pipe in /var/run instead
	for i in {1..100} ; do
		test $i -eq 100 && die "Startup of dmeventd is too slow."
		test -e "${DMEVENTD_PIDFILE}" && break
		sleep .2
	done
	echo ok
}

prepare_lvmetad() {
	rm -f debug.log strace.log
	# skip if we don't have our own lvmetad...
	if test -z "${installed_testsuite+varset}"; then
		(which lvmetad 2>/dev/null | grep -q "$abs_builddir") || skip
	fi

	local run_valgrind=
	test "${LVM_VALGRIND_LVMETAD:-0}" -eq 0 || run_valgrind="run_valgrind"

	kill_sleep_kill_ LOCAL_LVMETAD ${LVM_VALGRIND_LVMETAD:-0}

	# Avoid reconfiguring, if already set to use_lvmetad
	(grep use_lvmetad CONFIG_VALUES 2>/dev/null | tail -n 1 | grep -q 1) || \
		aux lvmconf "global/use_lvmetad = 1" "devices/md_component_detection = 0"
	# Default debug is "-l all" and could be override
	# by setting LVM_TEST_LVMETAD_DEBUG_OPTS before calling inittest.
	echo "preparing lvmetad..."
	$run_valgrind lvmetad -f "$@" -s "$TESTDIR/lvmetad.socket" \
		${LVM_TEST_LVMETAD_DEBUG_OPTS--l all} "$@" &
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
		# Ignore results here...
		LVM_LOG_FILE_EPOCH= pvscan --cache "$@" || true
		rm -f debug.log
	fi
}

prepare_lvmpolld() {
	rm -f debug.log
	# skip if we don't have our own lvmpolld...
	(which lvmpolld 2>/dev/null | grep "$abs_builddir") || skip

	lvmconf "global/use_lvmpolld = 1"

	local run_valgrind=
	test "${LVM_VALGRIND_LVMPOLLD:-0}" -eq 0 || run_valgrind="run_valgrind"

	kill_sleep_kill_ LOCAL_LVMPOLLD ${LVM_VALGRIND_LVMPOLLD:-0}

	echo "preparing lvmpolld..."
	$run_valgrind lvmpolld -f "$@" -s "$TESTDIR/lvmpolld.socket" -B "$TESTDIR/lib/lvm" -l all &
	echo $! > LOCAL_LVMPOLLD
	while ! test -e "$TESTDIR/lvmpolld.socket"; do echo -n .; sleep .1; done # wait for the socket
	echo ok
}

lvmpolld_talk() {
	local use=nc
	if type -p socat >& /dev/null; then
		use=socat
	elif echo | not nc -U "$TESTDIR/lvmpolld.socket" ; then
		echo "WARNING: Neither socat nor nc -U seems to be available." 1>&2
		echo "# failed to contact lvmpolld"
		return 1
	fi

	if test "$use" = nc ; then
		nc -U "$TESTDIR/lvmpolld.socket"
	else
		socat "unix-connect:$TESTDIR/lvmpolld.socket" -
	fi | tee -a lvmpolld-talk.txt
}

lvmpolld_dump() {
	(echo 'request="dump"'; echo '##') | lvmpolld_talk "$@"
}

teardown_devs_prefixed() {
	local prefix=$1
	local stray=${2:-0}
	local IFS=$IFS_NL
	local dm

	rm -rf "$TESTDIR/dev/$prefix"*

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

	udev_wait
}

teardown_devs() {
	# Delete any remaining dm/udev semaphores
	teardown_udev_cookies

	test ! -f MD_DEV || cleanup_md_dev
	test ! -f DEVICES || teardown_devs_prefixed "$PREFIX"

	# NOTE: SCSI_DEBUG_DEV test must come before the LOOP test because
	# prepare_scsi_debug_dev() also sets LOOP to short-circuit prepare_loop()
	if test -f SCSI_DEBUG_DEV; then
		test "${LVM_TEST_PARALLEL:-0}" -eq 1 || modprobe -r scsi_debug
	else
		test ! -f LOOP || losetup -d $(< LOOP) || true
		test ! -f LOOPFILE || rm -f $(< LOOPFILE)
	fi

	not diff LOOP BACKING_DEV >/dev/null 2>&1 || rm -f BACKING_DEV
	rm -f DEVICES LOOP

	# Attempt to remove any loop devices that failed to get torn down if earlier tests aborted
	test "${LVM_TEST_PARALLEL:-0}" -eq 1 -o -z "$COMMON_PREFIX" || {
		local stray_loops=( $(losetup -a | grep "$COMMON_PREFIX" | cut -d: -f1) )
		test ${#stray_loops[@]} -eq 0 || {
			teardown_devs_prefixed "$COMMON_PREFIX" 1
			echo "Removing stray loop devices containing $COMMON_PREFIX: ${stray_loops[@]}"
			for i in "${stray_loops[@]}" ; do losetup -d $i ; done
			# Leave test when udev processed all removed devices
			udev_wait
		}
	}
}

kill_sleep_kill_() {
	pidfile=$1
	slow=$2
	if test -s $pidfile ; then
		pid=$(< $pidfile)
		kill -TERM $pid 2>/dev/null || return 0
		if test $slow -eq 0 ; then sleep .1 ; else sleep 1 ; fi
		kill -KILL $pid 2>/dev/null || true
		wait=0
		while ps $pid > /dev/null && test $wait -le 10; do
			sleep .5
			wait=$(($wait + 1))
		done
	fi
}

print_procs_by_tag_() {
	(ps -o pid,args ehax | grep -we"LVM_TEST_TAG=${1:-kill_me_$PREFIX}") || true
}

count_processes_with_tag() {
	print_procs_by_tag_ | wc -l
}

kill_tagged_processes() {
	local pid
	local pids
	local wait

	# read uses all vars within pipe subshell
	print_procs_by_tag_ "$@" | while read -r pid wait; do
		if test -n "$pid" ; then
			echo "Killing tagged process: $pid ${wait:0:120}..."
			kill -TERM $pid 2>/dev/null || true
		fi
		pids="$pids $pid"
	done

	# wait if process exited and eventually -KILL
	wait=0
	for pid in $pids ; do
		while ps $pid > /dev/null && test $wait -le 10; do
			sleep .2
			wait=$(($wait + 1))
		done
		test $wait -le 10 || kill -KILL $pid 2>/dev/null || true
	done
}

teardown() {
	echo -n "## teardown..."
	unset LVM_LOG_FILE_EPOCH

	if test -f TESTNAME ; then

	kill_tagged_processes

	kill_sleep_kill_ LOCAL_LVMETAD ${LVM_VALGRIND_LVMETAD:-0}

	dm_table | not egrep -q "$vg|$vg1|$vg2|$vg3|$vg4" || {
		# Avoid activation of dmeventd if there is no pid
		cfg=$(test -s LOCAL_DMEVENTD || echo "--config activation{monitoring=0}")
		vgremove -ff $cfg  \
			$vg $vg1 $vg2 $vg3 $vg4 &>/dev/null || rm -f debug.log strace.log
	}

	kill_sleep_kill_ LOCAL_LVMPOLLD ${LVM_VALGRIND_LVMPOLLD:-0}

	echo -n .

	kill_sleep_kill_ LOCAL_CLVMD ${LVM_VALGRIND_CLVMD:-0}

	echo -n .

	kill_sleep_kill_ LOCAL_DMEVENTD ${LVM_VALGRIND_DMEVENTD:-0}

	echo -n .

	test -d "$DM_DEV_DIR/mapper" && teardown_devs

	echo -n .

	fi

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
	rm -f "$LOOPFILE"
	dd if=/dev/zero of="$LOOPFILE" bs=$((1024*1024)) count=0 seek=$(($size + 1)) 2> /dev/null
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
	local DEBUG_DEV

	rm -f debug.log strace.log
	test ! -f "SCSI_DEBUG_DEV" || return 0
	test -z "$LOOP"
	test -n "$DM_DEV_DIR"

	# Skip test if scsi_debug module is unavailable or is already in use
	modprobe --dry-run scsi_debug || skip
	lsmod | not grep -q scsi_debug || skip

	# Create the scsi_debug device and determine the new scsi device's name
	# NOTE: it will _never_ make sense to pass num_tgts param;
	# last param wins.. so num_tgts=1 is imposed
	touch SCSI_DEBUG_DEV
	modprobe scsi_debug dev_size_mb=$DEV_SIZE $SCSI_DEBUG_PARAMS num_tgts=1 || skip
	
	for i in {1..20} ; do
		DEBUG_DEV="/dev/$(grep -H scsi_debug /sys/block/*/device/model | cut -f4 -d /)"
		test -b "$DEBUG_DEV" && break
		sleep .1 # allow for async Linux SCSI device registration
        done
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

prepare_md_dev() {
	local level=$1
	local rchunk=$2
	local rdevs=$3

	local maj=$(mdadm --version 2>&1) || skip "mdadm tool is missing!"
	local mddev

	cleanup_md_dev

	rm -f debug.log strace.log MD_DEV MD_DEV_PV MD_DEVICES

	# Have MD use a non-standard name to avoid colliding with an existing MD device
	# - mdadm >= 3.0 requires that non-standard device names be in /dev/md/
	# - newer mdadm _completely_ defers to udev to create the associated device node
	maj=${maj##*- v}
	maj=${maj%%.*}
	[ "$maj" -ge 3 ] && \
		mddev=/dev/md/md_lvm_test0 || \
		mddev=/dev/md_lvm_test0

	mdadm --create --metadata=1.0 "$mddev" --auto=md --level $level --chunk $rchunk --raid-devices=$rdevs "${@:4}" || {
		# Some older 'mdadm' version managed to open and close devices internaly
		# and reporting non-exclusive access on such device
		# let's just skip the test if this happens.
		# Note: It's pretty complex to get rid of consequences
		#       the following sequence avoid leaks on f19
		# TODO: maybe try here to recreate few times....
		mdadm --stop "$mddev" || true
		udev_wait
		mdadm --zero-superblock "${@:4}" || true
		udev_wait
		skip "Test skipped, unreliable mdadm detected!"
	}
	test -b "$mddev" || skip "mdadm has not created device!"

	# LVM/DM will see this device
	case "$DM_DEV_DIR" in
	"/dev") readlink -f "$mddev" ;;
	*)	cp -LR "$mddev" "$DM_DEV_DIR"
		echo "$DM_DEV_DIR/md_lvm_test0" ;;
	esac > MD_DEV_PV
	echo "$mddev" > MD_DEV
	notify_lvmetad $(< MD_DEV_PV)
	printf "%s\n" "${@:4}" > MD_DEVICES
	for mddev in "${@:4}"; do
		notify_lvmetad "$mddev"
	done
}

cleanup_md_dev() {
	test -f MD_DEV || return 0

	local IFS=$IFS_NL
	local dev=$(< MD_DEV)

	udev_wait
	mdadm --stop "$dev" || true
	test "$DM_DEV_DIR" != "/dev" && rm -f "$DM_DEV_DIR/$(basename $dev)"
	notify_lvmetad $(< MD_DEV_PV)
	for dev in $(< MD_DEVICES); do
		mdadm --zero-superblock "$dev" || true
		notify_lvmetad "$dev"
	done
	udev_wait
	if [ -b "$mddev" ]; then
		# mdadm doesn't always cleanup the device node
		# sleeps offer hack to defeat: 'md: md127 still in use'
		# see: https://bugzilla.redhat.com/show_bug.cgi?id=509908#c25
		sleep 2
		rm -f "$mddev"
	fi
	rm -f MD_DEV MD_DEVICES MD_DEV_PV
}

prepare_backing_dev() {
	if test -f BACKING_DEV; then
		BACKING_DEV=$(< BACKING_DEV)
	elif test -b "$LVM_TEST_BACKING_DEVICE"; then
		BACKING_DEV=$LVM_TEST_BACKING_DEVICE
		echo "$BACKING_DEV" > BACKING_DEV
	else
		prepare_loop "$@"
	fi
}

prepare_devs() {
	local n=${1:-3}
	local devsize=${2:-34}
	local pvname=${3:-pv}
	local shift=0

	touch DEVICES
	prepare_backing_dev $(($n*$devsize))
	# shift start of PV devices on /dev/loopXX by 1M
	not diff LOOP BACKING_DEV >/dev/null 2>&1 || shift=2048
	echo -n "## preparing $n devices..."

	local size=$(($devsize*2048)) # sectors
	local count=0
	init_udev_transaction
	for i in $(seq 1 $n); do
		local name="${PREFIX}$pvname$i"
		local dev="$DM_DEV_DIR/mapper/$name"
		DEVICES[$count]=$dev
		count=$(( $count + 1 ))
		echo 0 $size linear "$BACKING_DEV" $((($i-1)*$size + $shift)) > "$name.table"
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
		blkdiscard "$d" 2>/dev/null || true
		# ensure disk header is always zeroed
		dd if=/dev/zero of="$d" bs=32k count=1
		wipefs -a "$d" 2>/dev/null || true
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

	for dev in "${DEVICES[@]}"; do
		notify_lvmetad "$dev"
	done
}


common_dev_() {
	local tgtype=$1
	local name=${2##*/}
	local offsets
	local read_ms
	local write_ms

	case "$tgtype" in
	delay)
		read_ms=${3:-0}
		write_ms=${4:-0}
		offsets=${@:5}
		if test "$read_ms" -eq 0 -a "$write_ms" -eq 0 ; then
			offsets=
		else
			test -z "${offsets[@]}" && offsets="0:"
		fi ;;
	error)  offsets=${@:3}
		test -z "${offsets[@]}" && offsets="0:" ;;
	esac

	local pos
	local size
	local type
	local pvdev
	local offset

	read pos size type pvdev offset < "$name.table"

	for fromlen in ${offsets[@]}; do
		from=${fromlen%%:*}
		len=${fromlen##*:}
		test -n "$len" || len=$(($size - $from))
		diff=$(($from - $pos))
		if test $diff -gt 0 ; then
			echo "$pos $diff $type $pvdev $(($pos + $offset))"
			pos=$(($pos + $diff))
		elif test $diff -lt 0 ; then
			die "Position error"
		fi

		case "$tgtype" in
		delay)
			echo "$from $len delay $pvdev $(($pos + $offset)) $read_ms $pvdev $(($pos + $offset)) $write_ms" ;;
		error)
			echo "$from $len error" ;;
		esac
		pos=$(($pos + $len))
	done > "$name.devtable"
	diff=$(($size - $pos))
	test "$diff" -gt 0 && echo "$pos $diff $type $pvdev $(($pos + $offset))" >>"$name.devtable"

	init_udev_transaction
	dmsetup load "$name" "$name.devtable"
	# TODO: add support for resume without udev rescan
	dmsetup resume "$name"
	finish_udev_transaction
}

# Replace linear PV device with its 'delayed' version
# Could be used to more deterministicaly hit some problems.
# Parameters: {device path} [read delay ms] [write delay ms] [offset:size]...
# Original device is restored when both delay params are 0 (or missing).
# If the size is missing, the remaing portion of device is taken
# i.e.  delay_dev "$dev1" 0 200 256:
delay_dev() {
	if test ! -f HAVE_DM_DELAY ; then
		target_at_least dm-delay 1 1 0 || skip
	fi
	touch HAVE_DM_DELAY
	common_dev_ delay "$@"
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
		    echo 0 10000000 error | dmsetup load "$dev"
		    dmsetup resume "$dev"
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
	common_dev_ error "$@"
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
	extend_filter "a|$DM_DEV_DIR/$PREFIX|"
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
	LVM_TEST_LVMETAD=${LVM_TEST_LVMETAD:-0}
	LVM_TEST_LVMPOLLD=${LVM_TEST_LVMPOLLD:-0}
	if test "$DM_DEV_DIR" = "/dev"; then
	    LVM_VERIFY_UDEV=${LVM_VERIFY_UDEV:-0}
	else
	    LVM_VERIFY_UDEV=${LVM_VERIFY_UDEV:-1}
	fi
	test -f "$config_values" || {
            cat > "$config_values" <<-EOF
activation/checks = 1
activation/monitoring = 0
activation/polling_interval = 0
activation/retry_deactivation = 1
activation/snapshot_autoextend_percent = 50
activation/snapshot_autoextend_threshold = 50
activation/udev_rules = 1
activation/udev_sync = 1
activation/verify_udev_operations = $LVM_VERIFY_UDEV
allocation/wipe_signatures_when_zeroing_new_lvs = 0
backup/archive = 0
backup/backup = 0
devices/cache_dir = "$TESTDIR/etc"
devices/default_data_alignment = 1
devices/dir = "$DM_DEV_DIR"
devices/filter = "a|.*|"
devices/global_filter = [ "a|$DM_DEV_DIR/mapper/.*pv[0-9_]*$|", "r|.*|" ]
devices/md_component_detection  = 0
devices/scan = "$DM_DEV_DIR"
devices/sysfs_scan = 1
global/abort_on_internal_errors = 1
global/cache_check_executable = "$LVM_TEST_CACHE_CHECK_CMD"
global/cache_dump_executable = "$LVM_TEST_CACHE_DUMP_CMD"
global/cache_repair_executable = "$LVM_TEST_CACHE_REPAIR_CMD"
global/detect_internal_vg_cache_corruption = 1
global/fallback_to_local_locking = 0
global/library_dir = "$TESTDIR/lib"
global/locking_dir = "$TESTDIR/var/lock/lvm"
global/locking_type=$LVM_TEST_LOCKING
global/si_unit_consistency = 1
global/thin_check_executable = "$LVM_TEST_THIN_CHECK_CMD"
global/thin_dump_executable = "$LVM_TEST_THIN_DUMP_CMD"
global/thin_repair_executable = "$LVM_TEST_THIN_REPAIR_CMD"
global/use_lvmetad = $LVM_TEST_LVMETAD
global/use_lvmpolld = $LVM_TEST_LVMPOLLD
log/activation = 1
log/file = "$TESTDIR/debug.log"
log/indent = 1
log/level = 9
log/overwrite = 1
log/syslog = 0
log/verbose = 0
EOF
	}

	local v
	for v in "$@"; do
	    echo "$v"
	done >> "$config_values"

	declare -A CONF 2>/dev/null || {
		# Associative arrays is not available
		local s
		for s in $(cut -f1 -d/ "$config_values" | sort | uniq); do
			echo "$s {"
			local k
			for k in $(grep ^"$s"/ "$config_values" | cut -f1 -d= | sed -e 's, *$,,' | sort | uniq); do
				grep "^$k" "$config_values" | tail -n 1 | sed -e "s,^$s/,	 ,"
			done
			echo "}"
			echo
		done | tee "$config" | sed -e "s,^,## LVMCONF: ,"
		return 0
	}

	local sec
	local last_sec

	# read sequential list and put into associative array
	while IFS=$IFS_NL read -r v; do
		# trim white-space-chars via echo when inserting
		CONF[$(echo ${v%%[={]*})]=${v#*/}
	done < "$config_values"

	# sort by section and iterate through them
	printf "%s\n" ${!CONF[@]} | sort | while read -r v ; do
		sec=${v%%/*} # split on section'/'param_name
		test "$sec" = "$last_sec" || {
			test -z "$last_sec" || echo "}"
			echo "$sec {"
			last_sec=$sec
		}
		echo "    ${CONF[$v]}"
	done > "$config"
	echo "}" >> "$config"

	sed -e "s,^,## LVMCONF: ," "$config"
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
	mkdir -p etc/profile
	mv -f "PROFILE_$profile_name" "etc/profile/$profile_name.profile"
}

prepare_profiles() {
	mkdir -p etc/profile
	for profile_name in $@; do
		test -L "lib/$profile_name.profile" || skip
		cp "lib/$profile_name.profile" "etc/profile/$profile_name.profile"
	done
}

apitest() {
	test -x "$TESTOLDPWD/api/$1.t" || skip
	"$TESTOLDPWD/api/$1.t" "${@:2}" && rm -f debug.log strace.log
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
	IFS=".-" read -r major minor revision <<< "$1"
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

	version_at_least "$version" "${@:2}" || {
		echo "Found $1 version $version, but requested ${*:2}." >&2
		return 1
	}
}

have_thin() {
	test "$THIN" = shared -o "$THIN" = internal || {
		echo "Thin is not built-in." >&2
		return 1;
	}
	target_at_least dm-thin-pool "$@"

	declare -a CONF
	# disable thin_check if not present in system
	if test -n "$LVM_TEST_THIN_CHECK_CMD" -a ! -x "$LVM_TEST_THIN_CHECK_CMD" ; then
		CONF[0]="global/thin_check_executable = \"\""
	fi
	if test -n "$LVM_TEST_THIN_DUMP_CMD" -a ! -x "$LVM_TEST_THIN_DUMP_CMD" ; then
		CONF[1]="global/thin_dump_executable = \"\""
	fi
	if test -n "$LVM_TEST_THIN_REPAIR_CMD" -a ! -x "$LVM_TEST_THIN_REPAIR_CMD" ; then
		CONF[2]="global/thin_repair_executable = \"\""
	fi
	if test ${#CONF[@]} -ne 0 ; then
		echo "TEST WARNING: Reconfiguring ${CONF[@]}"
		lvmconf "${CONF[@]}"
	fi
}

have_raid() {
	test "$RAID" = shared -o "$RAID" = internal || {
		echo "Raid is not built-in." >&2
		return 1;
	}
	target_at_least dm-raid "$@"
}

have_cache() {
	test "$CACHE" = shared -o "$CACHE" = internal || {
		echo "Cache is not built-in." >&2
		return 1;
	}
	target_at_least dm-cache "$@"

	declare -a CONF
	# disable cache_check if not present in system
	if test -n "$LVM_TEST_CACHE_CHECK_CMD" -a ! -x "$LVM_TEST_CACHE_CHECK_CMD" ; then
		CONF[0]="global/cache_check_executable = \"\""
	fi
	if test -n "$LVM_TEST_CACHE_DUMP_CMD" -a ! -x "$LVM_TEST_CACHE_DUMP_CMD" ; then
		CONF[1]="global/cache_dump_executable = \"\""
	fi
	if test -n "$LVM_TEST_CACHE_REPAIR_CMD" -a ! -x "$LVM_TEST_CACHE_REPAIR_CMD" ; then
		CONF[2]="global/cache_repair_executable = \"\""
	fi
	if test ${#CONF[@]} -ne 0 ; then
		echo "TEST WARNING: Reconfiguring ${CONF[@]}"
		lvmconf "${CONF[@]}"
	fi
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

awk_parse_init_count_in_lvmpolld_dump() {
	printf '%s' \
	\
	$'BEGINFILE { x=0; answ=0; FS="="; key="[[:space:]]*"vkey }' \
	$'{' \
		$'if (/.*{$/) { x++ }' \
		$'else if (/.*}$/) { x-- }' \
		$'else if ( x == 2 && $1 ~ key) { value=substr($2, 2); value=substr(value, 1, length(value) - 1); }' \
		$'if ( x == 2 && value == vvalue && $1 ~ /[[:space:]]*init_requests_count/) { answ=$2 }' \
		$'if (answ > 0) { exit 0 }' \
	$'}' \
	$'END { printf "%d", answ }'
}

check_lvmpolld_init_rq_count() {
	local ret=$(awk -v vvalue="$2" -v vkey=${3:-lvname} "$(awk_parse_init_count_in_lvmpolld_dump)" lvmpolld_dump.txt)
	test $ret -eq $1 || {
		echo "check_lvmpolld_init_rq_count failed. Expected $1, got $ret"
		return 1
	}
}

wait_pvmove_lv_ready() {
	# given sleep .1 this is about 60 secs of waiting
	local retries=${2:-300}

	if [ -e LOCAL_LVMPOLLD ]; then
		local lvid
		while : ; do
			test $retries -le 0 && die "Waiting for lvmpolld timed out"
			test -n "$lvid" || {
				lvid=$(get lv_field ${1//-/\/} vg_uuid,lv_uuid -a 2>/dev/null)
				lvid=${lvid//\ /}
				lvid=${lvid//-/}
			}
			test -z "$lvid" || {
				lvmpolld_dump > lvmpolld_dump.txt
				! check_lvmpolld_init_rq_count 1 $lvid lvid || break;
			}
			sleep .1
			retries=$((retries-1))
		done
	else
		while : ; do
			test $retries -le 0 && die "Waiting for pvmove LV to get activated has timed out"
			dmsetup info -c -o tables_loaded $1 > out 2>/dev/null|| true;
			not grep Live out >/dev/null || break
			sleep .1
			retries=$((retries-1))
		done
	fi
}

# return total memory size in kB units
total_mem() {
	while IFS=":" read -r a b ; do
		case "$a" in MemTotal*) echo ${b%% kB} ; break ;; esac
	done < /proc/meminfo
}

kernel_at_least() {
	version_at_least "$(uname -r)" "$@"
}

test -z "$LVM_TEST_AUX_TRACE" || set -x

test -f DEVICES && devs=$(< DEVICES)

if test "$1" = dmsetup; then
    shift
    dmsetup_wrapped "$@"
else
    "$@"
fi
