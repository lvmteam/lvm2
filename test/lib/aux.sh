#!/usr/bin/env bash
# Copyright (C) 2011-2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/utils

[[ -n "$BASH" ]] && set -euE -o pipefail

run_valgrind() {
	# Execute script which may use $TESTNAME for creating individual
	# log files for each execute command
	exec "${VALGRIND:-valgrind}" "$@"
}

expect_failure() {
        echo "TEST EXPECT FAILURE"
}

check_daemon_in_builddir() {
	# skip if we don't have our own daemon...
	if [[ -z "${installed_testsuite+varset}" ]]; then
		(which "$1" 2>/dev/null | grep -q "$abs_builddir") || \
			skip "$1 is not in executed path."
	fi
	rm -f debug.log strace.log
}

create_corosync_conf() {
	local COROSYNC_CONF="/etc/corosync/corosync.conf"
	local COROSYNC_NODE
	COROSYNC_NODE=$(hostname || true)

	if [[ -e "$COROSYNC_CONF" ]]; then
		if ! grep "created by lvm test suite" "$COROSYNC_CONF"; then
			rm "$COROSYNC_CONF"
		else
			mv "$COROSYNC_CONF" "$COROSYNC_CONF.prelvmtest"
		fi
	fi

	sed -e "s/@LOCAL_NODE@/$COROSYNC_NODE/" lib/test-corosync-conf > "$COROSYNC_CONF"
	echo "created new $COROSYNC_CONF"
}

create_dlm_conf() {
	local DLM_CONF="/etc/dlm/dlm.conf"

	if [[ -e "$DLM_CONF" ]]; then
		if ! grep "created by lvm test suite" "$DLM_CONF"; then
			rm "$DLM_CONF"
		else
			mv "$DLM_CONF" "$DLM_CONF.prelvmtest"
		fi
	fi
	mkdir -p "$(dirname "$DLM_CONF")"
	cp lib/test-dlm-conf "$DLM_CONF"
	echo "created new $DLM_CONF"
}

prepare_dlm() {
	pgrep dlm_controld && skip "Cannot run while existing dlm_controld process exists."
	pgrep corosync && skip "Cannot run while existing corosync process exists."

	create_corosync_conf
	create_dlm_conf

	systemctl start corosync
	sleep 1
	if ! pgrep corosync; then
		echo "Failed to start corosync."
		exit 1
	fi

	systemctl start dlm
	sleep 1
	if ! pgrep dlm_controld; then
		echo "Failed to start dlm."
		exit 1
	fi
}

create_sanlock_conf() {
	local SANLOCK_CONF="/etc/sanlock/sanlock.conf"

	if [[ -e "$SANLOCK_CONF" ]]; then
		if ! grep "created by lvm test suite" "$SANLOCK_CONF"; then
			rm "$SANLOCK_CONF"
		else
			mv "$SANLOCK_CONF" "$SANLOCK_CONF.prelvmtest"
		fi
	fi

	mkdir -p "$(dirname "$SANLOCK_CONF")"
	cp lib/test-sanlock-conf "$SANLOCK_CONF"
	echo "created new $SANLOCK_CONF"
}

prepare_sanlock() {
	pgrep sanlock && skip "Cannot run while existing sanlock process exists."

	create_sanlock_conf

	systemctl start sanlock
	if ! pgrep sanlock; then
		echo "Failed to start sanlock."
		exit 1
	fi
}

prepare_idm() {
	which seagate_ilm >/dev/null || skip "seagate_ilm command not found"
	pgrep seagate_ilm && skip "Cannot run while existing seagate_ilm process exists."

	seagate_ilm -D 0 -l 0 -L 7 -E 7 -S 7

	if ! pgrep seagate_ilm; then
		echo "Failed to start seagate_ilm."
		exit 1
	fi
}

prepare_lvmlockd() {
	pgrep lvmlockd && skip "Cannot run while existing lvmlockd process exists."

	if [[ "${LVM_TEST_LOCK_TYPE_SANLOCK:-0}" != 0 ]]; then
		# make check_lvmlockd_sanlock
		echo "Starting lvmlockd for sanlock."
		lvmlockd -o 2

	elif [[ "${LVM_TEST_LOCK_TYPE_DLM:-0}" != 0 ]]; then
		# make check_lvmlockd_dlm
		echo "Starting lvmlockd for dlm."
		lvmlockd

	elif [[ "${LVM_TEST_LOCK_TYPE_IDM:-0}" != 0 ]]; then
		# make check_lvmlockd_idm
		echo "Starting lvmlockd for idm."
		lvmlockd -g idm

	elif [[ "${LVM_TEST_LVMLOCKD_TEST_DLM:-0}" != 0 ]]; then
		# make check_lvmlockd_test
		echo "Starting lvmlockd --test (dlm)."
		lvmlockd --test -g dlm

	elif [[ "${LVM_TEST_LVMLOCKD_TEST_SANLOCK:-0}" != 0 ]]; then
		# FIXME: add option for this combination of --test and sanlock
		echo "Starting lvmlockd --test (sanlock)."
		lvmlockd --test -g sanlock -o 2

	elif [[ "${LVM_TEST_LVMLOCKD_TEST_IDM:-0}" != 0 ]]; then
		# make check_lvmlockd_test
		echo "Starting lvmlockd --test (idm)."
		lvmlockd --test -g idm

	else
		echo "Not starting lvmlockd."
		exit 0
	fi

	sleep 1
	if ! pgrep lvmlockd >LOCAL_LVMLOCKD; then
		echo "Failed to start lvmlockd."
		exit 1
	fi
}

prepare_clvmd() {
	[[ "${LVM_TEST_LOCKING:-0}" -ne 3 ]] && return # not needed

	if pgrep clvmd ; then
		skip "Cannot use fake cluster locking with real clvmd ($(pgrep clvmd)) running."
	fi

	check_daemon_in_builddir clvmd

	[[ -e "$DM_DEV_DIR/control" ]] || dmsetup table >/dev/null # create control node
	# skip if singlenode is not compiled in
	(clvmd --help 2>&1 | grep "Available cluster managers" | grep "singlenode" >/dev/null) || \
		skip "Compiled clvmd does not support singlenode for testing."

#	lvmconf "activation/monitoring = 1"
	local run_valgrind=""
	[[ "${LVM_VALGRIND_CLVMD:-0}" -eq 0 ]] || run_valgrind="run_valgrind"
	rm -f "$CLVMD_PIDFILE"
	echo "<======== Starting CLVMD ========>"
	echo -n "## preparing clvmd..."
	# lvs is executed from clvmd - use our version
	LVM_LOG_FILE_EPOCH=CLVMD LVM_LOG_FILE_MAX_LINES=1000000 $run_valgrind clvmd -Isinglenode -d 1 -f &
	echo $! > LOCAL_CLVMD

	for i in {200..0} ; do
		[[ "$i" -eq 0 ]] && die "Startup of clvmd is too slow."
		[[ -e "$CLVMD_PIDFILE" && -e "${CLVMD_PIDFILE%/*}/lvm/clvmd.sock" ]] && break
		echo -n .
		sleep .1
	done
	echo ok
}

prepare_dmeventd() {
	[[ -n "${RUNNING_DMEVENTD-}" ]] && skip "Cannot test dmeventd with real dmeventd ($RUNNING_DMEVENTD) running."

	check_daemon_in_builddir dmeventd
	lvmconf "activation/monitoring = 1"

	local run_valgrind=""
	[[ "${LVM_VALGRIND_DMEVENTD:-0}" -eq 0 ]] || run_valgrind="run_valgrind"
	echo -n "## preparing dmeventd..."
#	LVM_LOG_FILE_EPOCH=DMEVENTD $run_valgrind dmeventd -fddddl "$@" 2>&1 &
	LVM_LOG_FILE_EPOCH=DMEVENTD $run_valgrind dmeventd -fddddl -g 2 "$@" >debug.log_DMEVENTD_out 2>&1 &
	echo $! > LOCAL_DMEVENTD

	# FIXME wait for pipe in /var/run instead
	for i in {200..0} ; do
		[[ "$i" -eq 0 ]] && die "Startup of dmeventd is too slow."
		[[ -e "${DMEVENTD_PIDFILE}" ]] && break
		echo -n .
		sleep .1
	done
	echo ok
}

prepare_lvmpolld() {
	[[ -e LOCAL_LVMPOLLD ]] || lvmconf "global/use_lvmpolld = 1"

	local run_valgrind=""
	[[ "${LVM_VALGRIND_LVMPOLLD:-0}" -eq 0 ]] || run_valgrind="run_valgrind"

	kill_sleep_kill_ LOCAL_LVMPOLLD "${LVM_VALGRIND_LVMPOLLD:-0}"

	echo -n "## preparing lvmpolld..."
	$run_valgrind lvmpolld -f "$@" -s "$TESTDIR/lvmpolld.socket" -B "$TESTDIR/lib/lvm" -l all &
	echo $! > LOCAL_LVMPOLLD
	for i in {200..0} ; do
		[[ -e "$TESTDIR/lvmpolld.socket" ]] && break
		echo -n .
		sleep .1
	done # wait for the socket
	[[ "$i" -gt 0 ]] || die "Startup of lvmpolld is too slow."
	echo ok
}

lvmpolld_talk() {
	local use=nc
	if type -p socat >& /dev/null; then
		use=socat
	elif echo | not nc -U "$TESTDIR/lvmpolld.socket" ; then
		echo "WARNING: Neither socat nor nc -U seems to be available." 1>&2
		echo "## failed to contact lvmpolld."
		return 1
	fi

	if [[ "$use" = nc  ]]; then
		nc -U "$TESTDIR/lvmpolld.socket"
	else
		socat "unix-connect:$TESTDIR/lvmpolld.socket" -
	fi | tee -a lvmpolld-talk.txt
}

