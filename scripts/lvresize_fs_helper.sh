#!/bin/bash
#
# Copyright (C) 2022-2026 Red Hat, Inc. All rights reserved.
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

PATH="/sbin:/usr/sbin:/bin:/usr/bin"
GETOPT="getopt"
SCRIPTNAME=${0##*/}

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
		  Run fsck on the file system before resizing (ext* and btrfs).
	      --newsizebytes num
		  The new size of the file system.
	      --cryptresize
		  Resize the crypt device between the LV and file system.
	      --cryptpath path
		  The path to the crypt device.

	EOF

	exit 0
}

# errorexit: invalid invocation (stderr only).  die: runtime failure
# (stderr and syslog).  logerror: non-fatal problem or warning.
errorexit() {
	printf '%s\n' "$1" >&2
	exit 1
}

die() {
	logerror "$1"
	exit 1
}

# logger is best-effort: it may be missing (minimal or container
# environments), and under set -e a logger failure would otherwise abort
# before the actual resize command runs.

logmsg() {
	printf '%s\n' "$1"
	logger "${SCRIPTNAME}: $1" >/dev/null 2>&1 || true
}

logerror() {
	logmsg "$1" >&2
}

# Validate --newsizebytes.  $1 is 1 when the value is required (reduce and
# cryptresize) and 0 when it is optional (extend, where btrfs falls back to
# "max").  An optional value that is absent is fine; one that is present must
# be a positive number.
validate_newsizebytes() {
	local required=$1

	if [ -z "${NEWSIZEBYTES-}" ]; then
		[ "$required" -eq 1 ] || return 0
		errorexit "Missing required --newsizebytes."
	fi

	case "$NEWSIZEBYTES" in
	*[!0-9]*) errorexit "--newsizebytes must be a number." ;;
	esac

	# Drop leading zeros so the value is never parsed as octal downstream,
	# matching fsadm's parse_number().
	NEWSIZEBYTES=${NEWSIZEBYTES#"${NEWSIZEBYTES%%[!0]*}"}
	NEWSIZEBYTES=${NEWSIZEBYTES:-0}

	if [ "$NEWSIZEBYTES" -eq 0 ]; then
		errorexit "--newsizebytes must be greater than zero."
	fi
}

