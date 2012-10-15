#!/bin/bash
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
	exec "${VALGRIND:-valg}" "$@"
}

expect_failure() {
        echo "TEST EXPECT FAILURE"
}

prepare_clvmd() {
	test "${LVM_TEST_LOCKING:-0}" -ne 3 && return # not needed

	if pgrep clvmd ; then
		echo "Cannot use fake cluster locking with real clvmd ($(pgrep clvmd)) running."
		skip
	fi

	# skip if we don't have our own clvmd...
	(which clvmd 2>/dev/null | grep "$abs_builddir") || skip

	# skip if we singlenode is not compiled in
	(clvmd --help 2>&1 | grep "Available cluster managers" | grep "singlenode") || skip

#	lvmconf "activation/monitoring = 1"
	local run_valgrind=
	test -z "$LVM_VALGRIND_CLVMD" || run_valgrind="run_valgrind"
	$run_valgrind lib/clvmd -Isinglenode -d 1 -f &
	local local_clvmd=$!
	sleep .3
	# extra sleep for slow valgrind
	test -z "$LVM_VALGRIND_CLVMD" || sleep 7
	# check that it is really running now
	ps $local_clvmd || die
	echo $local_clvmd > LOCAL_CLVMD
}

prepare_dmeventd() {
	if pgrep dmeventd ; then
		echo "Cannot test dmeventd with real dmeventd ($(pgrep dmeventd)) running."
		skip
	fi

	# skip if we don't have our own dmeventd...
	(which dmeventd 2>/dev/null | grep "$abs_builddir") || skip

	lvmconf "activation/monitoring = 1"

	dmeventd -f "$@" &
	echo $! > LOCAL_DMEVENTD

	# FIXME wait for pipe in /var/run instead
	sleep .3
}

prepare_lvmetad() {
	# skip if we don't have our own lvmetad...
	(which lvmetad 2>/dev/null | grep "$abs_builddir") || skip

	lvmconf "global/use_lvmetad = 1"
	lvmconf "devices/md_component_detection = 0"

	local run_valgrind=
	test -z "$LVM_VALGRIND_LVMETAD" || run_valgrind="run_valgrind"

	echo "preparing lvmetad..."
	$run_valgrind lvmetad -f "$@" -s "$TESTDIR/lvmetad.socket" -l wire,debug &
	echo $! > LOCAL_LVMETAD
	while ! test -e "$TESTDIR/lvmetad.socket"; do echo -n .; sleep .1; done # wait for the socket
	echo ok
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
		dmsetup resume "${dm#Suspended:}" || true
	done

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
		test ${LVM_TEST_PARALLEL:-0} -eq 1 || modprobe -r scsi_debug
	else
		test ! -f LOOP || losetup -d $(cat LOOP) || true
		test ! -f LOOPFILE || rm -f $(cat LOOPFILE)
	fi
	rm -f DEVICES # devs is set in prepare_devs()
	rm -f LOOP

	# Attempt to remove any loop devices that failed to get torn down if earlier tests aborted
	test ${LVM_TEST_PARALLEL:-0} -eq 1 -o -z "$COMMON_PREFIX" || {
		teardown_devs_prefixed "$COMMON_PREFIX" 1
		local stray_loops=( $(losetup -a | grep "$COMMON_PREFIX" | cut -d: -f1) )
		test ${#stray_loops[@]} -eq 0 || {
			echo "Removing stray loop devices containing $COMMON_PREFIX: ${stray_loops[@]}"
			losetup -d "${stray_loops[@]}"
		}
	}
}