lvmpolld_dump() {
	(echo 'request="dump"'; echo '##') | lvmpolld_talk "${@-}"
}

prepare_lvmdbusd() {
	local lvmdbusdebug=
	local daemon
	local comm
	rm -f debug.log_LVMDBUSD_out

	kill_sleep_kill_ LOCAL_LVMDBUSD 0

	# FIXME: This is not correct! Daemon is auto started.
	echo -n "## checking lvmdbusd is NOT running..."
	if pgrep -f -l lvmdbusd | grep python3 || pgrep -x -l lvmdbusd ; then
		skip "Cannot run lvmdbusd while existing lvmdbusd process exists."
	fi
	echo ok

	# skip if we don't have our own lvmdbusd...
	echo -n "## find lvmdbusd to use..."
	if [[ -z "${installed_testsuite+varset}" ]]; then
		# NOTE: this is always present - additional checks are needed:
		daemon="$abs_top_builddir/daemons/lvmdbusd/lvmdbusd"
		if [[ -x "$daemon" ]] || chmod ugo+x "$daemon"; then
			echo "$daemon"
		else
			echo "Failed to make '$daemon' executable">&2
			return 1
		fi
		# Setup the python path so we can run
		export PYTHONPATH="$abs_top_builddir/daemons"
	else
		daemon=$(which lvmdbusd || :)
		echo "$daemon"
	fi
	[[ -x "$daemon" ]] || skip "The lvmdbusd daemon is missing."
	which python3 >/dev/null || skip "Missing python3."

	python3 -c "import pyudev, dbus, gi.repository" || skip "Missing python modules."
	python3 -c "from json.decoder import JSONDecodeError" || skip "Python json module is missing JSONDecodeError."

	# Copy the needed file to run on the system bus if it doesn't
	# already exist
	[[ ! -f /etc/dbus-1/system.d/com.redhat.lvmdbus1.conf ]] && \
		install -m 644 "$abs_top_builddir/scripts/com.redhat.lvmdbus1.conf" /etc/dbus-1/system.d/

	echo "## preparing lvmdbusd..."
	lvmconf "global/notify_dbus = 1"

	[[ "${LVM_DEBUG_LVMDBUSD:-0}" != "0" ]] && lvmdbusdebug="--debug"

	# Currently do not interfere with lvmdbusd testing of the file logging
	unset LVM_LOG_FILE_EPOCH
	unset LVM_LOG_FILE_MAX_LINES
	unset LVM_EXPECTED_EXIT_STATUS
	export LVM_DBUSD_TEST_SKIP_SIGNAL=1

	"$daemon" $lvmdbusdebug > debug.log_LVMDBUSD_out 2>&1 &
	local pid=$!

	echo -n "## checking lvmdbusd IS running..."
	if which dbus-send &>/dev/null ; then
	for i in {100..0}; do
		[[ -d "/proc/$pid" ]] || die "lvmdbusd died!"
		dbus-send --system --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames > dbus_services
		grep -q com.redhat.lvmdbus1 dbus_services && break
		sleep .1
	done
	if [[ "$i" -eq 0 ]]; then
		printf "\nFailed to serve lvm dBus service in 10 seconds.\n"
		sed -e "s,^,## DBUS_SERVICES: ," dbus_services
		ps aux
		return 1
	fi
	else
		sleep 2
	fi

	comm=
	# TODO: Is there a better check than wait 1 second and check pid?
	if ! comm=$(ps -p $pid -o comm=) >/dev/null || [[ $comm != lvmdbusd ]]; then
		printf "\nFailed to start lvmdbusd daemon\n"
		return 1
	fi
	echo "$pid" > LOCAL_LVMDBUSD
	echo ok
}

#
# Temporary solution to create some occupied thin metadata
# This heavily depends on thin metadata output format to stay as is.
# Currently it expects 2MB thin metadata and 200MB data volume size
# Argument specifies how many devices should be created.
#
prepare_thin_metadata() {
	local devices=$1
	local transaction_id=${2:-0}
	local data_block_size=${3:-128}
	local nr_data_blocks=${4:-3200}
	local i

	echo '<superblock uuid="" time="1" transaction="'"$transaction_id"'" data_block_size="'"$data_block_size"'" nr_data_blocks="'"$nr_data_blocks"'">'
	for (( i = 1; i <= devices; i++ )); do
		echo ' <device dev_id="'"$i"'" mapped_blocks="37" transaction="'"$i"'" creation_time="0" snap_time="1">'
		echo '  <range_mapping origin_begin="0" data_begin="0" length="37" time="0"/>'
		echo ' </device>'
	done
	echo "</superblock>"
}