# Reject a path a non-root user could modify or replace between this
# check and its use: every component of the canonical path must be
# root-owned and must not be group or world writable.  The sticky bit
# is accepted for directories (e.g. /tmp), where other users cannot
# remove entries owned by root.
validate_path() {
	local NAME=$1
	local NODE=$2
	local MODE

	while :; do
		MODE=$(stat -c '%u %a' "$NODE") ||
			errorexit "$NAME \"$NODE\" is not accessible."
		test "${MODE%% *}" = "0" ||
			errorexit "$NAME \"$NODE\" must be owned by root."
		MODE=${MODE##* }
		test $(( 0$MODE & 022 )) -eq 0 ||
			{ test -d "$NODE" && test $(( 0$MODE & 01000 )) -ne 0; } ||
			errorexit "$NAME \"$NODE\" must not be group or world writable."
		test "$NODE" = "/" && break
		NODE=${NODE%/*}
		test -n "$NODE" || NODE=/
	done
}

# Run on any exit (EXIT trap) or interruption (signal traps).
# Receives the exit code as $1 so the correct status is preserved:
# signals pass 2, the EXIT trap passes the shell's real $?.
# Always ends with exit, deliberately never returns to the caller.
cleanup() {
	local RET=${1:-1}

	trap '' EXIT HUP INT QUIT ABRT TERM

	if [ "$TMP_MOUNT_DONE" -eq 1 ]; then
		logmsg "cleanup unmount ${TEMPDIR}"
		if umount "$TEMPDIR"; then
			TMP_MOUNT_DONE=0
		else
			logerror "cleanup unmount failed for \"$TEMPDIR\""
		fi
	fi

	if test -n "$TEMPDIR"; then
		rmdir "$TEMPDIR" 2>/dev/null || true
		rmdir "${TEMPDIR%/*}" 2>/dev/null || true
	fi

	trap - EXIT HUP INT QUIT ABRT TERM

	test "$RET" -eq 2 && logmsg "Break detected."

	exit "$RET"
}

# Handle e2fsck return codes as bitmask per fsck(8) specification:
#   1 = errors corrected, 2 = system should be rebooted
#   4 = errors left uncorrected, 8 = operational error
#   16 = usage/syntax error, 32 = canceled by user
#   128 = shared library error
# Codes are OR'd together, so ret=3 means corrected + reboot.
# Bits 0-1 (ret 1,2) are non-fatal; bits 2-7 (ret >= 4) are fatal.
accept_e2fsck() {
	local ret=0
	"$@" || ret=$?

	test $((ret & 128)) -ne 0 &&
		die "e2fsck failed (shared library error) on \"$DEVPATH\""
	test $((ret & 32)) -ne 0 &&
		die "e2fsck canceled by user"
	test $((ret & 16)) -ne 0 &&
		die "e2fsck failed: usage or syntax error"
	test $((ret & 8)) -ne 0 &&
		die "e2fsck failed: operational error on \"$DEVPATH\""
	test $((ret & 4)) -ne 0 &&
		die "e2fsck failed: filesystem errors left uncorrected on \"$DEVPATH\""
	test $((ret & ~191)) -ne 0 &&
		die "e2fsck failed with return code $ret on \"$DEVPATH\""

	test $((ret & 2)) -ne 0 &&
		logerror "WARNING: e2fsck recommends reboot for \"$DEVPATH\""
	test $((ret & 1)) -ne 0 &&
		logmsg "e2fsck corrected errors on \"$DEVPATH\""

	logmsg "e2fsck done"
}

btrfs_path_major_minor() {
	local STAT TARGET

	TARGET=$(readlink -e "$1") ||
		die "Cannot resolve \"$1\"."
	test -b "$TARGET" ||
		die "\"$1\" is not a block device."
	STAT=$(stat --format '0x%t:0x%T' "$TARGET") ||
		die "Cannot get major:minor for \"$1\"."
	echo "$(( ${STAT%%:*} )):$(( ${STAT#*:} ))"
}

btrfs_devid() {
	local devpath=$1
	local devid devinfo major_minor path_major_minor show_output

	major_minor=$(btrfs_path_major_minor "$devpath")

	show_output=$(LC_ALL=C btrfs filesystem show "$devpath") ||
		die "btrfs filesystem show failed on \"$devpath\""

	# Multi-device btrfs: walk btrfs filesystem show output line by line.
	# Device lines may be /dev/mapper/*; resolve via btrfs_path_major_minor.
	while IFS= read -r devinfo; do
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
	done <<< "$show_output"

	die "btrfs devid not found for \"$devpath\""
}

# Set to 1 while the fs is temporarily mounted on $TEMPDIR
TMP_MOUNT_DONE=0
# Set to 1 if the fs resize command fails
RESIZEFS_FAILED=0

# Function to detect XFS mount options
detect_xfs_mount_options() {
	local device=$1
	local qflags_output qflags_hex
	local prefix acct_flag enfd_flag
	local -a opts=()
	MOUNT_OPTIONS=""

	# Get quota flags using xfs_db.
	if ! qflags_output=$(xfs_db -r "$device" -c 'sb 0' -c 'p qflags'); then
		logerror "xfs_db failed to read quota flags from \"$device\""
		return 1
	fi

	# Extract the hex value from output that is in format "qflags = 0x<hex_number>".
	qflags_hex="${qflags_output#qflags = }"

	# No flags set, no extra mount options needed.
	[[ "$qflags_hex" == "0" || "$qflags_hex" == "0x0" ]] && return 0

	if [[ ! "$qflags_hex" =~ ^0x[0-9a-fA-F]+$ ]]; then
		logerror "xfs_db unexpected output for \"$device\": got \"$qflags_hex\""
		return 1
	fi

	# Check XFS quota flags and build mount options
	# The quota flags as defined in Linux kernel source: fs/xfs/libxfs/xfs_log_format.h:
	#   XFS_UQUOTA_ACCT = 0x0001, XFS_UQUOTA_ENFD = 0x0002
	#   XFS_GQUOTA_ACCT = 0x0040, XFS_GQUOTA_ENFD = 0x0080
	#   XFS_PQUOTA_ACCT = 0x0008, XFS_PQUOTA_ENFD = 0x0200
	#
	# Format: "prefix acct_flag enfd_flag"
	for quota_type in "u 0x0001 0x0002" "g 0x0040 0x0080" "p 0x0008 0x0200"; do
		read -r prefix acct_flag enfd_flag <<< "$quota_type"
		if [ $((qflags_hex & acct_flag)) -ne 0 ]; then
			if [ $((qflags_hex & enfd_flag)) -ne 0 ]; then
				opts+=("${prefix}quota")
			else
				opts+=("${prefix}qnoenforce")
			fi
		fi
	done

	# Join array elements with commas
	MOUNT_OPTIONS=$(IFS=,; echo "${opts[*]}")

	# An empty option list (no quota accounting) is a valid result: the "||"
	# short-circuit also makes the function return success, so the caller
	# does not report a bogus "not using XFS mount options".
	[[ -z "$MOUNT_OPTIONS" ]] || logmsg "mount options for xfs: ${MOUNT_OPTIONS}"
}

fsextend() {
	if [ "$DO_UNMOUNT" -eq 1 ]; then
		logmsg "unmount ${MOUNTDIR}"
		umount "$MOUNTDIR" ||
			errorexit "unmount failed for \"$MOUNTDIR\""
		logmsg "unmount done"
	fi

	if [ "$DO_FSCK" -eq 1 ]; then
		if [[ "$FSTYPE" == "ext"* ]]; then
			logmsg "e2fsck ${DEVPATH}"
			accept_e2fsck e2fsck -f -p "$DEVPATH"
		elif [[ "$FSTYPE" == "btrfs" ]]; then
			logmsg "btrfs check ${DEVPATH}"
			btrfs check "$DEVPATH" ||
				errorexit "btrfs check failed on \"$DEVPATH\""
			logmsg "btrfs check done"
		fi
	fi

	if [ "$DO_CRYPTRESIZE" -eq 1 ]; then
		logmsg "cryptsetup resize ${DEVPATH}"
		cryptsetup resize "$DEVPATH" ||
			errorexit "cryptsetup resize failed on \"$DEVPATH\""
		logmsg "cryptsetup done"
	fi

	if [ "$DO_MOUNT" -eq 1 ]; then
		if [[ "$FSTYPE" == "xfs" ]]; then
			detect_xfs_mount_options "$DEVPATH" || logmsg "not using XFS mount options"
		fi

		logmsg "mount ${DEVPATH} ${TEMPDIR}"
		mount -t "$FSTYPE" ${MOUNT_OPTIONS:+-o "$MOUNT_OPTIONS"} "$DEVPATH" "$TEMPDIR" ||
			errorexit "mount failed for \"$DEVPATH\" on \"$TEMPDIR\""
		logmsg "mount done"
		TMP_MOUNT_DONE=1
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
			REAL_MOUNTPOINT="$TEMPDIR"
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
		logmsg "cleanup unmount ${TEMPDIR}"
		umount "$TEMPDIR" ||
			errorexit "cleanup unmount failed for \"$TEMPDIR\""
		logmsg "cleanup unmount done"
		TMP_MOUNT_DONE=0
		rmdir "$TEMPDIR" 2>/dev/null || true
		rmdir "${TEMPDIR%/*}" 2>/dev/null || true
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
		errorexit "File system extend failed."
	fi

	exit 0
}

fsreduce() {
	if [ "$DO_UNMOUNT" -eq 1 ]; then
		logmsg "unmount ${MOUNTDIR}"
		umount "$MOUNTDIR" ||
			errorexit "unmount failed for \"$MOUNTDIR\""
		logmsg "unmount done"
	fi

	if [ "$DO_FSCK" -eq 1 ]; then
		if [[ "$FSTYPE" == "ext"* ]]; then
			logmsg "e2fsck ${DEVPATH}"
			accept_e2fsck e2fsck -f -p "$DEVPATH"
		elif [[ "$FSTYPE" == "btrfs" ]]; then
			logmsg "btrfs check ${DEVPATH}"
			btrfs check "$DEVPATH" ||
				errorexit "btrfs check failed on \"$DEVPATH\""
			logmsg "btrfs check done"
		fi
	fi

	if [ "$DO_MOUNT" -eq 1 ]; then
		logmsg "mount ${DEVPATH} ${TEMPDIR}"
		mount -t "$FSTYPE" "$DEVPATH" "$TEMPDIR" ||
			errorexit "mount failed for \"$DEVPATH\" on \"$TEMPDIR\""
		logmsg "mount done"
		TMP_MOUNT_DONE=1
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
			REAL_MOUNTPOINT="$TEMPDIR"
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
		logmsg "cleanup unmount ${TEMPDIR}"
		umount "$TEMPDIR" ||
			errorexit "cleanup unmount failed for \"$TEMPDIR\""
		logmsg "cleanup unmount done"
		TMP_MOUNT_DONE=0
		rmdir "$TEMPDIR" 2>/dev/null || true
		rmdir "${TEMPDIR%/*}" 2>/dev/null || true
	fi

	if [ $RESIZEFS_FAILED -eq 1 ]; then
		errorexit "File system reduce failed."
	fi

	if [ "$DO_CRYPTRESIZE" -eq 1 ]; then
		NEWSIZESECTORS=$(( NEWSIZEBYTES / 512 ))
		logmsg "cryptsetup resize ${NEWSIZESECTORS} sectors ${DEVPATH}"
		cryptsetup resize --size "$NEWSIZESECTORS" "$DEVPATH" ||
			errorexit "cryptsetup resize failed on \"$DEVPATH\" to $NEWSIZESECTORS sectors"
		logmsg "cryptsetup done"
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
	cryptsetup resize --size "$NEWSIZESECTORS" "$DEVPATH" ||
		errorexit "cryptsetup resize failed on \"$DEVPATH\" to $NEWSIZESECTORS sectors"
	logmsg "cryptsetup done"

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

# Initialize to ensure clean state with set -u.  NEWSIZEBYTES stays empty
# when --newsizebytes is not given, so the btrfs extend path below can fall
# back to "max"; a literal 0 would defeat ${NEWSIZEBYTES:-max}.
FSTYPE=""
LVPATH=""
CRYPTPATH=""
NEWSIZEBYTES=""
MOUNT_OPTIONS=""
MOUNTDIR=""
TEMPDIR=""

trap 'cleanup $?' EXIT
trap 'cleanup 2' HUP INT QUIT ABRT TERM

OPTIONS=$("$GETOPT" -o h -l help,fsextend,fsreduce,cryptresize,mount,unmount,remount,fsck,fstype:,lvpath:,newsizebytes:,mountdir:,cryptpath: -n "${SCRIPTNAME}" -- "$@")
eval set -- "$OPTIONS"

while [ $# -gt 0 ]
do
	case $1 in
	--fsextend)	DO_FSEXTEND=1 ;;
	--fsreduce)	DO_FSREDUCE=1 ;;
	--cryptresize)	DO_CRYPTRESIZE=1 ;;
	--mount)	DO_MOUNT=1 ;;
	--unmount)	DO_UNMOUNT=1 ;;
	--fsck)		DO_FSCK=1 ;;
	--remount)	REMOUNT=1 ;;
	--fstype)	FSTYPE=$2; shift ;;
	--lvpath)	LVPATH=$2; shift ;;
	--newsizebytes)	NEWSIZEBYTES=$2; shift ;;
	--mountdir)	MOUNTDIR=$2; shift ;;
	--cryptpath)	CRYPTPATH=$2; shift ;;
	-h|--help)	usage ;;
	--)		shift; break ;;
	*)		errorexit "Unknown option \"$1\"." ;;
	esac
	shift
done

if [ "$UID" != 0 ] && [ "$EUID" != 0 ]; then
	errorexit "${SCRIPTNAME} must be run as root."
fi

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

DEVPATH=$(readlink -f "$DEVPATH") ||
	errorexit "Cannot resolve device path."
if [ ! -b "$DEVPATH" ]; then
	errorexit "Device is not a block device \"$DEVPATH\"."
fi

if [[ "$DO_UNMOUNT" -eq 1 && -z "$MOUNTDIR" ]]; then
	errorexit "Missing required --mountdir for --unmount."
fi

if [[ "$REMOUNT" -eq 1 && "$DO_UNMOUNT" -eq 0 ]]; then
	errorexit "--remount requires --unmount."
fi

if [[ "$DO_FSREDUCE" -eq 1 && "$FSTYPE" == "xfs" ]]; then
	errorexit "Cannot reduce xfs."
fi

if [[ "$DO_FSCK" -eq 1 && "$FSTYPE" == "xfs" ]]; then
	errorexit "Cannot use --fsck with xfs."
fi

# --cryptresize always needs a size, and --fsreduce needs one for every fs
# that can be reduced (xfs reduce is refused above).  --fsextend takes an
# optional size; when given it must still be a valid positive number.
if [[ "$DO_CRYPTRESIZE" -eq 1 ]] || [[ "$DO_FSREDUCE" -eq 1 && "$FSTYPE" != "xfs" ]]; then
	validate_newsizebytes 1
elif [[ "$DO_FSEXTEND" -eq 1 ]]; then
	validate_newsizebytes 0
fi

if [ "$DO_MOUNT" -eq 1 ]; then
	if test -n "${TMPDIR-}"; then
		if test "${TMPDIR#/}" = "$TMPDIR"; then
			errorexit "TMPDIR must be an absolute path."
		fi
		TMPDIR=$(readlink -f "$TMPDIR") ||
			errorexit "Cannot resolve TMPDIR \"$TMPDIR\"."
		validate_path TMPDIR "$TMPDIR"
	fi
	TEMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/${SCRIPTNAME}_XXXXXXXXXX") || errorexit "Failed to create temp dir."
	TEMPDIR="${TEMPDIR}/m"
	mkdir -m 0000 "$TEMPDIR" || errorexit "Failed to create temp mount point \"$TEMPDIR\"."
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
