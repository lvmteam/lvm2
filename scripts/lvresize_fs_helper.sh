#!/bin/bash
#
# Copyright (C) 2022-2025 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

set -euE -o pipefail

PATH="/sbin:/usr/sbin:/bin:/usr/bin:$PATH"
GETOPT="getopt"
SCRIPTNAME=$(basename "$0")
DM_DEV_DIR="${DM_DEV_DIR:-/dev}"

usage() {
	cat <<-EOF
	  ${SCRIPTNAME}: helper script called by lvresize to resize file systems.

	  ${SCRIPTNAME} --fsextend --fstype name --lvpath path
	      [ --mountdir path ]
	      [ --mount ]
	      [ --unmount ]
	      [ --remount ]
	      [ --fsck ]
	      [ --cryptresize ]
	      [ --cryptpath path ]
	      [ --newsizebytes num ]

	  ${SCRIPTNAME} --fsreduce --fstype name --lvpath path
	      [ --newsizebytes num ]
	      [ --mountdir path ]
	      [ --mount ]
	      [ --unmount ]
	      [ --remount ]
	      [ --fsck ]
	      [ --cryptresize ]
	      [ --cryptpath path ]

	  ${SCRIPTNAME} --cryptresize --cryptpath path --newsizebytes num

	  Options:
	      --fsextend
		  Extend the file system.
	      --fsreduce
		  Reduce the file system.
	      --fstype name
		  The type of file system (ext*, xfs, btrfs.)
	      --lvpath path
		  The path to the LV being resized.
	      --mountdir path
		  The file system is currently mounted here.
	      --mount
		  Mount the file system on a temporary directory before resizing.
	      --unmount
		  Unmount the file system before resizing.
	      --remount
		  Remount the file system after resizing if unmounted.
	      --fsck
		  Run fsck on the file system before resizing (only with ext*).
	      --newsizebytes num
		  The new size of the file system.
	      --cryptresize
		  Resize the crypt device between the LV and file system.
	      --cryptpath path
		  The path to the crypt device.

	EOF

	exit 0
}

errorexit() {
	echo "$1" >&2
	exit 1
}

logerror() {
	echo "$1" >&2
	logger "${SCRIPTNAME}: $1"
}

logmsg() {
	echo "$1"
	logger "${SCRIPTNAME}: $1"
}

# Handle e2fsck return codes according to fsck(8) exit code specification
# 0 = no errors, 1 = errors corrected, 2 = reboot required
# 4 = errors left uncorrected, 8 = operational error
# 16 = usage error, 32 = canceled, 128 = shared library error
accept_e2fsck() {
	local ret=0
	"$@" || ret=$?

	case "$ret" in
	  0)
		# No errors
		logmsg "e2fsck done"
		return 0
		;;
	  1)
		# Filesystem was corrected
		logmsg "e2fsck done (filesystem errors were corrected)"
		return 0
		;;
	  2)
		# System should be rebooted
		logerror "WARNING: Filesystem was corrected but system should be rebooted"
		logmsg "e2fsck done (reboot recommended)"
		return 0
		;;
	  4)
		logerror "e2fsck failed: filesystem errors left uncorrected on \"$DEVPATH\""
		exit 1
		;;
	  8)
		logerror "e2fsck failed: operational error on \"$DEVPATH\""
		exit 1
		;;
	  16)
		logerror "e2fsck failed: usage or syntax error"
		exit 1
		;;
	  32)
		logerror "e2fsck canceled by user"
		exit 1
		;;
	  *)
		logerror "e2fsck failed with return code $ret on \"$DEVPATH\""
		exit 1
		;;
	esac
}

btrfs_path_major_minor() {
	local STAT

	STAT=$(stat --format "echo \$((0x%t)):\$((0x%T))" "$(readlink -e "$1")") || \
		errorexit "Cannot get major:minor for \"$1\"."
	eval "$STAT"
}