teardown_devs_prefixed() {
	local prefix=$1
	local stray=${2:-0}
	local IFS=$IFS_NL
	local once=1
	local dm

	rm -rf "${TESTDIR:?}/dev/$prefix*"

	# Send idle message to frozen raids (with hope to unfreeze them)
	for dm in $(dm_status | grep -E "$prefix.*raid.*frozen" || true); do
		echo "## unfreezing: dmsetup message \"${dm%:*}\""
		dmsetup message "${dm%:*}" 0 "idle" &
	done

	# Resume suspended devices first
	for dm in $(dm_info name -S "name=~$PREFIX&&suspended=Suspended"); do
		[[ "$dm" != "No devices found" ]] || break
		echo "## resuming: dmsetup resume \"$dm\""
		dmsetup clear "$dm" &
		dmsetup resume "$dm" &
	done

	wait

	local mounts
	mounts=( $(grep "$prefix" /proc/mounts | cut -d' ' -f1) ) || true
	if [[ ${#mounts[@]} -gt 0 ]]; then
		[[ "$stray" -eq 0 ]] || echo "## removing stray mounted devices containing $prefix:" "${mounts[@]}"
		if umount -fl "${mounts[@]}"; then
			udev_wait
		fi
	fi

	# Remove devices, start with closed (sorted by open count)
	# Run 'dmsetup remove' in parallel
	rm -f REMOVE_FAILED
	#local listdevs=( $(dm_info name,open --sort open,name | grep "$prefix.*:0") )
	#dmsetup remove --deferred ${listdevs[@]%%:0} || touch REMOVE_FAILED

	# 2nd. loop is trying --force removal which can possibly 'unstuck' some blocked operations
	for i in 0 1; do
		[[ "$i" = 1 && "$stray" = 0 ]] && break  # no stray device removal
		local progress=1

		while :; do
			local sortby="name"

			# HACK: sort also by minors - so we try to close 'possibly later' created device first
			[[ "$i" = 0 ]] || sortby="-minor"

			for dm in $(dm_info name,open --separator ';'  --nameprefixes --unquoted --sort open,"$sortby" -S "name=~$prefix || uuid=~$prefix" --mangle none || true) ; do
				[[ "$dm" != "No devices found" ]] || break 2
				eval "$dm"
				local force="-f"
				if [[ "$i" = 0 ]]; then
					if [[ "$once" = 1  ]]; then
						case "$DM_NAME" in
						*pv[0-9]*) ;; # do not report removal of our own PVs
						*)
						once=0
						echo "## removing stray mapped devices with names beginning with $prefix: "
						;;
						esac
					fi
					[[ "$DM_OPEN" = 0 ]] || break  # stop loop with 1st. opened device
					force=""
				fi

				# Successfully 'remove' signals progress
				dmsetup remove $force "$DM_NAME" --mangle none && progress=1
			done

			[[ "$i" = 0 ]] || break

			[[ "$progress" = 1 ]] || break

			sleep .1
			udev_wait
			wait
			progress=0
		done # looping till there are some removed devices
	done
}

teardown_devs() {
	# Delete any remaining dm/udev semaphores
	teardown_udev_cookies
	restore_dm_mirror

	[[ -f MD_DEV ]] && cleanup_md_dev

	[[ -f DEVICES || -f RAMDISK || -f SCSI_DEBUG_DEV ]] && \
		teardown_devs_prefixed "$PREFIX"

	if [[ -f RAMDISK ]]; then
		for i in 1 2 ; do
			modprobe -r brd && { rm -f RAMDISK ;  break ; }
			sleep .1
			udev_wait
		done
	fi

	# NOTE: SCSI_DEBUG_DEV test must come before the LOOP test because
	# aux prepare_scsi_debug_dev() also sets LOOP to short-circuit aux prepare_loop()
	if [[ -f SCSI_DEBUG_DEV ]]; then
		udev_wait
		if [[ "${LVM_TEST_PARALLEL:-0}" -ne 1 ]]; then
			for i in 1 2 ; do
				modprobe -r scsi_debug && { rm -f SCSI_DEBUG_DEV ; break ; }
				sleep .1
				udev_wait
			done
		fi
	else
		[[ -f LOOP ]] && losetup -d "$(< LOOP)" || true
		[[ -f LOOPFILE ]] && rm -f "$(< LOOPFILE)"
	fi

	not diff LOOP BACKING_DEV >/dev/null 2>&1 || rm -f BACKING_DEV
	rm -f DEVICES LOOP RAMDISK

	# Attempt to remove any loop devices that failed to get torn down if earlier tests aborted
	if [[ "${LVM_TEST_PARALLEL:-0}" -ne 1 && -n "$COMMON_PREFIX" ]]; then
		local stray_loops
		stray_loops=( $(losetup -a | grep "$COMMON_PREFIX" | cut -d: -f1) ) || true
		if [[ ${#stray_loops[@]} -ne 0 ]]; then
			teardown_devs_prefixed "$COMMON_PREFIX" 1
			echo "## removing stray loop devices containing $COMMON_PREFIX:" "${stray_loops[@]}"
			for i in "${stray_loops[@]}" ; do [[ -b "$i" ]] && losetup -d "$i" || true ; done
			# Leave test when udev processed all removed devices
			udev_wait
		fi
	fi
}

kill_sleep_kill_() {
	local pidfile=$1
	local slow=$2
	local pid

	if [[ -s "$pidfile"  ]]; then
		pid=( $(< "$pidfile") ) || true
		rm -f "$pidfile"
		[[ "$pidfile" = "LOCAL_LVMDBUSD" ]] && killall -9 lvmdbusd || true
		kill -TERM "${pid[@]}" 2>/dev/null || return 0
		for i in {0..10} ; do
			ps "${pid[@]}" >/dev/null || return 0
			if [[ "$slow" -eq 0  ]]; then sleep .2 ; else sleep 1 ; fi
			kill -KILL "${pid[@]}" 2>/dev/null || true
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
	local wait

	# read uses all vars within pipe subshell
	local pids=()
	while read -r pid wait; do
		if [[ -n "$pid" ]]; then
			echo "## killing tagged process: $pid ${wait:0:120}..."
			kill -TERM "$pid" 2>/dev/null || true
		fi
		pids+=( "$pid" )
	done < <(print_procs_by_tag_ "${@-}")

	[[ ${#pids[@]} -eq 0 ]] && return

	# wait if process exited and eventually -KILL
	wait=0
	for pid in "${pids[@]}" ; do
		while ps "$pid" > /dev/null && "$wait" -le 10; do
			sleep .2
			wait=$(( wait + 1 ))
		done
		[[ "$wait" -le 10 ]] || kill -KILL "$pid" 2>/dev/null || true
	done
}

teardown() {
	local cfg
	local TEST_LEAKED_DEVICES=""
	echo -n "## teardown..."
	unset LVM_LOG_FILE_EPOCH

	if [[ -f TESTNAME  ]]; then

	if [[ ! -f SKIP_THIS_TEST  ]]; then
		# Evaluate left devices only for non-skipped tests
		TEST_LEAKED_DEVICES=$(dmsetup table | grep "$PREFIX" | \
			grep -Ev "${PREFIX}(pv|[0-9])" | \
			grep -v "$(cat ERR_DEV_NAME 2>/dev/null)" | \
			grep -v "$(cat ZERO_DEV_NAME 2>/dev/null)") || true
	fi

	kill_tagged_processes
	if [[ "${LVM_TEST_LVMLOCKD_TEST:-0}" != 0 ]]; then
		echo ""
		echo "## stopping lvmlockd in teardown"
		kill_sleep_kill_ LOCAL_LVMLOCKD 0
	fi

	dm_table | not grep -E -q "$vg|$vg1|$vg2|$vg3|$vg4" || {
		# Avoid activation of dmeventd if there is no pid
		cfg=""
		[[ -s LOCAL_DMEVENTD ]] || cfg="--config activation{monitoring=0}"
		if dm_info suspended,name | grep "^Suspended:.*$PREFIX" >/dev/null ; then
			echo "## skipping vgremove, suspended devices detected."
		else
			vgremove -ff "$cfg"  \
			"$vg" "$vg1" "$vg2" "$vg3" "$vg4" &>/dev/null || rm -f debug.log strace.log
		fi
	}

	kill_sleep_kill_ LOCAL_LVMDBUSD 0

	echo -n .

	kill_sleep_kill_ LOCAL_LVMPOLLD "${LVM_VALGRIND_LVMPOLLD:-0}"

	echo -n .

	kill_sleep_kill_ LOCAL_CLVMD "${LVM_VALGRIND_CLVMD:-0}"

	echo -n .

	kill_sleep_kill_ LOCAL_DMEVENTD "${LVM_VALGRIND_DMEVENTD:-0}"

	echo -n .

	echo "ok"

	[[ -d "$DM_DEV_DIR/mapper" ]] && teardown_devs

	fi

	if [[ -n "${TEST_LEAKED_DEVICES-}" ]]; then
		echo "## unexpected devices left dm table:"
		echo "$TEST_LEAKED_DEVICES"
		return 1
	fi

	if [[ "${LVM_TEST_PARALLEL:-0}" = 0 && -z "$RUNNING_DMEVENTD" ]]; then
		rm -f debug.log* # no trace of lvm2 command for this case
		not pgrep dmeventd &>/dev/null # printed in STACKTRACE
	fi

	if [[ -n "${TESTDIR-}" ]]; then
		cd "$TESTOLDPWD" || die "Failed to enter $TESTOLDPWD"
		# after this delete no further write is possible
		rm -rf "${TESTDIR:?}" || echo BLA
	fi

	# Remove any dangling symlink in /dev/disk (our tests can confuse udev)
	find /dev/disk -type l -exec test ! -e {} \; -print0 2>/dev/null | xargs -0 rm -f || true

	# Remove any metadata archives and backups from this test on system
	rm -f /etc/lvm/archive/"${PREFIX}"* /etc/lvm/backup/"${PREFIX}"*

	# Check if this test is leaking some 'symlinks' with our name (udev)
	LEAKED_LINKS=( $(find /dev -path "/dev/mapper/${PREFIX}*" -type l -exec test ! -e {} \; -print -o \
		-path "/dev/${PREFIX}*/" -type l -exec test ! -e {} \; -print  2>/dev/null) ) || true

	[[ "${#LEAKED_LINKS[@]}" -eq 0 ]] || echo "## removing stray symlinks the names beginning with ${PREFIX}"

	if [[ "${LVM_TEST_PARALLEL:-0}" = 0  ]]; then
		# for non parallel testing erase any dangling links prefixed with LVMTEST
		find /dev -path "/dev/mapper/${COMMON_PREFIX}*" -type l -exec test ! -e {} \; -print0 -o \
			-path "/dev/${COMMON_PREFIX}*" -type l -exec test ! -e {} \; -print0 2>/dev/null | xargs -0 rm -f || true
		LEAKED_PREFIX=${COMMON_PREFIX}
	else
		rm -f "${LEAKED_LINKS[@]}" || true
		LEAKED_PREFIX=${PREFIX}
	fi

	# Remove empty dirs with test prefix
	find /dev -type d -name "${LEAKED_PREFIX}*" -empty -delete 2>/dev/null || true

	# Fail test with leaked links as most likely somewhere is missing synchronization...
	[[ "${#LEAKED_LINKS[@]}" -eq 0 ]] || die "Test leaked these symlinks ${LEAKED_LINKS[*]}"
}

prepare_loop() {
	local size=$1
	shift # all other params are directly passed to all 'losetup' calls
	local i
	local slash

	[[ -f LOOP ]] && LOOP=$(< LOOP)
	echo -n "## preparing loop device..."

	# skip if aux prepare_scsi_debug_dev() was used
	if [[ -f SCSI_DEBUG_DEV && -f LOOP ]]; then
		echo "(skipped)"
		return 0
	fi

	[[ ! -e LOOP ]]
	[[ -n "${DM_DEV_DIR-}" ]]

	for i in 0 1 2 3 4 5 6 7; do
		[[ -e "$DM_DEV_DIR/loop$i" ]] || mknod "$DM_DEV_DIR/loop$i" b 7 $i
	done

	echo -n .

	local LOOPFILE="$PWD/test.img"
	rm -f "$LOOPFILE"
	dd if=/dev/zero of="$LOOPFILE" bs=$((1024*1024)) count=0 seek=$(( size + 1 )) 2> /dev/null
	if LOOP=$(losetup "$@" -s -f "$LOOPFILE" 2>/dev/null); then
		:
	elif LOOP=$(losetup -f) && losetup "$@" "$LOOP" "$LOOPFILE"; then
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
				losetup "$@" "$dev" "$LOOPFILE"
				LOOP=$dev
				break
			done
			[[ -z "${LOOP-}" ]] || break
		done
	fi
	[[ -n "${LOOP-}" ]] # confirm or fail
	touch NO_BLKDISCARD_Z    # loop devices do not support WRITE_ZEROS
	BACKING_DEV=$LOOP
	echo "$LOOP" > LOOP
	echo "$LOOP" > BACKING_DEV
	echo "ok ($LOOP)"
}

prepare_ramdisk() {
	local size=$1

	# if brd is unused, remove and use for test
	modprobe -r brd || return 0

	echo -n "## preparing ramdisk device..."
	modprobe brd rd_size=$((size * 1024)) rd_nr=1 || return

	BACKING_DEV=/dev/ram0
	echo "ok ($BACKING_DEV)"
	touch RAMDISK
}

prepare_real_devs() {
	lvmconf 'devices/scan = "/dev"'

	touch REAL_DEVICES

	if [[ -n "${LVM_TEST_DEVICE_LIST-}" ]]; then
		local count=0
		while read -r path; do
			REAL_DEVICES[count]=$path
			count=$((  count + 1 ))
			extend_filter "a|$path|"
			dd if=/dev/zero of="$path" bs=32k count=1
			wipefs -a "$path" 2>/dev/null || true
		done < "$LVM_TEST_DEVICE_LIST"
	fi
	printf "%s\\n" "${REAL_DEVICES[@]}" > REAL_DEVICES
}

# A drop-in replacement for 'aux prepare_loop()' that uses scsi_debug to create
# a ramdisk-based SCSI device upon which all LVM devices will be created
# - scripts must take care not to use a DEV_SIZE that will induce OOM-killer
prepare_scsi_debug_dev() {
	local DEV_SIZE=$1
	shift # rest of params directly passed to modprobe
	local DEBUG_DEV

	rm -f debug.log strace.log
	[[ -f "SCSI_DEBUG_DEV" ]] && return 0
	[[ ! -f LOOP ]]
	[[ -n "${DM_DEV_DIR-}" ]]

	# Skip test if scsi_debug module is unavailable or is already in use
	modprobe --dry-run scsi_debug || skip
	lsmod | not grep scsi_debug >/dev/null || skip

	# Create the scsi_debug device and determine the new scsi device's name
	# NOTE: it will _never_ make sense to pass num_tgts param;
	# last param wins.. so num_tgts=1 is imposed
	touch SCSI_DEBUG_DEV
	modprobe scsi_debug dev_size_mb="$(( DEV_SIZE + 2 ))" "$@" num_tgts=1 || skip

	for i in {1..20} ; do
		sleep .1 # allow for async Linux SCSI device registration
		ls /sys/block/sd*/device/model >/dev/null 2>&1 || continue
		DEBUG_DEV="/dev/$(grep -H scsi_debug /sys/block/sd*/device/model | cut -f4 -d /)"
		[[ -b "$DEBUG_DEV" ]] && break
	done
	[[ -b "$DEBUG_DEV" ]] || return 1 # should not happen

	# Create symlink to scsi_debug device in $DM_DEV_DIR
	SCSI_DEBUG_DEV="$DM_DEV_DIR/$(basename "$DEBUG_DEV")"
	echo "$SCSI_DEBUG_DEV" > SCSI_DEBUG_DEV
	echo "$SCSI_DEBUG_DEV" > BACKING_DEV
	# Setting $LOOP provides means for 'aux prepare_devs()' override
	[[ "$DEBUG_DEV" = "$SCSI_DEBUG_DEV" ]] || ln -snf "$DEBUG_DEV" "$SCSI_DEBUG_DEV"
}

cleanup_scsi_debug_dev() {
	teardown_devs
	rm -f LOOP
}

mdadm_create() {
	local devid
	local mddev

	which mdadm >/dev/null || skip "mdadm tool is missing!"

	cleanup_md_dev
	rm -f debug.log strace.log

	# try to find free MD node
	# using the old naming /dev/mdXXX
        # if we need more MD arrays test suite more likely leaked them
	for devid in {127..150} ; do
		grep -q "md${devid}" /proc/mdstat || break
	done
	[[ "$devid" -lt "150" ]] || skip "Cannot find free /dev/mdXXX node!"
	mddev=/dev/md${devid}

	mdadm --create "$mddev" "$@" || {
		# Some older 'mdadm' version managed to open and close devices internally
		# and reporting non-exclusive access on such device
		# let's just skip the test if this happens.
		# Note: It's pretty complex to get rid of consequences
		#       the following sequence avoid leaks on f19
		# TODO: maybe try here to recreate few times....
		mdadm --stop "$mddev" || true
		udev_wait
		while  [ "$#" -ne 0 ] ; do
			case "$1" in
			*"$PREFIX"*) mdadm --zero-superblock "$1" || true ;;
			esac
			shift
		done
		udev_wait
		skip "Test skipped, unreliable mdadm detected!"
	}

	for i in {10..0} ; do
		[[ -e "$mddev" ]] && break
		echo "Waiting for $mddev."
		sleep .5
	done

	[[ -b "$mddev" ]] || skip "mdadm has not created device!"
	echo "$mddev" > MD_DEV

	# LVM/DM will see this device
	case "$DM_DEV_DIR" in
	"/dev") echo "$mddev" > MD_DEV_PV ;;
	*)	rm -f "$DM_DEV_DIR/md${devid}"
		cp -LR "$mddev" "$DM_DEV_DIR"
		echo "${DM_DEV_DIR}/md${devid}" > MD_DEV_PV ;;
	esac

	rm -f MD_DEVICES
	while  [ "$#" -ne 0 ] ; do
		case "$1" in
		*"$PREFIX"*) echo "$1" >> MD_DEVICES ;;
		esac
		shift
	done
}

mdadm_assemble() {
	STRACE=
	mdadm -V 2>&1 | grep " v3.2" && {
		# use this 'trick' to slow down mdadm which otherwise
		# is racing with udev rule since mdadm internally
		# opens and closes raid leg devices in RW mode and then
		# tries to get exclusive access to the leg device during
		# insertion to kernel and fails during assembly
		# There can be some other affected version of mdadm.
		STRACE="strace -f -o /dev/null"
	}

	$STRACE mdadm --assemble "$@" || { [[ -n "$STRACE" ]] && skip "Timing failure" ; false ; }
	udev_wait
}

cleanup_md_dev() {
	local IFS=$IFS_NL
	local i
	local dev
	local base
	local mddev

	[[ -f MD_DEV ]] || return 0
	mddev=$(< MD_DEV)
	base=$(basename "$mddev")

	# try to find and remove any DM device on top of cleaned MD
	# assume  /dev/mdXXX is  9:MINOR
	local minor=${mddev##/dev/md}
	for i in $(dmsetup table | grep 9:"$minor" | cut -d: -f1) ; do
		dmsetup remove "$i" || {
			dmsetup --force remove "$i" || true
		}
	done

	for i in {0..10} ; do
		grep -q "$base" /proc/mdstat || break
		if [[ "$i" != 0 ]]; then
			sleep .1
			echo "$mddev is still present, stopping again"
			cat /proc/mdstat
		fi
		mdadm --stop "$mddev" || true
		udev_wait  # wait till events are process, not zeroing to early
	done

	[[ "$DM_DEV_DIR" = "/dev" ]] || rm -f "$(< MD_DEV_PV)"

	for dev in $(< MD_DEVICES); do
		mdadm --zero-superblock "$dev" 2>/dev/null || true
	done
	udev_wait
	rm -f MD_DEV MD_DEVICES MD_DEV_PV
}

wipefs_a() {
	local have_wipefs=

	if [[ -e HAVE_WIPEFS ]]; then
		have_wipefs=$(< HAVE_WIPEFS)
	else
		wipefs -V >HAVE_WIPEFS 2>/dev/null && have_wipefs=yes
	fi

	udev_wait

	for dev in "$@"; do
		[[ "${LVM_TEST_DEVICES_FILE:-0}" != 0 ]] && lvmdevices --deldev "$dev" || true

		if [[ -n "$have_wipefs" ]]; then
			wipefs -a "$dev" || {
				echo "$dev: device in-use, retrying wipe again."
				sleep .1
				udev_wait
				wipefs -a "$dev"
			}
		else
			dd if=/dev/zero of="$dev" bs=4096 count=8 oflag=direct >/dev/null || true
			mdadm --zero-superblock "$dev" 2>/dev/null || true
		fi

		[[ "${LVM_TEST_DEVICES_FILE:-0}" != 0 ]] && lvmdevices --adddev "$dev" || true
	done

	udev_wait
}

cleanup_idm_context() {
	local dev=$1
	local sg_dev

	if [[ "${LVM_TEST_LOCK_TYPE_IDM:-0}" != 0 ]]; then
		sg_dev=$(sg_map26 "${dev}")
		echo "Cleanup IDM context for drive ${dev} ($sg_dev)"
		sg_raw -v -r 512 -o idm_tmp_data.bin "$sg_dev" \
			88 00 01 00 00 00 00 20 FF 01 00 00 00 01 00 00
		sg_raw -v -s 512 -i idm_tmp_data.bin "$sg_dev" \
			8E 00 FF 00 00 00 00 00 00 00 00 00 00 01 00 00
		rm idm_tmp_data.bin
	fi
}


#
# clear device either with blkdiscard -z or fallback to 'dd'
# $1  device_path
# TODO: add support for parametrized [OPTION] usage (Not usable ATM)
# TODO: -bs  blocksize  (defaults 512K)
# TODO: -count  count/length  (defaults to whole device, otherwise in BS units)
# TODO: -seek  offset/seek  (defaults 0, beginning of zeroing area in BS unit)
clear_devs() {
	local bs=
	local count=
	local seek=

	while [ "$#" -ne 0 ] ; do
		case "$1" in
		"") ;;
		"--bs") bs=$2; shift ;;
		"--count") count=$2; shift ;;
		"--seek") seek=$2; shift ;;
		*TEST*) # Protection: only test devices with TEST in its path name can be zeroed
			if [[ ! -e NO_BLKDISCARD_Z ]]; then
				if blkdiscard -f -z "$1" ; then
					shift
					continue
				fi
				echo "Info: can't use 'blkdiscard -z' switch to 'dd'."
				touch NO_BLKDISCARD_Z
			fi

			dd if=/dev/zero of="$1" bs=512K oflag=direct $seek $count || true
			;;
		esac
		shift
	done
}

