#!/bin/bash
# Copyright (C) 2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/utils

prepare_clvmd() {
	if test -z "$LVM_TEST_LOCKING" || test "$LVM_TEST_LOCKING" -ne 3 ; then
		return 0 # not needed
	fi

	if pgrep clvmd ; then
		echo "Cannot use fake cluster locking with real clvmd ($(pgrep clvmd)) running."
                skip
	fi

	# skip if we don't have our own clvmd...
	(which clvmd | grep $abs_builddir) || skip

	# skip if we singlenode is not compiled in
	(clvmd --help 2>&1 | grep "Available cluster managers" | grep singlenode) || skip

	lvmconf "activation/monitoring = 1"

	clvmd -Isinglenode -d 1 &
	LOCAL_CLVMD="$!"

	# check that it is really running now
	sleep .1
	ps $LOCAL_CLVMD || skip
        echo "$LOCAL_CLVMD" > LOCAL_CLVMD
}

prepare_dmeventd() {
	if pgrep dmeventd ; then
		echo "Cannot test dmeventd with real dmeventd ($(pgrep dmeventd)) running."
                touch SKIP_THIS_TEST
		exit 1
	fi

	# skip if we don't have our own dmeventd...
	(which dmeventd | grep $abs_builddir) || {
            touch SKIP_THIS_TEST
            exit 1
        }

        lvmconf "activation/monitoring = 1"

	dmeventd -f "$@" &
	echo "$!" > LOCAL_DMEVENTD

	# FIXME wait for pipe in /var/run instead
	sleep 1
}

teardown_devs() {
	# Delete any remaining dm/udev semaphores
	teardown_udev_cookies

	test -n "$PREFIX" && {
		rm -rf $TESTDIR/dev/$PREFIX*

		init_udev_transaction
		while dmsetup table | grep -q ^$PREFIX; do
			for s in `dmsetup info -c -o name --noheading | grep ^$PREFIX`; do
				umount -fl $DM_DEV_DIR/mapper/$s >& /dev/null || true
				dmsetup remove -f $s >& /dev/null || true
			done
		done
		finish_udev_transaction

	}

	udev_wait
	# NOTE: SCSI_DEBUG_DEV test must come before the LOOP test because
	# prepare_scsi_debug_dev() also sets LOOP to short-circuit prepare_loop()
	if test -f SCSI_DEBUG_DEV; then
		modprobe -r scsi_debug
	else
		test -f LOOP && losetup -d $(cat LOOP)
		test -f LOOPFILE && rm -f $(cat LOOPFILE)
	fi
	rm -f DEVICES # devs is set in prepare_devs()
	rm -f LOOP

	# Attempt to remove any loop devices that failed to get torn down if earlier tests aborted
	test -n "$COMMON_PREFIX" && {
		# Resume any linears to be sure we do not deadlock
		STRAY_DEVS=$(dmsetup table | sed 's/:.*//' | grep $COMMON_PREFIX | cut -d' '  -f 1)
		for dm in $STRAY_DEVS ; do
			# FIXME: only those really suspended
			echo dmsetup resume $dm
			dmsetup resume $dm || true
		done

		STRAY_MOUNTS=`mount | grep $COMMON_PREFIX | cut -d\  -f1`
		if test -n "$STRAY_MOUNTS"; then
			echo "Removing stray mounted devices containing $COMMON_PREFIX:"
			mount | grep $COMMON_PREFIX
			umount -fl $STRAY_MOUNTS || true
			sleep 2
		fi

		init_udev_transaction
		NUM_REMAINING_DEVS=999
		while NUM_DEVS=`dmsetup table | grep ^$COMMON_PREFIX | wc -l` && \
		    test $NUM_DEVS -lt $NUM_REMAINING_DEVS -a $NUM_DEVS -ne 0; do
			echo "Removing $NUM_DEVS stray mapped devices with names beginning with $COMMON_PREFIX:"
			STRAY_DEVS=$(dmsetup table | sed 's/:.*//' | grep $COMMON_PREFIX | cut -d' '  -f 1)
			dmsetup info -c | grep ^$COMMON_PREFIX
			for dm in $STRAY_DEVS ; do
				echo dmsetup remove $dm
				dmsetup remove -f $dm || true
			done
			NUM_REMAINING_DEVS=$NUM_DEVS
		done
		finish_udev_transaction
		udev_wait

        	STRAY_LOOPS=`losetup -a | grep $COMMON_PREFIX | cut -d: -f1`
        	if test -n "$STRAY_LOOPS"; then
                	echo "Removing stray loop devices containing $COMMON_PREFIX:"
                	losetup -a | grep $COMMON_PREFIX
                	losetup -d $STRAY_LOOPS || true
        	fi
	}
}

