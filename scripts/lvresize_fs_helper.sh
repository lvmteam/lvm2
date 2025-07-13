#!/bin/bash
#
# Copyright (C) 2022 Red Hat, Inc. All rights reserved.
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

DM_DEV_DIR="${DM_DEV_DIR:-/dev}"

errorexit() {
	echo "$1" >&2
	exit 1
}

logmsg() {
	echo "$1"
	logger "${SCRIPTNAME}: $1"
}

btrfs_devid() {
	local devpath=$1
	local devid devinfo tmp_devpath
	local old_IFS=$IFS

	devpath=$(readlink -e "$devpath")

	IFS=$'\n'
	# It could be a multi-devices btrfs, filter the output.
	# Device in `btrfs filesystem show $devpath` could be /dev/mapper/* so call `readlink -e`
	for devinfo in $(LC_ALL=C btrfs filesystem show "$devpath" | grep -P '\tdevid'); do
		tmp_devpath=$(readlink -e "$(echo "$devinfo" | awk '{print $8}')")
		if [ "$tmp_devpath" = "$devpath" ]; then
			devid="$(echo "$devinfo" | awk '{print $2}')"
			break
		fi
	done
	IFS=$old_IFS

	# if DM_DEV_DIR is not /dev/ e.g /tmp, output of btrfs filesystem show would be like:
	# Label: none  uuid: d17f6974-267f-4140-8d71-83d4afd36a72
	# 		 Total devices 1 FS bytes used 144.00KiB
	# 		 devid    1 size 256.00MiB used 16.75MiB path /dev/mapper/LVMTEST120665vg-LV1
	#
	# But the VOLUME here is /tmp/mapper/LVMTEST120665vg-LV1
	if [ -z "$devid" ];then
		tmp_devpath=${devpath/#${DM_DEV_DIR}//dev}
		devid=$(LC_ALL=C btrfs filesystem show "$devpath" | grep "$tmp_devpath"$)
		devid=${devid##*devid}
		devid=${devid%%size*}
		devid=$(echo "$devid" |sed 's/^[ \t]*//g'|sed 's/[ \t]*$'//g)
	fi

	echo "$devid"
}

# Set to 1 while the fs is temporarily mounted on $TMPDIR
TMP_MOUNT_DONE=0
# Set to 1 if the fs resize command fails
RESIZEFS_FAILED=0

fsextend() {
	if [ "$DO_UNMOUNT" -eq 1 ]; then
		logmsg "unmount ${MOUNTDIR}"
		if umount "$MOUNTDIR"; then
			logmsg "unmount done"
		else
			logmsg "unmount failed"
			exit 1
		fi
	fi

	if [ "$DO_FSCK" -eq 1 ]; then
		if [[ "$FSTYPE" == "ext"* ]]; then
			logmsg "e2fsck ${DEVPATH}"
			if e2fsck -f -p "$DEVPATH"; then
				logmsg "e2fsck done"
			else
				logmsg "e2fsck failed"
				exit 1
			fi
		elif [[ "$FSTYPE" == "btrfs" ]]; then
			logmsg "btrfsck ${DEVPATH}"
			if btrfsck "$DEVPATH"; then
				logmsg "btrfsck done"
			else
				logmsg "btrfsck failed"
				exit 1
			fi
		fi
	fi
	
	if [ "$DO_CRYPTRESIZE" -eq 1 ]; then
		logmsg "cryptsetup resize ${DEVPATH}"
		if cryptsetup resize "$DEVPATH"; then
			logmsg "cryptsetup done"
		else
			logmsg "cryptsetup failed"
			exit 1
		fi
	fi

	if [ "$DO_MOUNT" -eq 1 ]; then
		logmsg "mount ${DEVPATH} ${TMPDIR}"
		if mount -t "$FSTYPE" "$DEVPATH" "$TMPDIR"; then
			logmsg "mount done"
			TMP_MOUNT_DONE=1
		else
			logmsg "mount failed"
			exit 1
		fi
	fi

	if [[ "$FSTYPE" == "ext"* ]]; then
		logmsg "resize2fs ${DEVPATH}"
		if resize2fs "$DEVPATH"; then
			logmsg "resize2fs done"
		else
			logmsg "resize2fs failed"
			RESIZEFS_FAILED=1
		fi
	elif [[ "$FSTYPE" == "xfs" ]]; then
		logmsg "xfs_growfs ${DEVPATH}"
		if xfs_growfs "$DEVPATH"; then
			logmsg "xfs_growfs done"
		else
			logmsg "xfs_growfs failed"
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
			logmsg "btrfs filesystem resize failed"
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
			logmsg "cleanup unmount failed"
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

	if [ $RESIZEFS_FAILED -eq 1 ]; then
		logmsg "File system extend failed."
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
			logmsg "unmount failed"
			exit 1
		fi
	fi

	if [ "$DO_FSCK" -eq 1 ]; then
		if [[ "$FSTYPE" == "ext"* ]]; then
			logmsg "e2fsck ${DEVPATH}"
			if e2fsck -f -p "$DEVPATH"; then
				logmsg "e2fsck done"
			else
				logmsg "e2fsck failed"
				exit 1
			fi
		elif [[ "$FSTYPE" == "btrfs" ]]; then
			logmsg "btrfsck ${DEVPATH}"
			if btrfsck "$DEVPATH"; then
				logmsg "btrfsck done"
			else
				logmsg "btrfsck failed"
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
			logmsg "mount failed"
			exit 1
		fi
	fi

	if [[ "$FSTYPE" == "ext"* ]]; then
		NEWSIZEKB=$(( NEWSIZEBYTES / 1024 ))
		logmsg "resize2fs ${DEVPATH} ${NEWSIZEKB}k"
		if resize2fs "$DEVPATH" "$NEWSIZEKB"k; then
			logmsg "resize2fs done"
		else
			logmsg "resize2fs failed"
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
			logmsg "btrfs filesystem resize failed"
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
			logmsg "cleanup unmount failed"
			exit 1
		fi
	fi

	if [ $RESIZEFS_FAILED -eq 1 ]; then
		logmsg "File system reduce failed."
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
		logmsg "cryptsetup failed"
		exit 1
	fi

	exit 0
}

usage() {
	echo "${SCRIPTNAME}: helper script called by lvresize to resize file systems."
	echo ""
	echo "${SCRIPTNAME} --fsextend --fstype name --lvpath path"
	echo "    [ --mountdir path ]"
	echo "    [ --mount ]"
	echo "    [ --unmount ]"
	echo "    [ --remount ]"
	echo "    [ --fsck ]"
	echo "    [ --cryptresize ]"
	echo "    [ --cryptpath path ]"
	echo "    [ --newsizebytes num ]"
	echo ""
	echo "${SCRIPTNAME} --fsreduce --fstype name --lvpath path"
	echo "    [ --newsizebytes num ]"
	echo "    [ --mountdir path ]"
	echo "    [ --mount ]"
	echo "    [ --unmount ]"
	echo "    [ --remount ]"
	echo "    [ --fsck ]"
	echo "    [ --cryptresize ]"
	echo "    [ --cryptpath path ]"
	echo ""
	echo "${SCRIPTNAME} --cryptresize --cryptpath path --newsizebytes num"
	echo ""
	echo "Options:"
	echo "    --fsextend"
	echo "        Extend the file system."
	echo "    --fsreduce"
	echo "        Reduce the file system."
	echo "    --fstype name"
	echo "        The type of file system (ext*, xfs, btrfs.)"
	echo "    --lvpath path"
	echo "        The path to the LV being resized."
	echo "    --mountdir path"
	echo "        The file system is currently mounted here."
	echo "    --mount"
	echo "        Mount the file system on a temporary directory before resizing."
	echo "    --unmount"
	echo "        Unmount the file system before resizing."
	echo "    --remount"
	echo "        Remount the file system after resizing if unmounted."
	echo "    --fsck"
	echo "        Run fsck on the file system before resizing (only with ext*)."
	echo "    --newsizebytes num"
	echo "        The new size of the file system."
	echo "    --cryptresize"
	echo "        Resize the crypt device between the LV and file system."
	echo "    --cryptpath path"
	echo "        The path to the crypt device."
	echo ""
}

#
# BEGIN SCRIPT
#
PATH="/sbin:/usr/sbin:/bin:/usr/sbin:$PATH"
SCRIPTNAME=$(basename "$0")

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

if [ "$UID" != 0 ] && [ "$EUID" != 0 ]; then
	errorexit "${SCRIPTNAME} must be run as root."
fi

GETOPT="getopt"

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
		errorexit "Unknown option \"$1\."
		exit 1
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