#
# corrupt device content
# $1  file_path
# $2  string/pattern search for corruption
# $3  string/pattern replacing/corrupting
corrupt_dev() {
	local a

	# search for string on a file
	# Note: returned string may possibly start with other ASCII chars
	# a[0] is position in file,  a[1] is the actual string
	a=( $(strings -t d -n 64 "$1" | grep -m 1 "$2") ) || true

	[[ -n "${a[0]-}" ]] || return 0

	# Seek for the sequence and replace it with corruption pattern
	echo -n "${a[1]/$2/$3}" | LANG=C dd of="$1" bs=1 seek="${a[0]}" conv=fdatasync
}

prepare_backing_dev() {
	local size=${1=32}
	shift

	if [[ -n "${LVM_TEST_BACKING_DEVICE-}" ]]; then
		IFS=',' read -r -a BACKING_DEVICE_ARRAY <<< "$LVM_TEST_BACKING_DEVICE"

		for d in "${BACKING_DEVICE_ARRAY[@]}"; do
			if [[ ! -b "$d" ]]; then
				echo "Device $d doesn't exist!"
				return 1
			fi
		done
	fi

	if [[ -f BACKING_DEV ]]; then
		BACKING_DEV=$(< BACKING_DEV)
		return 0
	elif [[ -n "${LVM_TEST_BACKING_DEVICE-}" ]]; then
		BACKING_DEV=${BACKING_DEVICE_ARRAY[0]}
		echo "$BACKING_DEV" > BACKING_DEV
		return 0
	elif [[ "${LVM_TEST_PREFER_BRD-1}" = "1" && \
	     ! -d /sys/block/ram0 ]] && \
	     kernel_at_least 4 16 0 && \
	     [[ "$size" -lt 16384 ]]; then
		# try to use ramdisk if possible, but for
		# big allocs (>16G) do not try to use ramdisk
		# Also we can't use BRD device prior kernel 4.16
		# since they were DAX based and lvm2 often relies
		# in save table loading between exiting backend device
		# and  bio-based 'error' device.
		# However with request based DAX brd device we get this:
		# device-mapper: ioctl: can't change device type after initial table load.
		prepare_ramdisk "$size" "$@" && return
		echo "(failed)"
	fi

	prepare_loop "$size" "$@"
}