teardown() {
    echo -n "## teardown..."

    test -f LOCAL_CLVMD && {
	kill "$(cat LOCAL_CLVMD)"
	sleep .1
	kill -9 "$(cat LOCAL_CLVMD)" || true
    }

    echo -n .

    test -f LOCAL_DMEVENTD && kill -9 "$(cat LOCAL_DMEVENTD)"

    echo -n .

    teardown_devs

    echo -n .

    test -n "$TESTDIR" && {
	cd $OLDPWD
	rm -rf $TESTDIR || echo BLA
    }

    echo "ok"
}

make_ioerror() {
	echo 0 10000000 error | dmsetup create -u TEST-ioerror ioerror
	ln -s $DM_DEV_DIR/mapper/ioerror $DM_DEV_DIR/ioerror
}

prepare_loop() {
	size=$1
	test -n "$size" || size=32

        echo -n "## preparing loop device..."

	# skip if prepare_scsi_debug_dev() was used
	if [ -f "SCSI_DEBUG_DEV" -a -f "LOOP" ]; then
                echo "(skipped)"
		return 0
	fi

	test ! -e LOOP
	test -n "$DM_DEV_DIR"

	for i in 0 1 2 3 4 5 6 7; do
		test -e $DM_DEV_DIR/loop$i || mknod $DM_DEV_DIR/loop$i b 7 $i
	done

        echo -n .

	LOOPFILE="$PWD/test.img"
	dd if=/dev/zero of="$LOOPFILE" bs=$((1024*1024)) count=0 seek=$(($size-1)) 2> /dev/null
	if LOOP=`losetup -s -f "$LOOPFILE" 2>/dev/null`; then
		:
	elif LOOP=`losetup -f` && losetup $LOOP "$LOOPFILE"; then
		# no -s support
		:
	else
		# no -f support 
		# Iterate through $DM_DEV_DIR/loop{,/}{0,1,2,3,4,5,6,7}
		for slash in '' /; do
			for i in 0 1 2 3 4 5 6 7; do
				local dev=$DM_DEV_DIR/loop$slash$i
				! losetup $dev >/dev/null 2>&1 || continue
				# got a free
				losetup "$dev" "$LOOPFILE"
				LOOP=$dev
				break
			done
			if [ -n "$LOOP" ]; then 
				break
			fi
		done
	fi
	test -n "$LOOP" # confirm or fail
        echo "$LOOP" > LOOP
        echo "ok ($LOOP)"
}