teardown() {
	echo -n "## teardown..."
	test ! -s LOCAL_LVMETAD || \
	    (kill -TERM "$(cat LOCAL_LVMETAD)" && sleep 1 &&
	     kill -KILL "$(cat LOCAL_LVMETAD)" 2> /dev/null) || true

	dm_table | not egrep -q "$vg|$vg1|$vg2|$vg3|$vg4" || {
		# Avoid activation of dmeventd if there is no pid
		cfg=$(test -s LOCAL_DMEVENTD || echo "--config activation{monitoring=0}")
		vgremove -ff $cfg  \
			$vg $vg1 $vg2 $vg3 $vg4 &>/dev/null || rm -f debug.log
	}

	test -s LOCAL_CLVMD && {
		kill -INT "$(cat LOCAL_CLVMD)"
		test -z "$LVM_VALGRIND_CLVMD" || sleep 1
		sleep .1
		kill -9 "$(cat LOCAL_CLVMD)" &>/dev/null || true
	}

	echo -n .

	pgrep dmeventd || true
	test ! -s LOCAL_DMEVENTD || kill -9 "$(cat LOCAL_DMEVENTD)" || true

	echo -n .

	test -d "$DM_DEV_DIR/mapper" && teardown_devs

	echo -n .

	test -n "$TESTDIR" && {
		cd "$TESTOLDPWD"
		rm -rf "$TESTDIR" || echo BLA
	}

	echo "ok"

	test ${LVM_TEST_PARALLEL:-0} -eq 1 -o -n "$RUNNING_DMEVENTD" || not pgrep dmeventd #&>/dev/null
}

make_ioerror() {
	echo 0 10000000 error | dmsetup create -u ${PREFIX}-ioerror ioerror
	ln -s "$DM_DEV_DIR/mapper/ioerror" "$DM_DEV_DIR/ioerror"
}

prepare_loop() {
	local size=${1=32}
	local i
	local slash

	test -f LOOP && LOOP=$(cat LOOP)
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
	dd if=/dev/zero of="$LOOPFILE" bs=$((1024*1024)) count=0 seek=$(($size-1)) 2> /dev/null
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
	echo "$LOOP" > LOOP
	echo "ok ($LOOP)"
}

# A drop-in replacement for prepare_loop() that uses scsi_debug to create
# a ramdisk-based SCSI device upon which all LVM devices will be created
# - scripts must take care not to use a DEV_SIZE that will enduce OOM-killer
prepare_scsi_debug_dev() {
	local DEV_SIZE=$1
	local SCSI_DEBUG_PARAMS=${@:2}

	test -f "SCSI_DEBUG_DEV" && return 0
	test -z "$LOOP"
	test -n "$DM_DEV_DIR"

	# Skip test if awk isn't available (required for get_sd_devs_)
	which awk || skip

	# Skip test if scsi_debug module is unavailable or is already in use
	modprobe --dry-run scsi_debug || skip
	lsmod | grep -q scsi_debug && skip

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
	echo "$SCSI_DEBUG_DEV" > LOOP
	# Setting $LOOP provides means for prepare_devs() override
	test "$LVM_TEST_DEVDIR" != "/dev" && ln -snf "$DEBUG_DEV" "$SCSI_DEBUG_DEV"
}

cleanup_scsi_debug_dev() {
	teardown_devs
	rm -f SCSI_DEBUG_DEV LOOP
}

prepare_devs() {
	local n=${1:-3}
	local devsize=${2:-34}
	local pvname=${3:-pv}
	local loopsz

	prepare_loop $(($n*$devsize))
	echo -n "## preparing $n devices..."

	if ! loopsz=$(blockdev --getsz "$LOOP" 2>/dev/null); then
		loopsz=$(blockdev --getsize "$LOOP" 2>/dev/null)
	fi

	local size=$(($loopsz/$n))
	devs=

	init_udev_transaction
	for i in $(seq 1 $n); do
		local name="${PREFIX}$pvname$i"
		local dev="$DM_DEV_DIR/mapper/$name"
		devs="$devs $dev"
		echo 0 $size linear "$LOOP" $((($i-1)*$size)) > "$name.table"
		dmsetup create -u "TEST-$name" "$name" "$name.table"
	done
	finish_udev_transaction

	#for i in `seq 1 $n`; do
	#	local name="${PREFIX}$pvname$i"
	#	dmsetup info -c $name
	#done
	#for i in `seq 1 $n`; do
	#	local name="${PREFIX}$pvname$i"
	#	dmsetup table $name
	#done

	echo $devs > DEVICES
	echo "ok"
}

disable_dev() {
	local dev

	init_udev_transaction
	for dev in "$@"; do
		maj=$(($(stat --printf=0x%t "$dev")))
		min=$(($(stat --printf=0x%T "$dev")))
		echo "Disabling device $dev ($maj:$min)"
		dmsetup remove -f "$dev" || true
		notify_lvmetad --major "$maj" --minor "$min"
	done
	finish_udev_transaction
}