prepare_devs() {
	local n=${1:-3}
	local devsize=${2:-34}
	local pvname=${3:-pv}
	local header_shift=1 # shift header from begin & end of device by 1MiB
	local cnt

	# sanlock requires more space for the internal sanlock lv
	# This could probably be lower, but what are the units?
	[[ "${LVM_TEST_LOCK_TYPE_SANLOCK:-0}" != 0 ]] && devsize=1024

	touch DEVICES
	prepare_backing_dev $(( n * devsize + 2 * header_shift ))
	[[ -e NO_BLKDISCARD_Z ]] || { blkdiscard "$BACKING_DEV" 2>/dev/null || true; }
	echo -n "## preparing $n devices..."

	local size=$(( devsize * 2048 )) # sectors
	local table=()
	local concise=()
	for (( i = 0; i < n; i++ )); do
		local name="${PREFIX}$pvname$(( i + 1 ))"
		local dev="$DM_DEV_DIR/mapper/$name"
		DEVICES[i]=$dev
		# If the backing device number can meet the requirement for PV devices,
		# then allocate a dedicated backing device for PV; otherwise, rollback
		# to use single backing device for device-mapper.
		if [[ -n "$LVM_TEST_BACKING_DEVICE" && "$n" -le ${#BACKING_DEVICE_ARRAY[@]} ]]; then
			table[i]="0 $size linear ${BACKING_DEVICE_ARRAY[i]} $(( header_shift * 2048 ))"
		else
			table[i]="0 $size linear $BACKING_DEV $(( i * size + ( header_shift * 2048 ) ))"
		fi
		concise[i]="$name,TEST-$name,,,${table[i]}"
		echo "${table[i]}" > "$name.table"
	done

	dmsetup create --concise "$(printf '%s;' "${concise[@]}")" || {
		if [[ -z "${LVM_TEST_BACKING_DEVICE-}" ]]; then
			echo "failed"
			return 1
		fi
		LVM_TEST_BACKING_DEVICE=
		rm -f BACKING_DEV CREATE_FAILED
		prepare_devs "$@"
		return $?
	}

	if [[ -n "${LVM_TEST_BACKING_DEVICE-}" ]]; then
		for d in "${BACKING_DEVICE_ARRAY[@]}"; do
			cnt=$(( $(blockdev --getsize64 "$d") / 1024 / 1024 ))
			cnt=$(( cnt < 1000 ? cnt : 1000 ))
			dd if=/dev/zero of="$d" bs=1MB count=$cnt
			wipefs -a "$d" 2>/dev/null || true
			cleanup_idm_context "$d"
		done
	fi

	# non-ephemeral devices need to be cleared between tests
	[[ -f LOOP || -f RAMDISK ]] || for d in "${DEVICES[@]}"; do
		# ensure disk header is always zeroed
		dd if=/dev/zero of="$d" bs=32k count=1
		wipefs -a "$d" 2>/dev/null || true
	done

	if [[ "${LVM_TEST_DEVICES_FILE:-0}" != 0 ]]; then
		mkdir -p "$LVM_SYSTEM_DIR/devices" || true
		rm -f "$LVM_SYSTEM_DIR/devices/system.devices"
		touch "$LVM_SYSTEM_DIR/devices/system.devices"
		for d in "${DEVICES[@]}"; do
			lvmdevices --adddev "$d" || true
		done
	fi

	#for i in `seq 1 $n`; do
	#	local name="${PREFIX}$pvname$i"
	#	dmsetup info -c $name
	#done
	#for i in `seq 1 $n`; do
	#	local name="${PREFIX}$pvname$i"
	#	dmsetup table $name
	#done

	printf "%s\\n" "${DEVICES[@]}" > DEVICES
#	( IFS=$'\n'; echo "${DEVICES[*]}" ) >DEVICES
	echo "ok"
}

common_dev_() {
	local tgtype=$1
	local dev=$2
	local name=${dev##*/}
	shift 2
	local read_ms=${1-0}
	local write_ms=${2-0}

	case "$tgtype" in
	delay)
		if [[ "$read_ms" -eq 0 && "$write_ms" -eq 0 ]]; then
			# zero delay is just equivalent to 'enable_dev'
			enable_dev "$dev"
			return
		fi
		shift 2
		;;
	delayzero)
		shift 2
		# zero delay is just equivalent to 'zero_dev'
		[[ "$read_ms" -eq 0 && "$write_ms" -eq 0 ]] && tgtype="zero"
		;;
	# error|zero target does not take read_ms & write_ms only offset list
	esac

	local pos
	local size
	local type
	local pvdev
	local offset
	local from
	local len
	local diff
	local err_dev
	local zero_dev

	read -r pos size type pvdev offset < "$name.table"

	# Cache device values to avoid repeated cat calls in loop
	[[ -f ERR_DEV ]] && err_dev=$(< ERR_DEV)
	[[ -f ZERO_DEV ]] && zero_dev=$(< ZERO_DEV)

	for fromlen in "${@-0:}"; do
		from=${fromlen%%:*}
		len=${fromlen##*:}
		if [[ "$len" = "$fromlen" ]]; then
			# Missing the colon at the end: empty len
			len=
		fi
		[[ -n "$len" ]] || len=$(( size - from ))
		diff=$(( from - pos ))
		if [[ $diff -gt 0  ]]; then
			echo "$pos $diff $type $pvdev $(( pos + offset ))"
			pos=$(( pos + diff ))
		elif [[ $diff -lt 0  ]]; then
			die "Position error"
		fi

		case "$tgtype" in
		delay)
			echo "$from $len delay $pvdev $(( pos + offset )) $read_ms $pvdev $(( pos + offset )) $write_ms" ;;
		writeerror)
			echo "$from $len delay $pvdev $(( pos + offset )) 0 $err_dev 0 0" ;;
		delayzero)
			echo "$from $len delay $zero_dev 0 $read_ms $zero_dev 0 $write_ms" ;;
		error|zero)
			echo "$from $len $tgtype" ;;
		esac
		pos=$(( pos + len ))
	done > "$name.devtable"
	diff=$(( size - pos ))
	[[ "$diff" -gt 0 ]] && echo "$pos $diff $type $pvdev $(( pos + offset ))" >>"$name.devtable"

	restore_from_devtable "$dev"
}

# Replace linear PV device with its 'delayed' version
# Could be used to more deterministically hit some problems.
# Parameters: {device path} [read delay ms] [write delay ms] [offset[:[size]]]...
# Original device is restored when both delay params are 0 (or missing).
# If the size is missing, the remaining portion of device is taken
# i.e. aux delay_dev "$dev1" 0 200 256:
delay_dev() {
	if [[ ! -f HAVE_DM_DELAY  ]]; then
		target_at_least dm-delay 1 1 0 || return 0
		touch HAVE_DM_DELAY
	fi
	common_dev_ delay "$@"
}

disable_dev() {
	local dev
	local error=""
	local notify=""
	local maj
	local min

	while [[ -n "$1" ]]; do
	    if [[ "$1" = "--error" ]]; then
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
		if [[ -n "$error" ]]; then
		    echo 0 10000000 error | dmsetup load "$dev"
		    dmsetup resume "$dev"
		else
		    dmsetup remove -f "$dev" 2>/dev/null || true
		fi
	done
}