# A drop-in replacement for prepare_loop() that uses scsi_debug to create
# a ramdisk-based SCSI device upon which all LVM devices will be created
# - scripts must take care not to use a DEV_SIZE that will enduce OOM-killer
prepare_scsi_debug_dev()
{
    local DEV_SIZE="$1"
    shift
    local SCSI_DEBUG_PARAMS="$@"

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

    local DEBUG_DEV=/dev/$(grep -H scsi_debug /sys/block/*/device/model | cut -f4 -d /)
    [ -b $DEBUG_DEV ] || exit 1 # should not happen

    # Create symlink to scsi_debug device in $DM_DEV_DIR
    SCSI_DEBUG_DEV="$DM_DEV_DIR/$(basename $DEBUG_DEV)"
    echo "$SCSI_DEBUG_DEV" > SCSI_DEBUG_DEV
    echo "$SCSI_DEBUG_DEV" > LOOP
    # Setting $LOOP provides means for prepare_devs() override
    ln -snf $DEBUG_DEV $SCSI_DEBUG_DEV
    return 0
}

cleanup_scsi_debug_dev()
{
    aux teardown_devs
    rm -f SCSI_DEBUG_DEV
    rm -f LOOP
}

prepare_devs() {
	local n="$1"
	test -z "$n" && n=3
	local devsize="$2"
	test -z "$devsize" && devsize=34
	local pvname="$3"
	test -z "$pvname" && pvname="pv"

	prepare_loop $(($n*$devsize))
        echo -n "## preparing $n devices..."

	if ! loopsz=`blockdev --getsz $LOOP 2>/dev/null`; then
  		loopsz=`blockdev --getsize $LOOP 2>/dev/null`
	fi

	local size=$(($loopsz/$n))
        devs=

	init_udev_transaction
	for i in `seq 1 $n`; do
		local name="${PREFIX}$pvname$i"
		local dev="$DM_DEV_DIR/mapper/$name"
		devs="$devs $dev"
		echo 0 $size linear $LOOP $((($i-1)*$size)) > $name.table
		dmsetup create -u TEST-$name $name $name.table
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

	init_udev_transaction
	for dev in "$@"; do
        	dmsetup remove -f $dev || true
	done
	finish_udev_transaction

}

enable_dev() {

	init_udev_transaction
	for dev in "$@"; do
		local name=`echo "$dev" | sed -e 's,.*/,,'`
		dmsetup create -u TEST-$name $name $name.table || dmsetup load $name $name.table
		dmsetup resume $dev
	done
	finish_udev_transaction
}

backup_dev() {
	for dev in "$@"; do
		dd if=$dev of=$dev.backup bs=1024
	done
}

restore_dev() {
	for dev in "$@"; do
		test -e $dev.backup || {
			echo "Internal error: $dev not backed up, can't restore!"
			exit 1
		}
		dd of=$dev if=$dev.backup bs=1024
	done
}

prepare_pvs() {
	prepare_devs "$@"
	pvcreate -ff $devs
}

prepare_vg() {
	vgremove -ff $vg >& /dev/null || true
	teardown_devs

	prepare_pvs "$@"
	vgcreate -c n $vg $devs
	#pvs -v
}

lvmconf() {
    if test -z "$LVM_TEST_LOCKING"; then LVM_TEST_LOCKING=1; fi
    if test "$DM_DEV_DIR" = "/dev"; then
	VERIFY_UDEV=0;
    else
	VERIFY_UDEV=1;
    fi
    test -f CONFIG_VALUES || {
        cat > CONFIG_VALUES <<-EOF
devices/dir = "$DM_DEV_DIR"
devices/scan = "$DM_DEV_DIR"
devices/filter = [ "a/dev\/mirror/", "a/dev\/mapper\/.*pv[0-9_]*$/", "r/.*/" ]
devices/cache_dir = "$TESTDIR/etc"
devices/sysfs_scan = 0
devices/default_data_alignment = 1
log/syslog = 0
log/indent = 1
log/level = 9
log/file = "$TESTDIR/debug.log"
log/overwrite = 1
log/activation = 1
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
activation/verify_udev_operations = $VERIFY_UDEV
activation/polling_interval = 0
activation/snapshot_autoextend_percent = 50
activation/snapshot_autoextend_threshold = 50
activation/monitoring = 0
EOF
    }

    for v in "$@"; do
        echo "$v" >> CONFIG_VALUES
    done

    rm -f CONFIG
    for s in `cat CONFIG_VALUES | cut -f1 -d/ | sort | uniq`; do
        echo "$s {" >> CONFIG
        for k in `grep ^$s/ CONFIG_VALUES | cut -f1 -d= | sed -e 's, *$,,' | sort | uniq`; do
            grep "^$k" CONFIG_VALUES | tail -n 1 | sed -e "s,^$s/,    ," >> CONFIG
        done
        echo "}" >> CONFIG
        echo >> CONFIG
    done
    mv -f CONFIG $TESTDIR/etc/lvm.conf
}

apitest() {
	t=$1
        shift
	test -x $abs_top_builddir/test/api/$t.t || skip
	$abs_top_builddir/test/api/$t.t "$@"
}

api() {
	test -x $abs_top_builddir/test/api/wrapper || skip
	$abs_top_builddir/test/api/wrapper "$@"
}

udev_wait() {
	pgrep udev >/dev/null || return 0
	which udevadm >/dev/null || return 0
	if test -n "$1" ; then
		udevadm settle --exit-if-exists=$1
	else
		udevadm settle --timeout=15
	fi
}

test -f DEVICES && devs=$(cat DEVICES)
test -f LOOP && LOOP=$(cat LOOP)

"$@"