enable_dev() {
	local dev

	init_udev_transaction
	for dev in "$@"; do
		local name=$(echo "$dev" | sed -e 's,.*/,,')
		dmsetup create -u "TEST-$name" "$name" "$name.table" || \
			dmsetup load "$name" "$name.table"
		# using device name (since device path does not exists yes with udev)
		dmsetup resume "$name"
		notify_lvmetad "$dev"
	done
	finish_udev_transaction
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
	pvcreate -ff $devs
}

prepare_vg() {
	teardown_devs

	prepare_pvs "$@"
	vgcreate -c n $vg $devs
}

lvmconf() {
	LVM_TEST_LOCKING=${LVM_TEST_LOCKING:-1}
	if test "$DM_DEV_DIR" = "/dev"; then
	    LVM_VERIFY_UDEV=${LVM_VERIFY_UDEV:-0}
	else
	    LVM_VERIFY_UDEV=${LVM_VERIFY_UDEV:-1}
	fi
	test -f CONFIG_VALUES || {
            cat > CONFIG_VALUES <<-EOF
devices/dir = "$DM_DEV_DIR"
devices/scan = "$DM_DEV_DIR"
devices/filter = [ "a|$DM_DEV_DIR/mirror|", "a|$DM_DEV_DIR/mapper/.*pv[0-9_]*$|", "r|.*|" ]
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
EOF
	}

	local v
	for v in "$@"; do
	    echo "$v" >> CONFIG_VALUES
	done

	rm -f CONFIG
	local s
	for s in $(cat CONFIG_VALUES | cut -f1 -d/ | sort | uniq); do
		echo "$s {" >> CONFIG
		local k
		for k in $(grep ^"$s"/ CONFIG_VALUES | cut -f1 -d= | sed -e 's, *$,,' | sort | uniq); do
			grep "^$k" CONFIG_VALUES | tail -n 1 | sed -e "s,^$s/,	  ," >> CONFIG
		done
		echo "}" >> CONFIG
		echo >> CONFIG
	done
	mv -f CONFIG etc/lvm.conf
}

apitest() {
	local t=$1
	shift
	test -x "$abs_top_builddir/test/api/$t.t" || skip
	"$abs_top_builddir/test/api/$t.t" "$@" && rm -f debug.log
}

api() {
	test -x "$abs_top_builddir/test/api/wrapper" || skip
	"$abs_top_builddir/test/api/wrapper" "$@" && rm -f debug.log
}

udev_wait() {
	pgrep udev >/dev/null || return 0
	which udevadm >/dev/null || return 0
	if test -n "$1" ; then
		udevadm settle --exit-if-exists="$1" || true
	else
		udevadm settle --timeout=15 || true
	fi
}

# wait_for_sync <VG/LV>
wait_for_sync() {
	local i
	for i in {1..500} ; do
		check in_sync $1 $2 && return
		sleep .2
	done

	echo "Sync is taking too long - assume stuck"
	return 1
}

#
# Check wheter kernel [dm module] target exist
# at least in expected version
#
# [dm-]target-name major minor revision
#
# i.e.   dm_target_at_least  dm-thin-pool  1 0
target_at_least()
{
	case "$1" in
	  dm-*) modprobe "$1" || true ;;
	esac

	local version=$(dmsetup targets 2>/dev/null | grep "${1##dm-} " 2>/dev/null)
	version=${version##* v}
	shift

	local major=$(echo "$version" | cut -d. -f1)
	test -z "$1" && return 0
	test -n "$major" || return 1
	test "$major" -gt "$1" && return 0
	test "$major" -eq "$1" || return 1

	test -z "$2" && return 0
	local minor=$(echo "$version" | cut -d. -f2)
	test -n "$minor" || return 1
	test "$minor" -gt "$2" && return 0
	test "$minor" -eq "$2" || return 1

	test -z "$3" && return 0
	local revision=$(echo "$version" | cut -d. -f3)
	test "$revision" -ge "$3" 2>/dev/null || return 1
}

have_thin()
{
	target_at_least dm-thin-pool "$@" || exit 1
	test "$THIN" = shared || test "$THIN" = internal || exit 1

	# disable thin_check if not present in system
	which thin_check || lvmconf 'global/thin_check_executable = ""'
}

# check if lvm shell is build-in  (needs readline)
have_readline()
{
	echo version | lvm &>/dev/null
}

test -f DEVICES && devs=$(cat DEVICES)

#unset LVM_VALGRIND
"$@"