btrfs_devid() {
	local devpath=$1
	local devid devinfo major_minor path_major_minor
	local IFS=$'\n'

	major_minor=$(btrfs_path_major_minor "$devpath")

	# It could be a multi-devices btrfs, filter the output.
	# Device in `btrfs filesystem show $devpath` could be /dev/mapper/* so call `readlink -e`
	for devinfo in $(LC_ALL=C btrfs filesystem show "$devpath"); do
		case "$devinfo" in
		*devid*)
			path_major_minor=$(btrfs_path_major_minor "${devinfo#* path }")
			# compare Major:Minor
			[ "$major_minor" = "$path_major_minor" ] || continue
			devid=${devinfo##*devid}
			devid=${devid%%size*}

			# trim all prefix and postfix spaces from devid
			devid=${devid#"${devid%%[![:space:]]*}"}
			echo "${devid%"${devid##*[![:space:]]}"}"
			return 0
			;;
		esac
	done

	# fail, devid not found
	return 1
}

# Set to 1 while the fs is temporarily mounted on $TMPDIR
TMP_MOUNT_DONE=0
# Set to 1 if the fs resize command fails
RESIZEFS_FAILED=0

# Function to detect XFS mount options
detect_xfs_mount_options() {
	local device=$1
	local qflags_output qflags_hex
	MOUNT_OPTIONS=""

	# Get quota flags using xfs_db.
	if ! qflags_output=$(xfs_db -r "$device" -c 'sb 0' -c 'p qflags'); then
		logerror "xfs_db failed to read quota flags from \"$device\""
		return 1
	fi

	# Extract the hex value from output that is in format "qflags = 0x<hex_number>".
	qflags_hex="${qflags_output#qflags = }"

	# No flags set, no extra mount options needed.
	if [[ "$qflags_hex" == "0" ]]; then
		return 0
	fi

	if [[ ! "$qflags_hex" =~ ^0x[0-9a-fA-F]+$ ]]; then
		logerror "xfs_db unexpected output for \"$device\": got \"$qflags_hex\""
		return 1
	fi

	# Check XFS quota flags and set MOUNT_OPTIONS appropriately
	# The quota flags as defined in Linux kernel source: fs/xfs/libxfs/xfs_log_format.h:
	#   XFS_UQUOTA_ACCT = 0x0001
	#   XFS_UQUOTA_ENFD = 0x0002
	#   XFS_GQUOTA_ACCT = 0x0040
	#   XFS_GQUOTA_ENFD = 0x0080
	#   XFS_PQUOTA_ACCT = 0x0008
	#   XFS_PQUOTA_ENFD = 0x0200

	if [ $(($qflags_hex & 0x0001)) -ne 0 ]; then
		if [ $(($qflags_hex & 0x0002)) -ne 0 ]; then
			MOUNT_OPTIONS="${MOUNT_OPTIONS}uquota,"
		else
			MOUNT_OPTIONS="${MOUNT_OPTIONS}uqnoenforce,"
		fi
	fi

	if [ $(($qflags_hex & 0x0040)) -ne 0 ]; then
		if [ $(($qflags_hex & 0x0080)) -ne 0 ]; then
			MOUNT_OPTIONS="${MOUNT_OPTIONS}gquota,"
		else
			MOUNT_OPTIONS="${MOUNT_OPTIONS}gqnoenforce,"
		fi
	fi

	if [ $(($qflags_hex & 0x0008)) -ne 0 ]; then
		if [ $(($qflags_hex & 0x0200)) -ne 0 ]; then
			MOUNT_OPTIONS="${MOUNT_OPTIONS}pquota,"
		else
			MOUNT_OPTIONS="${MOUNT_OPTIONS}pqnoenforce,"
		fi
	fi

	# Trim trailing comma
	MOUNT_OPTIONS="${MOUNT_OPTIONS%,}"

	if [[ -n "$MOUNT_OPTIONS" ]]; then
		logmsg "mount options for xfs: ${MOUNT_OPTIONS}"
	fi
}

fsextend() {
	if [ "$DO_UNMOUNT" -eq 1 ]; then
		logmsg "unmount ${MOUNTDIR}"
		if umount "$MOUNTDIR"; then
			logmsg "unmount done"
		else
			logerror "unmount failed for \"$MOUNTDIR\""
			exit 1
		fi
	fi

	if [ "$DO_FSCK" -eq 1 ]; then
		if [[ "$FSTYPE" == "ext"* ]]; then
			logmsg "e2fsck ${DEVPATH}"
			accept_e2fsck e2fsck -f -p "$DEVPATH"
		elif [[ "$FSTYPE" == "btrfs" ]]; then
			logmsg "btrfs check ${DEVPATH}"
			if btrfs check "$DEVPATH"; then
				logmsg "btrfs check done"
			else
				logerror "btrfs check failed on \"$DEVPATH\""
				exit 1
			fi
		fi
	fi

	if [ "$DO_CRYPTRESIZE" -eq 1 ]; then
		logmsg "cryptsetup resize ${DEVPATH}"
		if cryptsetup resize "$DEVPATH"; then
			logmsg "cryptsetup done"
		else
			logerror "cryptsetup resize failed on \"$DEVPATH\""
			exit 1
		fi
	fi

	if [ "$DO_MOUNT" -eq 1 ]; then
		if [[ "$FSTYPE" == "xfs" ]]; then
			detect_xfs_mount_options "$DEVPATH" || logmsg "not using XFS mount options"
		fi

		logmsg "mount ${DEVPATH} ${TMPDIR}"
		if mount -t "$FSTYPE" ${MOUNT_OPTIONS:+-o "$MOUNT_OPTIONS"} "$DEVPATH" "$TMPDIR"; then
			logmsg "mount done"
			TMP_MOUNT_DONE=1
		else
			logerror "mount failed for \"$DEVPATH\" on \"$TMPDIR\""
			exit 1
		fi
	fi

	if [[ "$FSTYPE" == "ext"* ]]; then
		logmsg "resize2fs ${DEVPATH}"
		if resize2fs "$DEVPATH"; then
			logmsg "resize2fs done"
		else
			logerror "resize2fs failed on \"$DEVPATH\""
			RESIZEFS_FAILED=1
		fi
	elif [[ "$FSTYPE" == "xfs" ]]; then
		logmsg "xfs_growfs ${DEVPATH}"
		if xfs_growfs "$DEVPATH"; then
			logmsg "xfs_growfs done"
		else
			logerror "xfs_growfs failed on \"$DEVPATH\""
			RESIZEFS_FAILED=1
		fi
	elif [[ "$FSTYPE" == "btrfs" ]]; then
		NEWSIZEBYTES=${NEWSIZEBYTES:-max}
		BTRFS_DEVID="$(btrfs_devid "$DEVPATH")"
		REAL_MOUNTPOINT="$MOUNTDIR"

		if [ $TMP_MOUNT_DONE -eq 1 ]; then
			REAL_MOUNTPOINT="$TMPDIR"
		fi

		logmsg "btrfs filesystem resize ${BTRFS_DEVID}:${NEWSIZEBYTES} ${REAL_MOUNTPOINT}"
		if btrfs filesystem resize "$BTRFS_DEVID":"$NEWSIZEBYTES" "$REAL_MOUNTPOINT"; then
			logmsg "btrfs filesystem resize done"
		else
			logerror "btrfs filesystem resize failed: devid $BTRFS_DEVID to $NEWSIZEBYTES on \"$REAL_MOUNTPOINT\""
			RESIZEFS_FAILED=1
		fi
	fi

	# If the fs was temporarily mounted, now unmount it.
	if [ $TMP_MOUNT_DONE -eq 1 ]; then
		logmsg "cleanup unmount ${TMPDIR}"
		if umount "$TMPDIR"; then
			logmsg "cleanup unmount done"
			TMP_MOUNT_DONE=0
			rmdir "$TMPDIR"
		else
			logerror "cleanup unmount failed for \"$TMPDIR\""
			exit 1
		fi
	fi

	# If the fs was temporarily unmounted, now remount it.
	# Not considered a command failure if this fails.
	if [[ $DO_UNMOUNT -eq 1 && $REMOUNT -eq 1 ]]; then
		if [[ "$FSTYPE" == "xfs" ]]; then
			detect_xfs_mount_options "$DEVPATH" || logmsg "not using XFS mount options"
		fi

		logmsg "remount ${DEVPATH} ${MOUNTDIR}"
		if mount -t "$FSTYPE" ${MOUNT_OPTIONS:+-o "$MOUNT_OPTIONS"} "$DEVPATH" "$MOUNTDIR"; then
			logmsg "remount done"
		else
			logmsg "remount failed"
		fi
	fi

	if [ $RESIZEFS_FAILED -eq 1 ]; then
		logerror "File system extend failed."
		exit 1
	fi

	exit 0
}

fsreduce() {
	if [ "$DO_UNMOUNT" -eq 1 ]; then
		logmsg "unmount ${MOUNTDIR}"
		if umount "$MOUNTDIR"; then
			logmsg "unmount done"
		else
			logerror "unmount failed for \"$MOUNTDIR\""
			exit 1
		fi
	fi

	if [ "$DO_FSCK" -eq 1 ]; then
		if [[ "$FSTYPE" == "ext"* ]]; then
			logmsg "e2fsck ${DEVPATH}"
			accept_e2fsck e2fsck -f -p "$DEVPATH"
		elif [[ "$FSTYPE" == "btrfs" ]]; then
			logmsg "btrfs check ${DEVPATH}"
			if btrfs check "$DEVPATH"; then
				logmsg "btrfs check done"
			else
				logerror "btrfs check failed on \"$DEVPATH\""
				exit 1
			fi
		fi
	fi

	if [ "$DO_MOUNT" -eq 1 ]; then
		logmsg "mount ${DEVPATH} ${TMPDIR}"
		if mount -t "$FSTYPE" "$DEVPATH" "$TMPDIR"; then
			logmsg "mount done"
			TMP_MOUNT_DONE=1
		else
			logerror "mount failed for \"$DEVPATH\" on \"$TMPDIR\""
			exit 1
		fi
	fi

	if [[ "$FSTYPE" == "ext"* ]]; then
		NEWSIZEKB=$(( NEWSIZEBYTES / 1024 ))
		logmsg "resize2fs ${DEVPATH} ${NEWSIZEKB}k"
		if resize2fs "$DEVPATH" "$NEWSIZEKB"k; then
			logmsg "resize2fs done"
		else
			logerror "resize2fs failed on \"$DEVPATH\" to ${NEWSIZEKB}k"
			# will exit after cleanup unmount
			RESIZEFS_FAILED=1
		fi
	elif [[ "$FSTYPE" == "btrfs" ]]; then
		BTRFS_DEVID="$(btrfs_devid "$DEVPATH")"
		REAL_MOUNTPOINT="$MOUNTDIR"

		if [ $TMP_MOUNT_DONE -eq 1 ]; then
			REAL_MOUNTPOINT="$TMPDIR"
		fi

		logmsg "btrfs filesystem resize ${BTRFS_DEVID}:${NEWSIZEBYTES} ${REAL_MOUNTPOINT}"
		if btrfs filesystem resize "$BTRFS_DEVID":"$NEWSIZEBYTES" "$REAL_MOUNTPOINT"; then
			logmsg "btrfs filesystem resize done"
		else
			logerror "btrfs filesystem resize failed: devid $BTRFS_DEVID to $NEWSIZEBYTES on \"$REAL_MOUNTPOINT\""
			RESIZEFS_FAILED=1
		fi
	fi

	# If the fs was temporarily mounted, now unmount it.
	if [ $TMP_MOUNT_DONE -eq 1 ]; then
		logmsg "cleanup unmount ${TMPDIR}"
		if umount "$TMPDIR"; then
			logmsg "cleanup unmount done"
			TMP_MOUNT_DONE=0
			rmdir "$TMPDIR"
		else
			logerror "cleanup unmount failed for \"$TMPDIR\""
			exit 1
		fi
	fi

	if [ $RESIZEFS_FAILED -eq 1 ]; then
		logerror "File system reduce failed."
		exit 1
	fi

	if [ "$DO_CRYPTRESIZE" -eq 1 ]; then
		NEWSIZESECTORS=$(( NEWSIZEBYTES / 512 ))
		logmsg "cryptsetup resize ${NEWSIZESECTORS} sectors ${DEVPATH}"
		if cryptsetup resize --size "$NEWSIZESECTORS" "$DEVPATH"; then
			logmsg "cryptsetup done"
		else
			logmsg "cryptsetup failed"
			exit 1
		fi
	fi

	# If the fs was temporarily unmounted, now remount it.
	# Not considered a command failure if this fails.
	if [[ $DO_UNMOUNT -eq 1 && $REMOUNT -eq 1 ]]; then
		logmsg "remount ${DEVPATH} ${MOUNTDIR}"
		if mount -t "$FSTYPE" "$DEVPATH" "$MOUNTDIR"; then
			logmsg "remount done"
		else
			logmsg "remount failed"
		fi
	fi

	exit 0
}

cryptresize() {
	NEWSIZESECTORS=$(( NEWSIZEBYTES / 512 ))
	logmsg "cryptsetup resize ${NEWSIZESECTORS} sectors ${DEVPATH}"
	if cryptsetup resize --size "$NEWSIZESECTORS" "$DEVPATH"; then
		logmsg "cryptsetup done"
	else
		logerror "cryptsetup resize failed on \"$DEVPATH\" to $NEWSIZESECTORS sectors"
		exit 1
	fi

	exit 0
}

#
# BEGIN SCRIPT
#

# These are the only commands that this script will run.
# Each is enabled (1) by the corresponding command options:
# --fsextend, --fsreduce, --cryptresize, --mount, --unmount, --fsck
DO_FSEXTEND=0
DO_FSREDUCE=0
DO_CRYPTRESIZE=0
DO_MOUNT=0
DO_UNMOUNT=0
DO_FSCK=0

# --remount: attempt to remount the fs if it was originally
# mounted and the script unmounted it.
REMOUNT=0

# Initialize MOUNT_OPTIONS to ensure clean state
MOUNT_OPTIONS=""
MOUNTDIR=""

if [ "$UID" != 0 ] && [ "$EUID" != 0 ]; then
	errorexit "${SCRIPTNAME} must be run as root."
fi

OPTIONS=$("$GETOPT" -o h -l help,fsextend,fsreduce,cryptresize,mount,unmount,remount,fsck,fstype:,lvpath:,newsizebytes:,mountdir:,cryptpath: -n "${SCRIPTNAME}" -- "$@")
eval set -- "$OPTIONS"

while true
do
	case $1 in
	--fsextend)
		DO_FSEXTEND=1
		shift
		;;
	--fsreduce)
		DO_FSREDUCE=1
		shift
		;;
	--cryptresize)
		DO_CRYPTRESIZE=1
		shift
		;;
	--mount)
		DO_MOUNT=1
		shift
		;;
	--unmount)
		DO_UNMOUNT=1
		shift
		;;
	--fsck)
		DO_FSCK=1
		shift
		;;
	--remount)
		REMOUNT=1
		shift
		;;
	--fstype)
		FSTYPE=$2;
		shift; shift
		;;
	--lvpath)
		LVPATH=$2;
		shift; shift
		;;
	--newsizebytes)
		NEWSIZEBYTES=$2;
		shift; shift
		;;
	--mountdir)
		MOUNTDIR=$2;
		shift; shift
		;;
	--cryptpath)
		CRYPTPATH=$2;
		shift; shift
		;;
	-h|--help)
		usage
		shift
		exit 0
		;;
	--)
		shift
		break
		;;
	*)
		errorexit "Unknown option \"$1\"."
		;;
    esac