enable_dev() {
	local dev

	rm -f debug.log strace.log
	init_udev_transaction
	for dev in "$@"; do
		local name=${dev##*/}
		dmsetup create -u "TEST-$name" "$name" "$name.table" 2>/dev/null || \
			dmsetup load "$name" "$name.table"
		# using device name (since device path does not exists yes with udev)
		dmsetup resume "$name"
	done
	finish_udev_transaction
}

# Try to remove list of DM device from table
remove_dm_devs() {
	local remove=( "$@" )
	local held
	local i

	for i in {1..50}; do
		held=()
		for d in "${remove[@]}" ; do
			dmsetup remove "$d" 2>/dev/null || {
				dmsetup info -c "$d" 2>/dev/null && {
					held+=( "$d" )
					dmsetup status "$d"
				}
			}
		done
		if [[ ${#held[@]} -eq 0 ]]; then
		        rm -f debug.log*
			return
		fi
		remove=( "${held[@]}" )
	done
	die "Can't remove device(s) ${held[*]}"
}

# Throttle down performance of kcopyd when mirroring i.e. disk image
throttle_sys="/sys/module/dm_mirror/parameters/raid1_resync_throttle"
throttle_dm_mirror() {
	# if the kernel config file is present, validate whether the kernel uses HZ_1000
	# and return failure for this 'throttling' when it does NOT as without this setting
	# whole throttling is pointless on modern hardware
	local kconfig

	kconfig="/boot/config-$(uname -r)"
	if [[ -e "$kconfig"  ]]; then
		grep -q "CONFIG_HZ_1000=y" "$kconfig" 2>/dev/null || {
			echo "WARNING: CONFIG_HZ_1000=y is NOT set in $kconfig -> throttling is unusable"
			return 1
		}
	fi
	[[ -e "$throttle_sys" ]] || return
	[[ -f THROTTLE ]] || cat "$throttle_sys" > THROTTLE
	echo "${1-1}" > "$throttle_sys"
}

# Restore original kcopyd throttle value and have mirroring fast again
restore_dm_mirror() {
	if [[ -f THROTTLE ]]; then
		cat THROTTLE > "$throttle_sys"
		rm -f THROTTLE
	fi
}


# Once there is $name.devtable
# this is a quick way to restore to this table entry
restore_from_devtable() {
	local dev

	rm -f debug.log strace.log
	init_udev_transaction
	for dev in "$@"; do
		local name=${dev##*/}
		dmsetup load "$name" "$name.devtable"
		if not dmsetup resume "$name" ; then
			dmsetup clear "$name"
			dmsetup resume "$name"
			finish_udev_transaction
			echo "Device $name has unusable table \"$(cat "$name.devtable")\""
			return 1
		fi
	done
	finish_udev_transaction
}

#
# Convert device to device with errors
# Takes the list of pairs of error segment from:len
# Combination with zero or delay is unsupported
# Original device table is replaced with multiple lines
# i.e.  error_dev "$dev1" 8:32 96:8
error_dev() {
	common_dev_ error "$@"
}

# New kernels do not allow to create devices bigger the <8EiB
# This gives 18014398509481983 as maximum usable sectors
#
# MAX_DEV_SIZE=18014398509481983
#
# Interestingly older kernels (3.X series) do have some strange problems
# if there is a device of such size  (systemd-udev runs in endless loop:
#   truncate_inode_pages_range+0x1ed/0x5e0
#   truncate_inode_pages+0x1f/0x30
#   kill_bdev+0x26/0x30
#
# As we don't really care about couple missing sectors here, just lower
# the size to the max usable size that is not cause troubles:
MAX_DEV_SIZE=18014398509481976

#
# Convert device to device with write errors but normal reads.
# For this 'delay' dev is used and reroutes 'reads' back to original device
# and for writes it will use extra new TEST-errordev (huge error target)
# i.e.  writeerror_dev "$dev1" 8:32
writeerror_dev() {
	local name=${PREFIX}-errordev

	if [[ ! -e ERR_DEV ]]; then
		# delay target is used for error mapping
		if [[ ! -f HAVE_DM_DELAY  ]]; then
			target_at_least dm-delay 1 1 0 || return 0
			touch HAVE_DM_DELAY
		fi
		# error device with the size of 8EiB
		dmsetup create -u "TEST-$name" "$name" --table "0 $MAX_DEV_SIZE error"
		# Take major:minor of our error device
		echo "$name" > ERR_DEV_NAME
		dmsetup info -c  --noheadings -o major,minor "$name" > ERR_DEV
	fi

	common_dev_ writeerror "$@"
}

#
# Convert device to device with sections of delayed zero read and writes.
# For this 'delay' dev will use extra new TEST-zerodev (huge zero target)
# and reroutes reads and writes
# i.e.  delayzero_dev "$dev1" 8:32
delayzero_dev() {
	local name=${PREFIX}-zerodev

	if [[ ! -e ZERO_DEV ]]; then
		# delay target is used for error mapping
		if [[ ! -f HAVE_DM_DELAY  ]]; then
			target_at_least dm-delay 1 1 0 || return 0
			touch HAVE_DM_DELAY
		fi
		# error device with the size of 8EiB
		dmsetup create -u "TEST-$name" "$name" --table "0 $MAX_DEV_SIZE zero"
		# Take major:minor of our error device
		echo "$name" > ZERO_DEV_NAME
		dmsetup info -c  --noheadings -o major,minor "$name" > ZERO_DEV
	fi

	common_dev_ delayzero "$@"
}

#
# Convert existing device to a device with zero segments
# Takes the list of pairs of zero segment from:len
# Combination with error or delay is unsupported
# Original device table is replaced with multiple lines
# i.e.  zero_dev "$dev1" 8:32 96:8
zero_dev() {
	common_dev_ zero "$@"
}

backup_dev() {
	local dev

	for dev in "$@"; do
		dd if="$dev" of="${dev##*/}.backup" bs=16K conv=fdatasync || \
			die "Cannot backup device: \"$dev\"  with size $(blockdev --getsize64 "$dev" || true) bytes."
	done
}

restore_dev() {
	local dev

	for dev in "$@"; do
		[[ -e "${dev##*/}.backup" ]] || \
			die "Internal error: $dev not backed up, can't restore!"
		dd of="$dev" if="${dev##*/}.backup" bs=16K
	done
}

prepare_pvs() {
	prepare_devs "$@"
	pvcreate -ff "${DEVICES[@]}"
}

prepare_vg() {
	teardown_devs

	prepare_devs "$@"
	vgcreate $SHARED -s 512K "$vg" "${DEVICES[@]}"
}

extend_devices() {
	[[ "${LVM_TEST_DEVICES_FILE:-0}" = 0 ]] && return

	for dev in "$@"; do
		lvmdevices --adddev "$dev"
	done
}

extend_filter() {
	local filter

	[[ "${LVM_TEST_DEVICES_FILE:-0}" != 0 ]] && return

	filter=$(grep ^devices/global_filter CONFIG_VALUES | tail -n 1)
	for rx in "$@"; do
		filter=$(echo "$filter" | sed -e "s:\\[:[ \"$rx\", :")
	done
	lvmconf "$filter" "devices/scan_lvs = 1"
}

extend_filter_md() {
	local filter

	[[ "${LVM_TEST_DEVICES_FILE:-0}" != 0 ]] && return

	filter=$(grep ^devices/global_filter CONFIG_VALUES | tail -n 1)
	for rx in "$@"; do
		filter=$(echo "$filter" | sed -e "s:\\[:[ \"$rx\", :")
	done
	lvmconf "$filter" "devices/scan = [ \"$DM_DEV_DIR\", \"/dev\" ]"
}

extend_filter_LVMTEST() {
	extend_filter "a|$DM_DEV_DIR/$PREFIX|" "$@"
}

hide_dev() {
	local filter

	if [[ "${LVM_TEST_DEVICES_FILE:-0}" != 0 ]]; then
		for dev in "$@"; do
			lvmdevices --deldev "$dev"
		done
	else
		filter=$(grep ^devices/global_filter CONFIG_VALUES | tail -n 1)
		for dev in "$@"; do
			filter=$(echo "$filter" | sed -e "s:\\[:[ \"r|$dev|\", :")
		done
		lvmconf "$filter"
	fi
}

unhide_dev() {
	local filter

	if [[ "${LVM_TEST_DEVICES_FILE:-0}" != 0 ]]; then
		for dev in "$@"; do
			lvmdevices -y --adddev "$dev"
		done
	else
		filter=$(grep ^devices/global_filter CONFIG_VALUES | tail -n 1)
		for dev in "$@"; do
			filter=$(echo "$filter" | sed -e "s:\"r|$dev|\", ::")
		done
		lvmconf "$filter"
	fi
}

mkdev_md5sum() {
	rm -f debug.log strace.log
	mkfs.ext2 "$DM_DEV_DIR/$1/$2" || return 1
	md5sum "$DM_DEV_DIR/$1/$2" > "md5.$1-$2"
}

generate_config() {
	local config_values
	local config
	if [[ -n "$profile_name" ]]; then
		config_values="PROFILE_VALUES_$profile_name"
		config="PROFILE_$profile_name"
		touch "$config_values"
	else
		config_values=CONFIG_VALUES
		config=CONFIG
	fi

	LVM_TEST_LOCKING=${LVM_TEST_LOCKING:-1}
	LVM_TEST_LVMPOLLD=${LVM_TEST_LVMPOLLD:-0}
	LVM_TEST_LVMLOCKD=${LVM_TEST_LVMLOCKD:-0}
	LVM_TEST_DEVICES_FILE=${LVM_TEST_DEVICES_FILE:-0}
        # FIXME:dct: This is harmful! Variables are unused here and are tested not being empty elsewhere:
	#LVM_TEST_LOCK_TYPE_SANLOCK=${LVM_TEST_LOCK_TYPE_SANLOCK:-0}
	#LVM_TEST_LOCK_TYPE_DLM=${LVM_TEST_LOCK_TYPE_DLM:-0}
	if [[ "$DM_DEV_DIR" = "/dev" ]]; then
	    LVM_VERIFY_UDEV=${LVM_VERIFY_UDEV:-0}
	else
	    LVM_VERIFY_UDEV=${LVM_VERIFY_UDEV:-1}
	fi
	if [[ ! -f "$config_values" ]]; then
		cat > "$config_values" <<-EOF
		activation/checks = 1
		activation/monitoring = 0
		activation/polling_interval = 1
		activation/retry_deactivation = 1
		activation/snapshot_autoextend_percent = 50
		activation/snapshot_autoextend_threshold = 50
		activation/verify_udev_operations = $LVM_VERIFY_UDEV
		activation/raid_region_size = 512
		allocation/wipe_signatures_when_zeroing_new_lvs = 0
		allocation/vdo_slab_size_mb = 128
		allocation/zero_metadata = 0
		backup/archive = 0
		backup/backup = 0
		devices/cache_dir = "$LVM_SYSTEM_DIR"
		devices/default_data_alignment = 1
		devices/dir = "$DM_DEV_DIR"
		devices/md_component_detection = 0
		devices/scan = "$DM_DEV_DIR"
		devices/sysfs_scan = 1
		devices/write_cache_state = 0
		devices/use_devicesfile = $LVM_TEST_DEVICES_FILE
		devices/filter = "a|.*|"
		devices/global_filter = [ "a|$DM_DEV_DIR/mapper/${PREFIX}.*pv[0-9_]*$|", "r|.*|" ]
		global/abort_on_internal_errors = 1
		global/cache_check_executable = "$LVM_TEST_CACHE_CHECK_CMD"
		global/cache_dump_executable = "$LVM_TEST_CACHE_DUMP_CMD"
		global/cache_repair_executable = "$LVM_TEST_CACHE_REPAIR_CMD"
		global/cache_restore_executable = "$LVM_TEST_CACHE_RESTORE_CMD"
		global/detect_internal_vg_cache_corruption = 1
		global/etc = "$LVM_SYSTEM_DIR"
		global/event_activation = 1
		global/fallback_to_local_locking = 0
		global/locking_type=$LVM_TEST_LOCKING
		global/notify_dbus = 0
		global/si_unit_consistency = 1
		global/thin_check_executable = "$LVM_TEST_THIN_CHECK_CMD"
		global/thin_dump_executable = "$LVM_TEST_THIN_DUMP_CMD"
		global/thin_repair_executable = "$LVM_TEST_THIN_REPAIR_CMD"
		global/thin_restore_executable = "$LVM_TEST_THIN_RESTORE_CMD"
		global/use_lvmlockd = $LVM_TEST_LVMLOCKD
		global/use_lvmpolld = $LVM_TEST_LVMPOLLD
		log/activation = 1
		log/file = "$TESTDIR/debug.log"
		log/indent = 1
		log/level = 9
		log/overwrite = 1
		log/syslog = 0
		log/verbose = 0
		EOF
		# For 'rpm' builds use system installed binaries
		# and libraries and locking dir and some more built-in
		# defaults
		# For test suite run use binaries from builddir.
		if [[ -n "${abs_top_builddir+varset}" ]]; then
			cat >> "$config_values" <<-EOF
			dmeventd/executable = "$abs_top_builddir/test/lib/dmeventd"
			activation/udev_rules = 1
			activation/udev_sync = 1
			global/fsadm_executable = "$abs_top_builddir/test/lib/fsadm"
			global/lvresize_fs_helper_executable = "$abs_top_builddir/test/lib/lvresize_fs_helper"
			global/library_dir = "$TESTDIR/lib"
			global/locking_dir = "$TESTDIR/var/lock/lvm"
			EOF
		fi
	fi

	# append all parameters  (avoid adding empty \n)
	local v
	[[ $# -gt 0 ]] && printf "%s\\n" "$@" >> "$config_values"

	declare -A CONF 2>/dev/null || {
		# Associative arrays is not available
		local s
		for s in $(cut -f1 -d/ "$config_values" | sort | uniq); do
			echo "$s {"
			local k
			for k in $(grep ^"$s"/ "$config_values" | cut -f1 -d= | sed -e 's, *$,,' | sort | uniq); do
				grep "^${k}[ \t=]" "$config_values" | tail -n 1 | sed -e "s,^$s/,	 ," || true
			done
			echo "}"
			echo
		done | tee "$config" | sed -e "s,^,## LVMCONF: ,"
		return 0
	}

	local sec
	local last_sec=""

	# read sequential list and put into associative array
	while IFS= read -r v; do
		CONF["${v%%[={ ]*}"]=${v#*/}
	done < "$config_values"

	# sort by section and iterate through them
	printf "%s\\n" "${!CONF[@]}" | sort | while read -r v ; do
		sec=${v%%/*} # split on section'/'param_name
		if [[ "$sec" != "$last_sec" ]]; then
			[[ -z "$last_sec" ]] || echo "}"
			echo "$sec {"
			last_sec=$sec
		fi
		echo "    ${CONF[$v]}"
	done > "$config"
	echo "}" >> "$config"

	sed -e "s,^,## LVMCONF: ," "$config"
}

lvmconf() {
	local val
	local profile_name=""
	if [[ $# -ne 0 ]]; then
		# Compare if passed args aren't already all in generated lvm.conf
		local needed=0
		for i in "$@"; do
			val=$(grep "${i%%[={ ]*}" CONFIG_VALUES 2>/dev/null | tail -1) || { needed=1; break; }
			[[ "$val" = "$i" ]] || { needed=1; break; }
		done
		if [[ "$needed" -eq 0 ]]; then
			echo "## Skipping reconfiguring for: (" "$@" ")"
			return 0 # not needed
		fi
	fi
	generate_config "$@"
	mv -f CONFIG "$LVM_SYSTEM_DIR/lvm.conf"
}

profileconf() {
	local pdir="$LVM_SYSTEM_DIR/profile"
	local profile_name=$1
	shift
	generate_config "$@"
	mkdir -p "$pdir"
	mv -f "PROFILE_$profile_name" "$pdir/$profile_name.profile"
}

prepare_profiles() {
	local pdir="$LVM_SYSTEM_DIR/profile"
	local profile_name
	mkdir -p "$pdir"
	for profile_name in "$@"; do
		[[ -L "lib/$profile_name.profile" ]] || skip
		cp "lib/$profile_name.profile" "$pdir/$profile_name.profile"
	done
}

unittest() {
	[[ -x "$TESTOLDPWD/unit/unit-test" ]] || skip
	"$TESTOLDPWD/unit/unit-test" "${@}"
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
# - This FAILS because kmem_cache_sanity_check collides with the existing
#   name ("foo-a") associated with the non-removed cache.
#
# This is a problem for RAID (specifically dm-raid) because the name used
# for the kmem_cache_create is ("raid%d-%p", level, mddev).  If the cache
# persists for long enough, the memory address of an old mddev will be
# reused for a new mddev - causing an identical formulation of the cache
# name.  Even though kmem_cache_destroy had long ago been used to delete
# the old cache, the merging of caches has cause the name and cache of that
# old instance to be preserved and causes a collision (and thus failure) in
# kmem_cache_create().  I see this regularly in testing the following
# kernels:
#
# This seems to be finally resolved with this patch:
# http://www.redhat.com/archives/dm-devel/2014-March/msg00008.html
# so we need to put here exclusion for kernels which do trace SLUB
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

#
# Some 32bit kernel cannot pass some erroring magic which forces
# thin-pool to be falling into Error state.
#
# Skip test on such kernels (see: https://bugzilla.redhat.com/1310661)
#
thin_pool_error_works_32() {
	case "$(uname -r)" in
	  2.6.32-618.*.i686) return 1 ;;
	  2.6.32-623.*.i686) return 1 ;;
	  2.6.32-573.1[28].1.el6.i686) return 1 ;;
	esac
}

thin_restore_needs_more_volumes() {
	# Note: new version prints thin_restore first
	case $("$LVM_TEST_THIN_RESTORE_CMD" -V) in
		# With older version of thin-tool we got slightly more compact metadata
		0.[0-6]*|0.7.0*) return 0 ;;
		0.8.5-2.el7) return 0 ;;
	esac
	return 1
}

udev_wait() {
	local arg="--timeout=15"
	[[ -n "${1-}" ]] && arg="--exit-if-exists=${1-}"

	if [[ ! -f UDEV_PID ]]; then
		pgrep udev >UDEV_PID 2>/dev/null || return 0
		which udevadm &>/dev/null || { echo "" >UDEV_PID ; return 0 ; }
	fi

	[[ -s UDEV_PID ]] && { udevadm settle "$arg" 2>/dev/null || true ; }
}

# wait_for_sync <VG/LV>
wait_for_sync() {
	local i
	for i in {1..100} ; do
		check in_sync "$@" && return
		sleep .2
	done

	echo "Sync is taking too long - assume stuck."
	echo t >/proc/sysrq-trigger 2>/dev/null
	return 1
}

wait_recalc() {
	local sync
	local checklv=$1

	for i in {1..100} ; do
		sync=$(get lv_field "$checklv" sync_percent | cut -d. -f1)
		echo "sync_percent is $sync"

		[[ "$sync" = "100" ]] && return

		sleep .1
	done

	# TODO: There is some strange bug, first leg of RAID with integrity
	# enabled never gets in sync. I saw this in BB, but not when executing
	# the commands manually
#	if test -z "$sync"; then
#		echo "TEST\ WARNING: Resync of dm-integrity device '$checklv' failed"
#                dmsetup status "$DM_DEV_DIR/mapper/${checklv/\//-}"
#		exit
#	fi
	echo "Timeout waiting for recalc."
	dmsetup status "$DM_DEV_DIR/mapper/${checklv/\//-}"
	return 1
}

# Check if tests are running on 64bit architecture
can_use_16T() {
	[[ "$(getconf LONG_BIT)" -eq 64 ]]
}

# Check if major.minor.revision' string is 'at_least'
version_at_least() {
	local major
	local minor
	local revision
	IFS=".-" read -r major minor revision <<< "$1"
	shift

	[[ -n "${1:-}" ]] || return 0
	[[ -n "$major" ]] || return 1
	[[ "$major" -gt "$1" ]] && return 0
	[[ "$major" -eq "$1" ]] || return 1

	[[ -n "${2:-}" ]] || return 0
	[[ -n "$minor" ]] || return 1
	[[ "$minor" -gt "$2" ]] && return 0
	[[ "$minor" -eq "$2" ]] || return 1

	[[ -n "${3:-}" ]] || return 0
	[[ "$revision" -ge "$3" ]] 2>/dev/null || return 1
}
#
# Check whether kernel [dm module] target exist
# at least in expected version
#
# [dm-]target-name major minor revision
#
# i.e.   dm_target_at_least  dm-thin-pool  1 0
target_at_least() {
	rm -f debug.log strace.log
	modprobe "$1" || {
		case "$1" in
		  dm-vdo) modprobe "kvdo" || true ;;
		esac
	}

	if [[ "$1" = dm-raid ]]; then
		case "$(uname -r)" in
		  3.12.0*) return 1 ;;
		esac
	fi

	local version
	version=$(dmsetup targets 2>/dev/null | grep "^${1##dm-} " 2>/dev/null)
	version=${version##* v}

	version_at_least "$version" "${@:2}" || {
		echo "Found $1 version $version, but requested ${*:2}." >&2
		return 1
	}
}

# Check whether the kernel driver version is greater or equal
# to the specified version. This can be used to skip tests on
# kernels where they are known to not be supported.
#
# e.g. aux driver_at_least 4 33
#
driver_at_least() {
	local version
	version=$(dmsetup version | tail -1 2>/dev/null)
	version=${version##*:}
	version_at_least "$version" "$@" || {
		echo "Found driver version $version, but requested" "$@" "." >&2
		return 1
	}
}

have_thin() {
	lvm segtypes 2>/dev/null | grep thin$ >/dev/null || {
		echo "Thin is not built-in." >&2
		return 1
	}
	target_at_least dm-thin-pool "$@"

	declare -a CONF=()
	# disable thin_check if not present in system
	[[ -n "$LVM_TEST_THIN_CHECK_CMD" && ! -x "$LVM_TEST_THIN_CHECK_CMD" ]] && \
		CONF[0]="global/thin_check_executable = \"\""
	[[ -n "$LVM_TEST_THIN_DUMP_CMD" && ! -x "$LVM_TEST_THIN_DUMP_CMD" ]] && \
		CONF[1]="global/thin_dump_executable = \"\""
	[[ -n "$LVM_TEST_THIN_REPAIR_CMD" && ! -x "$LVM_TEST_THIN_REPAIR_CMD" ]] && \
		CONF[2]="global/thin_repair_executable = \"\""
	if [[ ${#CONF[@]} -ne 0  ]]; then
		echo "TEST WARNING: Reconfiguring" "${CONF[@]}"
		lvmconf "${CONF[@]}"
	fi
}

have_vdo() {
	lvm segtypes 2>/dev/null | grep 'vdo$' >/dev/null || {
		echo "VDO is not built-in." >&2
		return 1
	}
	target_at_least dm-vdo "$@"
	local vdoformat

	vdoformat=$(lvm lvmconfig --typeconfig full --valuesonly global/vdo_format_executable || true)
	# Remove surrounding "" around string
	# TODO: lvmconfig should have an option to give this output directly
	vdoformat=${vdoformat//\"}
	[[ -x "$vdoformat" ]] || { echo "No executable to format VDO \"$vdoformat\"..."; return 1; }
}

have_writecache() {
	lvm segtypes 2>/dev/null | grep 'writecache$' >/dev/null || {
		echo "writecache is not built-in." >&2
		return 1
	}
	target_at_least dm-writecache "$@"
}

have_integrity() {
	lvm segtypes 2>/dev/null | grep 'integrity$' >/dev/null || {
		echo "integrity is not built-in." >&2
		return 1
	}
	target_at_least dm-integrity "$@"
}

have_raid() {
	target_at_least dm-raid "$@"

	# some kernels have broken mdraid bitmaps, don't use them!
	# may oops kernel, we know for sure all FC24 are currently broken
	# in general any 4.1, 4.2 is likely useless unless patched
	case "$(uname -r)" in
	  4.[12].*fc24*) return 1 ;;
	esac
}

have_raid_resizable() {
	# Check if the kernel can resize raid volume without killing leg.
	# MD core has had a bug in handling read-ahead requests.
	# This results in marking raid with suspended leg as failed.
	# Bug was fixed from kernel 6.16
	# Fixing kernel commit: 9f346f7d4ea73692b82f5102ca8698e4040469ea
	# Patch was backported to most 6.15 releases.
	# Note: Fixing test for this failure would likely require full raid
	# array resync, so it's better to skip the test for affected kernels.
	case "$(uname -r)" in
	  6.1[34]*) return 1 ;;
	  5.14.0-6[12]*.el9.*) return 1 ;; # Kernel is missing fixing commit!
	  6.12.0-12[456]*.el10*) return 1 ;; # -- '' --
	esac
}

have_raid4 () {
	local r=0

	have_raid 1 8 0 && r=1
	have_raid 1 9 1 && r=0

	return $r
}

have_cache() {
	lvm segtypes 2>/dev/null | grep ' cache-pool$' >/dev/null || {
		echo "Cache is not built-in." >&2
		return 1
	}
	target_at_least dm-cache "$@"

	declare -a CONF=()
	# disable cache_check if not present in system
	[[ -n "$LVM_TEST_CACHE_CHECK_CMD" && ! -x "$LVM_TEST_CACHE_CHECK_CMD" ]] && \
		CONF[0]="global/cache_check_executable = \"\""
	[[ -n "$LVM_TEST_CACHE_DUMP_CMD" && ! -x "$LVM_TEST_CACHE_DUMP_CMD" ]] && \
		CONF[1]="global/cache_dump_executable = \"\""
	[[ -n "$LVM_TEST_CACHE_REPAIR_CMD" && ! -x "$LVM_TEST_CACHE_REPAIR_CMD" ]] && \
		CONF[2]="global/cache_repair_executable = \"\""
	if [[ ${#CONF[@]} -ne 0  ]]; then
		echo "TEST WARNING: Reconfiguring" "${CONF[@]}"
		lvmconf "${CONF[@]}"
	fi
}

# detect if lvm2 was compiled with FSINFO support by checking for '--fs checksize'
# if passed 'skip' keyword - print
have_fsinfo() {
	local r
	r=$(not lvresize --fs checksize -L+1 $vg/unknownlvname 2>&1) || die "lvresize must fail!"

	case "$r" in
	*"Unknown --fs value"*) return 1 ;;
	esac
}

have_tool_at_least() {
	local version
	version=$("$1" -V 2>/dev/null)
	version=${version%%-*}
	version=${version##* }
	shift

	version_at_least "$version" "$@"
}

# check if lvm shell is built-in  (needs readline)
have_readline() {
	echo version | lvm &>/dev/null
}

have_multi_core() {
	which nproc &>/dev/null || return 0
	[[ "$(nproc)" -ne 1 ]]
}

dmsetup_wrapped() {
	udev_wait
	dmsetup "$@"
}

awk_parse_init_count_in_lvmpolld_dump() {
	printf '%s' \
	\
	$'BEGINFILE { x=0; answ=0 }' \
	$'{' \
		$'if (/.*{$/) { x++ }' \
		$'else if (/.*}$/) { x-- }' \
		$'else if ( x == 2 && $1 ~ "[[:space:]]*"vkey) { value=substr($2, 2); value=substr(value, 1, length(value) - 1); }' \
		$'if ( x == 2 && value == vvalue && $1 ~ /[[:space:]]*init_requests_count/) { answ=$2 }' \
		$'if (answ > 0) { exit 0 }' \
	$'}' \
	$'END { printf "%d", answ }'
}

check_lvmpolld_init_rq_count() {
	local ret
	ret=$(awk -v vvalue="$2" -v vkey="${3:-lvname}" -F= "$(awk_parse_init_count_in_lvmpolld_dump)" lvmpolld_dump.txt)
	if [[ "$ret" -ne "$1" ]]; then
		die "check_lvmpolld_init_rq_count failed. Expected $1, got $ret"
	fi
}

wait_pvmove_lv_ready() {
	# given sleep .1 this is about 20 secs of waiting
	local lvid=()
	local all

	for i in {100..0}; do
		if [[ -e LOCAL_LVMPOLLD ]]; then
			if [[ "${#lvid[@]}" -eq "$#"  ]]; then
				lvmpolld_dump > lvmpolld_dump.txt
				all=1
				for l in "${lvid[@]%-real}" ; do
					check_lvmpolld_init_rq_count 1 "${l##LVM-}" lvid || all=0
				done
				[[ "$all" = 1 ]] && return
			else
				# wait till wanted LV really appears
				lvid=( $(dmsetup info --noheadings -c -o uuid "$@" 2>/dev/null) ) || true
			fi
		else
			dmsetup info -c --noheadings -o tables_loaded "$@" >out 2>/dev/null || true
			[[ "$(grep -c Live out)" = "$#" ]] && return
		fi
		sleep .1
	done

	[[ -e LOCAL_LVMPOLLD ]] && die "Waiting for lvmpolld timed out"
	die "Waiting for pvmove LV to get activated has timed out"
}

# Holds device open with sleep which automatically expires after given timeout
# Prints  PID of running holding sleep process in background
hold_device_open() {
	local vgname=$1
	local lvname=$2
	local sec=${3-20} # default 20sec

	sleep "$sec" < "$DM_DEV_DIR/$vgname/$lvname" >/dev/null 2>&1 &
	SLEEP_PID=$!
	# wait till device is opened
	for i in {1..50}; do
		if [[ "$(dmsetup info --noheadings -c -o open "$vgname"-"$lvname")" -ne 0  ]]; then
			echo "$SLEEP_PID"
			return
		fi
		sleep .1
	done

	die "$vgname-$lvname expected to be opened, but it's not!"
}

# return total memory size in kB units
total_mem() {
	local a
	local b

	while IFS=":" read -r a b ; do
		case "$a" in MemTotal*) echo "${b%% kB}" ; break ;; esac
	done < /proc/meminfo
}

kernel_at_least() {
	version_at_least "$(uname -r)" "$@"
}

[[ "${LVM_TEST_AUX_TRACE-0}" = "0" ]] || set -x

[[ -f DEVICES ]] && devs=$(< DEVICES)

unset LVM_VALGRIND

if [[ "$1" = "dmsetup" ]]; then
    shift
    dmsetup_wrapped "$@"
else
    "$@"
fi