done

#
# Input arg checking
#

# There are three top level commands: --fsextend, --fsreduce, --cryptresize.
if [[ "$DO_FSEXTEND" -eq 0 && "$DO_FSREDUCE" -eq 0 && "$DO_CRYPTRESIZE" -eq 0 ]]; then
	errorexit "Missing --fsextend|--fsreduce|--cryptresize."
fi

if [[ "$DO_FSEXTEND" -eq 1 || "$DO_FSREDUCE" -eq 1 ]]; then
	case "$FSTYPE" in
	  ext[234]) ;;
	  "xfs")    ;;
	  "btrfs")  ;;
	  *) errorexit "Cannot resize --fstype \"$FSTYPE\"."
	esac

	if [ -z "$LVPATH" ]; then
		errorexit "Missing required --lvpath."
	fi
fi

if [[ "$DO_CRYPTRESIZE" -eq 1 && -z "$CRYPTPATH" ]]; then
	errorexit "Missing required --cryptpath for --cryptresize."
fi

if [ "$DO_CRYPTRESIZE" -eq 1 ]; then
	DEVPATH=$CRYPTPATH
else
	DEVPATH=$LVPATH
fi

if [ -z "$DEVPATH" ]; then
	errorexit "Missing path to device."
fi

if [ ! -e "$DEVPATH" ]; then
	errorexit "Device does not exist \"$DEVPATH\"."
fi

if [[ "$DO_UNMOUNT" -eq 1 && -z "$MOUNTDIR" ]]; then
	errorexit "Missing required --mountdir for --unmount."
fi

if [[ "$DO_FSREDUCE" -eq 1 && "$FSTYPE" == "xfs" ]]; then
	errorexit "Cannot reduce xfs."
fi

if [[ "$DO_FSCK" -eq 1 && "$FSTYPE" == "xfs" ]]; then
	errorexit "Cannot use --fsck with xfs."
fi

if [ "$DO_MOUNT" -eq 1 ]; then
	TMPDIR=$(mktemp --suffix _lvresize_$$ -d -p /tmp)
	if [ ! -e "$TMPDIR" ]; then
		errorexit "Failed to create temp dir."
	fi
	# In case the script terminates without doing cleanup
	function finish {
		if [ "$TMP_MOUNT_DONE" -eq 1 ]; then
			logmsg "exit unmount ${TMPDIR}"
			umount "$TMPDIR"
			rmdir "$TMPDIR"
		fi
	}
	trap finish EXIT
fi

#
# Main program function:
# - the two main functions are fsextend and fsreduce.
# - one special case function is cryptresize.
#

if [ "$DO_FSEXTEND" -eq 1 ]; then
	fsextend
elif [ "$DO_FSREDUCE" -eq 1 ]; then
	fsreduce
elif [ "$DO_CRYPTRESIZE" -eq 1 ]; then
	cryptresize
fi
