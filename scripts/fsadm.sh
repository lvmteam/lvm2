#!/bin/bash
# shellcheck disable=SC2329 # functions invoked indirectly via $CMD
#
# Copyright (C) 2007-2026 Red Hat, Inc. All rights reserved.
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
#
# Author: Zdenek Kabelac <zkabelac at redhat.com>
#
# Script for resizing devices (usable for LVM resize)
#
# Requires bash (see shebang) and typical GNU/Linux userland (date, readlink,
# mktemp).  Conditional pipefail avoids early failure on shells without it.
#
# Needed utilities:
#   mount, umount, mktemp, awk, blockdev, blkid, fsck, readlink, stat,
#   find, xargs, head, cut, tr, lvm, dmsetup
#
# ext2/ext3/ext4: resize2fs, tune2fs
# reiserfs: resize_reiserfs, reiserfstune
# xfs: xfs_growfs, xfs_info, xfs_db, xfs_repair
# crypto_LUKS: cryptsetup
#
# Return values:
#   0 success
#   1 error
#   2 break detected
#   3 unsupported online filesystem check for given mounted fs

set -eu
# Older dash versions do not support pipefail.
if (set -o pipefail) 2>/dev/null; then
	set -o pipefail
fi

TOOL="fsadm"

PATH="/sbin:/usr/sbin:/bin:/usr/bin"

tool_usage() {
	cat <<-EOF
	  ${TOOL}: Utility to resize or check the filesystem on a device

	  ${TOOL} [options] check <device>
	      - Check the filesystem on device using fsck

	  ${TOOL} [options] resize <device> [<new_size>[BKMGTPE]]
	      - Change the size of the filesystem on device to new_size

	  Options:
	      -h | --help	 Show this help message
	      -v | --verbose	 Be verbose
	      -e | --ext-offline Unmount filesystem before ext2/ext3/ext4 resize
	      -f | --force	 Bypass sanity checks
	      -n | --dry-run	 Dry run: read devices and check, skip fs changes
	      -l | --lvresize	 Resize given device (if it is LVM device)
	      -c | --cryptresize Resize given crypt device
	      -y | --yes	 Answer "yes" at any prompts

	  new_size - Absolute number of filesystem blocks to be in the filesystem,
	             or an absolute size using a suffix (in powers of 1024).
	             If new_size is not supplied, the whole device is used.

	EOF

	exit 0
}

# External commands
TUNE_EXT="tune2fs"
RESIZE_EXT="resize2fs"
TUNE_REISER="reiserfstune"
RESIZE_REISER="resize_reiserfs"
TUNE_XFS="xfs_info"
RESIZE_XFS="xfs_growfs"
XFS_CHECK="xfs_check"
# XFS_DB reads geometry from the superblock without mounting the filesystem;
# xfs_info needs a mount point on older xfsprogs.
XFS_DB="xfs_db"
# XFS_REPAIR -n is used when XFS_CHECK is not found
XFS_REPAIR="xfs_repair"
FSCK="fsck"
CRYPTSETUP="cryptsetup"
STAT="stat"

MOUNT="mount"
UMOUNT="umount"
MKDIR="mkdir"
RMDIR="rmdir"
BLOCKDEV="blockdev"
BLKID="blkid"
DATE="date"
AWK="awk"
READLINK="readlink"
READLINK_E="-e"

# LVM and dmsetup commands are resolved and verified in init_lvm_tools()
# after CLI parsing so LVM_BINARY / DMSETUP_BINARY overrides are validated
# before use.

# _fsadm_cmd() passes --yes/--verbose/--force on command line but not --ext-offline,
# so EXTOFF must propagate via _FSADM_EXTOFF environment variable.

# Runtime state
EXTOFF=${_FSADM_EXTOFF:-0}
# DO_LVRESIZE: 0 off; 1 user passed --lvresize; 2 filesystem work done,
# run lvresize in cleanup (see prepare_resize and cleanup).
DO_LVRESIZE=0
LVRESIZE_SIZE=
DO_CRYPTRESIZE=0
DRY=0
ACTION=
DEVICE=
FORCE=
NEWSIZE=
VERB=
YES=
TEMPDIR=
CRYPT_RESIZE=
FSTYPE="unknown"
VOLUME="unknown"
DM_DEV_DIR="${DM_DEV_DIR:-/dev}"
BLOCKSIZE=
BLOCKCOUNT=
MOUNTPOINT=
MOUNTED=
REMOUNT=
PROCDIR="/proc"
PROCMOUNTS="$PROCDIR/mounts"
PROCSELFMOUNTINFO="$PROCDIR/self/mountinfo"

# Functions
verbose() {
	test -z "$VERB" || printf '%s\n' "$TOOL: $*" >&2
}

warning() {
	printf '%s\n' "$TOOL: WARNING: $*" >&2
}

# Support multi-line error messages
error() {
	local i

	for i in "$@"; do
		printf '%s\n' "$TOOL: $i" >&2
	done

	exit 1
}

print_command() {
	local ARG

	{
		for ARG in "$@"; do
			case "$ARG" in
			  *[!a-zA-Z0-9_./:=+-]*|"")
				printf " '"
				while :; do
					case "$ARG" in
					  *\'*) printf '%s%s' "${ARG%%\'*}" "'\\''"; ARG=${ARG#*\'} ;;
					  *) printf "%s'" "$ARG"; break ;;
					esac
				done ;;
			  *) printf ' %s' "$ARG" ;;
			esac
		done
		printf '\n'
	} >&2
}

dry() {
	if [ "$DRY" -ne 0 ]; then
		printf '%s: Dry execution:' "$TOOL" >&2
		print_command "$@"
		return 0
	fi
	verbose "Executing $*"
	"$@"
}

lvresize_reexec_only() { test "$DO_LVRESIZE" -eq 2; }

# Verify required filesystem utilities are installed
validate_fs_tools() {
	local FSTYPE=$1
	local PACKAGE CMD

	case "$1" in
	  ext[234])
		PACKAGE=e2fsprogs
		set -- "$TUNE_EXT" "$FSCK"
		if test "$ACTION" = resize; then set -- "$@" "$RESIZE_EXT"; fi ;;
	  reiserfs)
		PACKAGE=reiserfsprogs
		set -- "$FSCK"
		if test "$ACTION" = resize; then set -- "$@" "$TUNE_REISER" "$RESIZE_REISER"; fi ;;
	  xfs)
		PACKAGE=xfsprogs
		# check_xfs selects whichever read-only checker is installed.
		set --
		if test "$ACTION" = resize; then set -- "$TUNE_XFS" "$XFS_DB" "$RESIZE_XFS"; fi ;;
	  crypto_LUKS)
		PACKAGE=cryptsetup
		set -- "$CRYPTSETUP" ;;
	  *) return 0 ;;
	esac
	for CMD in "$@"; do
		command -v "$CMD" >/dev/null 2>&1 ||
			error "Utility $CMD required for $FSTYPE." "Please install $PACKAGE package."
	done
}

# Handle fsck return codes according to fsck(8) exit code specification
# Exit codes are bitmask sums: 1 = corrected, 2 = reboot required,
# 4 = errors uncorrected, 8 = operational error,
# 16 = usage error, 32 = canceled, 128 = shared library error
# In dry run, execute fsck when -n is present (read-only check on ext*).
# Mutating fsck without -n is logged via dry() only. ReiserFS is skipped in
# check() dry run because its checker is not side-effect free.
accept_fsck() {
	local RET=0
	local _a=

	if test "$DRY" -ne 0; then
		for _a in "$@"; do
			test "$_a" = "-n" && break
		done
		test "$_a" = "-n" || {
			dry "$@"
			return 0
		}
	fi

	"$@" || RET=$?

	test "$RET" -eq 0 && return 0

	test $(( RET & 128 )) -ne 0 &&
		error "Fsck shared library error on \"$VOLUME\"."
	test $(( RET & 32 )) -ne 0 &&
		error "Fsck canceled by user request."
	test $(( RET & 16 )) -ne 0 &&
		error "Fsck usage or syntax error." \
		      "This may indicate a bug in $TOOL script."
	test $(( RET & 8 )) -ne 0 &&
		error "Fsck operational error on \"$VOLUME\"." \
		      "Check fsck command syntax or system resources."
	test $(( RET & 4 )) -ne 0 &&
		error "Filesystem errors left uncorrected on \"$VOLUME\"." \
		      "Manual intervention may be required."
	test $(( RET & 2 )) -ne 0 &&
		warning "Filesystem was corrected but system should be rebooted."
	test $(( RET & 1 )) -ne 0 &&
		verbose "Filesystem errors were corrected on \"$VOLUME\"."

	# 191 = 1|2|4|8|16|32|128 (all defined fsck exit code bits)
	test $(( RET & ~191 )) -ne 0 &&
		error "Fsck failed with unexpected return code $RET on \"$VOLUME\"."

	return 0
}

cleanup() {
	trap '' EXIT HUP INT QUIT ABRT TERM

	# reset MOUNTPOINT - avoid recursion
	if test -n "$TEMPDIR" && test "$MOUNTPOINT" = "$TEMPDIR"; then
		MOUNTPOINT=
		temp_umount
		TEMPDIR=
	fi
	# remove mktemp dir if it was created but not fully used
	if test -n "$TEMPDIR"; then
		"$RMDIR" "$TEMPDIR" 2>/dev/null || warning "Failed to remove \"$TEMPDIR\"."
		"$RMDIR" "${TEMPDIR%/*}" 2>/dev/null || warning "Failed to remove \"${TEMPDIR%/*}\"."
		TEMPDIR=
	fi
	if test -n "$REMOUNT"; then
		verbose "Remounting unmounted filesystem back."
		dry "$MOUNT" "$VOLUME" "$MOUNTED" ||
			warning "Failed to remount \"$VOLUME\" on \"$MOUNTED\"."
	fi
	trap - EXIT HUP INT QUIT ABRT TERM

	test "$1" -eq 2 && verbose "Break detected."

	if [ "$1" -eq 0 ] && [ "$DO_LVRESIZE" -eq 2 ]; then
		# start LVRESIZE with the filesystem modification flag
		# and allow recursive call of fsadm
		_FSADM_EXTOFF=$EXTOFF
		export _FSADM_EXTOFF
		unset FSADM_RUNNING
		dry exec "$LVM" lvresize ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} ${YES:+"$YES"} --fs resize_fsadm -L"${LVRESIZE_SIZE}b" "$VOLUME_ORIG"
	fi

	# error exit status for break
	exit "${1:-1}"
}

# Print a normalized unsigned decimal that fits signed 64-bit arithmetic.
parse_number() {
	local NUM=$1

	case "$NUM" in
	  *[!0-9]*|"") printf '%s\n' "$TOOL: Expected decimal digits, got \"$1\"." >&2; return 1 ;;
	esac
	NUM=${NUM#"${NUM%%[!0]*}"}
	NUM=${NUM:-0}
	# Values must fit in signed 64-bit, whose maximum is 9223372036854775807.
	# Comparisons with larger literals wrap in some shells, so a 19-digit
	# value is checked digit-by-digit against that boundary:
	#   leading digit 9 -> remaining 18 digits must be <= 9223372036854775807 - 9e18 = 223372036854775807
	#   leading digit <9 -> always fits, 19-digit prefix is at most 8 999 999 999 999 999 999
	#   more than 19 digits -> always overflows
	if test "${#NUM}" -gt 19 ||
	   { test "${#NUM}" -eq 19 &&
	     case "$NUM" in
	       9*) test "${NUM#?}" -gt 223372036854775807 ;;
	       *) false ;;
	     esac
	   }; then
		printf '%s\n' "$TOOL: Number \"$1\" exceeds signed 64-bit range." >&2
		return 1
	fi
	printf '%s\n' "$NUM"
}

# convert parameter from Bytes/Kilo/Mega/Giga/Tera/Peta/Exa and blocks
# (2^(0/10/20/30/40/50/60))
# Sets: NEWSIZE, NEWBLOCKCOUNT
decode_size() {
	local NUM
	local SCALE

	NUM=$(parse_number "${1%[bBkKmMgGtTpPeE]}") || error "Invalid size value \"$1\"."
	case "$1" in
	  *[bB]) SCALE=1 ;;
	  *[kK]) SCALE=$(( 1 << 10 )) ;;
	  *[mM]) SCALE=$(( 1 << 20 )) ;;
	  *[gG]) SCALE=$(( 1 << 30 )) ;;
	  *[tT]) SCALE=$(( 1 << 40 )) ;;
	  *[pP]) SCALE=$(( 1 << 50 )) ;;
	  *[eE]) SCALE=$(( 1 << 60 )) ;;
	      *) SCALE=$2 ;;
	esac
	test "$NUM" -gt 0 || error "Size must be greater than zero."
	test "$NUM" -le "$(( 9223372036854775807 / SCALE ))" || error "Size value overflow."
	NEWSIZE=$(( NUM * SCALE ))
	NEWBLOCKCOUNT=$(( NEWSIZE / $2 ))
	test "$NEWBLOCKCOUNT" -gt 0 || error "Size is smaller than one filesystem block."
}

# Prepare filesystem resizing, or request an LV hand-off from the caller.
# Sets: NEWSIZE, NEWBLOCKCOUNT, LVRESIZE_SIZE, DO_LVRESIZE
prepare_resize() {
	decode_size "$1" "$2"
	if test "$DRY" -ne 0 && test "$NEWSIZE" -gt "$DEVSIZE"; then
		warning "Requested size exceeds the current device size; growth depends on enlarging the underlying device."
	fi

	if [ "$DO_LVRESIZE" -eq 1 ]; then
		LVRESIZE_SIZE=${LVRESIZE_SIZE:-$NEWSIZE}
		# Dry runs continue checking; real runs return to cleanup for lvresize.
		if test "$DRY" -eq 0; then
			DO_LVRESIZE=2
		fi
	fi
}

# Outputs: major:minor from a device node path; does not change device state.
# Handles /dev/dm-* via sysfs, other devices via stat
detect_major_minor() {
	local MAJOR MINOR
	local STATOUT

	case "$1" in
	  /dev/dm-[0-9]*)
		IFS=: read -r MAJOR MINOR <"/sys/block/${1#/dev/}/dev" 2>/dev/null ||
			return 1
		case "$MAJOR" in *[!0-9]*|"") return 1 ;; esac
		case "$MINOR" in *[!0-9]*|"") return 1 ;; esac
		;;
	  *)
		STATOUT=$("$STAT" --format '0x%t:0x%T' "$1") || return 1
		MAJOR=$(( ${STATOUT%%:*} ))
		MINOR=$(( ${STATOUT#*:} ))
		;;
	esac
	printf '%s:%s\n' "$MAJOR" "$MINOR"
}

# Query one dmsetup info field using a major:minor device number.
# Failures are not fatal: stderr is suppressed and an empty value is
# returned so callers do not have to handle command failures.
device_info() {
	local DEVNO=$1

	"$DMSETUP" info -c --noheadings -j "${DEVNO%:*}" -m "${DEVNO#*:}" -o "$2" 2>/dev/null || true
}

# Check whether the device-mapper device with the given major:minor is open.
device_is_open() {
	local OPEN
	OPEN=$(device_info "$1" open)
	test "${OPEN:-0}" -gt 0 2>/dev/null
}

# Outputs: MAJOR:MINOR from a dev_t value
decode_major_minor() {
	# 0x00000fff00  mask MAJOR
	# 0xfffff000ff  mask MINOR

	#MINOR=$(( $1 / 1048576 ))
	#MAJOR=$(( ($1 - ${MINOR} * 1048576) / 256 ))
	#MINOR=$(( $1 - ${MINOR} * 1048576 - ${MAJOR} * 256 + ${MINOR} * 256))

	echo "$(( ( $1 >> 8 ) & 4095 )):$(( ( ( $1 >> 12 ) & 268435200 ) | ( $1 & 255 ) ))"
}

# detect filesystem type on the given device and validate required tools
# not using blkid option '-o value' to be compatible with older version
# Sets: FSTYPE
detect_fstype() {
	FSTYPE=$("$BLKID" -c /dev/null -s TYPE "$1" || true)
	test -n "$FSTYPE" || error "Cannot get filesystem type of \"$1\"."
	FSTYPE=${FSTYPE##*TYPE=\"} # cut quotation marks
	FSTYPE=${FSTYPE%%\"*}
	verbose "\"$FSTYPE\" filesystem found on \"$1\"."
	validate_fs_tools "$FSTYPE"
}

# detect filesystem on the given device
# dereference device name if it is symbolic link
# Sets: VOLUME, VOLUME_ORIG, RVOLUME, FSTYPE, MAJORMINOR
# MAJORMINOR identifies the current VOLUME; probes of other devices stay local.
detect_fs() {
	local SYSVOLUME

	test -n "${VOLUME_ORIG-}" || VOLUME_ORIG=$1
	case "$1" in
	  "${DM_DEV_DIR}/"*) VOLUME=$1 ;;
	  *) VOLUME="$DM_DEV_DIR/$1" ;;
	esac
	VOLUME=$("$READLINK" "$READLINK_E" "$VOLUME" 2>/dev/null || true)
	test -n "$VOLUME" || error "Cannot get readlink \"$1\"."
	RVOLUME=$VOLUME
	case "$RVOLUME" in
	  # hardcoded /dev  since udev does not create these entries elsewhere
	  /dev/dm-[0-9]*)
		read -r SYSVOLUME <"/sys/block/${RVOLUME#/dev/}/dm/name" 2>/dev/null &&
			VOLUME="$DM_DEV_DIR/mapper/$SYSVOLUME"
		;;
	esac
	MAJORMINOR=$(detect_major_minor "$RVOLUME") ||
		error "Cannot get major:minor for \"$VOLUME\"."
	detect_fstype "$VOLUME"
}

# Reject a mounted device number that differs from the current VOLUME.
validate_mounted_major_minor() {
	local REFNAME
	local CURNAME

	if [ "$1" != "$MAJORMINOR" ]; then
		REFNAME=$(device_info "$1" name)
		CURNAME=$(device_info "$MAJORMINOR" name)
		error "Cannot $ACTION device \"$VOLUME\" without umounting filesystem \"$MOUNTED\" first." \
		      "Mounted filesystem is using device \"$REFNAME\", but referenced device is \"$CURNAME\"." \
		      "Filesystem utilities currently do not support renamed devices."
	fi
}

# ATM fsresize & fsck tools are not able to work properly
# when mounted device has changed its name.
# So whenever such device no longer exists with original name
# abort further command processing
check_valid_mounted_device() {
	local VOL
	local CURNAME
	local MOUNTEDMAJORMINOR
	local SUGGEST

	VOL=$("$READLINK" "$READLINK_E" "$1" 2>/dev/null || true)
	CURNAME=$(device_info "$MAJORMINOR" name)
	SUGGEST="Possibly device \"$1\" has been renamed to \"$CURNAME\"?"
	test -n "$CURNAME" || SUGGEST="Mounted volume is not a device mapper device."

	test -n "$VOL" ||
		error "Cannot access device \"$1\" referenced by mounted filesystem \"$MOUNTED\"." \
		"$SUGGEST" \
		"Filesystem utilities currently do not support renamed devices."

	MOUNTEDMAJORMINOR=$(detect_major_minor "$VOL") ||
		error "Cannot get major:minor for \"$VOL\" mounted on \"$MOUNTED\"."

	validate_mounted_major_minor "$MOUNTEDMAJORMINOR"
}

# Sets: MOUNTED
detect_mounted_with_proc_self_mountinfo() {
	local MOUNTDEV
	local RAW

	# shellcheck disable=SC2016 # awk field refs, not shell vars
	RAW=$("$AWK" -v mm="$MAJORMINOR" '$3 == mm {print; exit}' "$PROCSELFMOUNTINFO" 2>/dev/null) ||
		error "Cannot read mount information from \"$PROCSELFMOUNTINFO\"."

	# If not found in self mountinfo but device is open,
	# scan all /proc/*/mountinfo (handles cgroup mounts)
	if test -z "$RAW" &&
	   device_is_open "$MAJORMINOR"; then
		# shellcheck disable=SC2016 # awk field refs, not shell vars
		RAW=$(find "$PROCDIR" -maxdepth 2 -name mountinfo -print0 2>/dev/null |
			xargs -0 "$AWK" -v mm="$MAJORMINOR" '$3 == mm {print; exit}' 2>/dev/null |
			head -1 2>/dev/null || true)
	fi

	# Field 5 is the mount point; printf %b decodes \040 etc.
	# shellcheck disable=SC2016 # awk field refs, not shell vars
	MOUNTED=$("$AWK" 'NR==1 { print $5; exit }' <<-EOF
		$RAW
	EOF
	)
	MOUNTED=$(printf '%b' "$MOUNTED")

	test -n "$MOUNTED" || return 1

	# extract 2nd field after ' - ' separator as mounted device
	MOUNTDEV=$(printf '%s\n' "${RAW##* - }" | cut -d ' ' -f 2)
	MOUNTDEV=$(printf '%b' "$MOUNTDEV")
	check_valid_mounted_device "$MOUNTDEV"
}

# With older systems without /proc/*/mountinfo we may need to check
# every mount point as cannot easily depend on the name of mounted
# device (which could have been renamed).
# We need to visit every mount point and check its major minor
# Sets: MOUNTED
detect_mounted_with_proc_mounts() {
	local MOUNTDEV
	local STATOUT
	local i

	# Strategy 1: match by device name in /proc/mounts
	# shellcheck disable=SC2016 # awk field refs, not shell vars
	MOUNTED=$("$AWK" -v vol="$VOLUME" -v rvol="$RVOLUME" \
		'$1 == vol || $1 == rvol {print; exit}' "$PROCMOUNTS") ||
		error "Cannot read mount information from \"$PROCMOUNTS\"."

	# cut device name prefix and trim everything past mountpoint
	# printf translates \040 to spaces
	# /proc/mounts format: device mountpoint fstype options ...
	MOUNTDEV=$(printf '%b' "${MOUNTED%% *}")
	MOUNTED=${MOUNTED#* }
	MOUNTED=$(printf '%b' "${MOUNTED%% *}")

	# Strategy 2: fall back to mount command output
	if test -z "$MOUNTED"; then
		# will not work with spaces in paths
		# shellcheck disable=SC2016 # awk field refs, not shell vars
		MOUNTED=$(LC_ALL=C "$MOUNT" | "$AWK" -v vol="$VOLUME" -v rvol="$RVOLUME" \
						'($1 == vol || $1 == rvol) && !found {print; found=1}') ||
			error "Cannot read mount command output."
		# mount format: device on mountpoint type fstype ...
		MOUNTDEV=${MOUNTED%% on *}
		MOUNTED=${MOUNTED##* on }
		MOUNTED=${MOUNTED% type *} # allow type in the mount name
	fi

	if test -n "$MOUNTED"; then
		check_valid_mounted_device "$MOUNTDEV"
		return 0  # mounted
	fi

	# Strategy 3: device is open but not found by name,
	# check every mount point against MAJOR:MINOR
	if device_is_open "$MAJORMINOR"; then
		while read -r i; do
			MOUNTDEV=$(printf '%b' "${i%% *}")
			MOUNTED=${i#* }
			MOUNTED=$(printf '%b' "${MOUNTED%% *}")
			STATOUT=$("$STAT" --format "%d" "$MOUNTED" 2>/dev/null) || continue
			if test "$(decode_major_minor "$STATOUT")" = "$MAJORMINOR"; then
				check_valid_mounted_device "$MOUNTDEV"
				return 0
			fi
		done < "$PROCMOUNTS"
	fi

	MOUNTED=
	return 1  # nothing is mounted
}

# check if the given device is already mounted and where
# Sets: MOUNTED (mountpoint path or empty)
# FIXME: resolve swap usage and device stacking
detect_mounted() {
	if test -e "$PROCSELFMOUNTINFO"; then
		detect_mounted_with_proc_self_mountinfo
	elif test -e "$PROCMOUNTS"; then
		detect_mounted_with_proc_mounts
	else
		error "Cannot detect mounted device \"$VOLUME\"."
	fi
}

# get the full size of device in bytes
# Sets: DEVSIZE
detect_device_size() {
	local SECTORS

	# check if blockdev supports getsize64
	if ! DEVSIZE=$("$BLOCKDEV" --getsize64 "$VOLUME" 2>/dev/null) || test -z "$DEVSIZE"; then
		SECTORS=$("$BLOCKDEV" --getsize "$VOLUME") || error "Cannot read size of device \"$VOLUME\"."
		SECTORS=$(parse_number "$SECTORS") || error "Invalid device sector count on \"$VOLUME\"."
		test "$SECTORS" -le "$(( 9223372036854775807 / 512 ))" || error "Device size overflow."
		# --getsize always reports 512-byte sectors, even on 4K devices.
		DEVSIZE=$(( SECTORS * 512 ))
	fi
	DEVSIZE=$(parse_number "$DEVSIZE") || error "Invalid device size on \"$VOLUME\"."
	test "$DEVSIZE" -gt 0 2>/dev/null ||
		error "Invalid device size \"$DEVSIZE\"."
}

# Sets: TEMPDIR
temp_mount() {
	if test -n "${TMPDIR-}" && test "${TMPDIR#/}" = "$TMPDIR"; then
		error "TMPDIR must be an absolute path."
	fi
	# Reject a TMPDIR a non-root user could replace.  Parent directories
	# are not checked for the same reason as binary overrides.
	if test -n "${TMPDIR-}"; then
		TMPDIR=$("$READLINK" -f "$TMPDIR") ||
			error "Cannot resolve TMPDIR \"$TMPDIR\"."
		validate_path TMPDIR "$TMPDIR"
	fi
	TEMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/${TOOL}_XXXXXXXXXX") || error "Failed to create temporary directory."
	TEMPDIR="${TEMPDIR}/m"
	dry "$MKDIR" -m 0000 "$TEMPDIR" || error "Failed to create temporary mount point \"$TEMPDIR\"."
	dry "$MOUNT" "$VOLUME" "$TEMPDIR" || error "Failed to mount \"$VOLUME\" on \"$TEMPDIR\"."
}

temp_umount() {
	dry "$UMOUNT" "$TEMPDIR" || error "Failed to umount \"$TEMPDIR\"."
	dry "$RMDIR" "${TEMPDIR}" || error "Failed to remove \"$TEMPDIR\"."
	dry "$RMDIR" "${TEMPDIR%/*}" || error "Failed to remove \"${TEMPDIR%/*}\"."
}

yes_no() {
	local ANS

	printf '%s ? [Y|n] ' "$*"

	[ -n "$YES" ] && { echo y; return 0; }

	# Without -y: EOF or closed stdin yields "no" (non-interactive safe).
	while read -r ANS; do
		case "$ANS" in
		  y|Y) echo y; return 0 ;;
		  n|N) break ;;
		  "")  [ -t 0 ] && { echo y; return 0; } ;;
		esac
	done

	echo n
	return 1
}

try_umount() {
	test "$DRY" -ne 0 || yes_no "Do you want to unmount \"$MOUNTED\"" ||
		error "Cannot proceed with mounted filesystem \"$MOUNTED\"."
	dry "$UMOUNT" "$MOUNTED" || error "Failed to unmount \"$MOUNTED\"."
}

validate_parsing() {
	BLOCKSIZE=$(parse_number "$BLOCKSIZE") || error "Invalid block size from $1 for \"$VOLUME\"."
	BLOCKCOUNT=$(parse_number "$BLOCKCOUNT") || error "Invalid block count from $1 for \"$VOLUME\"."
	test "$BLOCKSIZE" -gt 0 && test "$BLOCKCOUNT" -gt 0 &&
		test "$BLOCKCOUNT" -le "$(( 9223372036854775807 / BLOCKSIZE ))" ||
		error "Invalid filesystem geometry from $1 for \"$VOLUME\"."
}

# Choose fsck mode flags: an explicit -y always wins, ask interactively
# only when a terminal is available on both stdin and stdout (e2fsck
# refuses interactive repairs otherwise), and fall back to preen (-p).
get_fsck_mode() {
	if test -n "$YES"; then
		printf '%s' "$YES"
	elif test -t 0 && test -t 1; then
		:
	else
		printf '%s' "-p"
	fi
}

####################################
# Resize ext2/ext3/ext4 filesystem
# - unmounted or mounted for upsize
# - unmounted for downsize
####################################
resize_ext() {
	local IS_MOUNTED=0
	local FLAG
	local i
	local OUTPUT

	if detect_mounted; then
		IS_MOUNTED=1
	fi

	verbose "Parsing $TUNE_EXT -l \"$VOLUME\"."
	OUTPUT=$(LC_ALL=C "$TUNE_EXT" -l "$VOLUME") || error "Cannot read $TUNE_EXT geometry on \"$VOLUME\"."
	BLOCKSIZE='' BLOCKCOUNT=''
	while read -r i; do
		case "$i" in
		  "Block size"*) BLOCKSIZE=${i##*  } ;;
		  "Block count"*) BLOCKCOUNT=${i##*  } ;;
		esac
	done <<-EOF
		$OUTPUT
	EOF
	validate_parsing "$TUNE_EXT"
	prepare_resize "$1" "$BLOCKSIZE"
	lvresize_reexec_only && return 0
	if test "$DRY" -ne 0; then
		if test "$IS_MOUNTED" -eq 0; then
			verbose "Checking unmounted filesystem without repairs."
			accept_fsck "$FSCK" -f -n "$VOLUME"
		else
			warning "Skipping offline check of mounted filesystem \"$VOLUME\" in dry run."
		fi
	fi
	if test "$NEWBLOCKCOUNT" -lt "$BLOCKCOUNT" || test "$EXTOFF" -eq 1; then
		if test "$IS_MOUNTED" -eq 1; then
			verbose "\"$FSTYPE\" resizes only unmounted filesystem."
			try_umount
		fi
		if test "$DRY" -eq 0; then
			REMOUNT=$MOUNTED
		fi
		if test "$DRY" -eq 0 || test "$IS_MOUNTED" -eq 1; then
			# Forced fsck -f for unmounted extX filesystem.
			FLAG=$(get_fsck_mode)
			accept_fsck "$FSCK" -f ${FLAG:+"$FLAG"} "$VOLUME"
		fi
	fi

	verbose "Resizing filesystem on device \"$VOLUME\" to $NEWSIZE bytes ($BLOCKCOUNT -> $NEWBLOCKCOUNT blocks of $BLOCKSIZE bytes)."
	dry "$RESIZE_EXT" ${FORCE:+"$FORCE"} "$VOLUME" "$NEWBLOCKCOUNT"
}

#############################
# Resize reiserfs filesystem
# - unmounted for upsize
# - unmounted for downsize
# Sets: REMOUNT
#############################
resize_reiser() {
	local i
	local OUTPUT

	verbose "Parsing $TUNE_REISER \"$VOLUME\"."
	OUTPUT=$(LC_ALL=C "$TUNE_REISER" "$VOLUME") || error "Cannot read $TUNE_REISER geometry on \"$VOLUME\"."
	BLOCKSIZE='' BLOCKCOUNT=''
	while read -r i; do
		case "$i" in
		  "Blocksize"*) BLOCKSIZE=${i##*: } ;;
		  "Count of blocks"*) BLOCKCOUNT=${i##*: } ;;
		esac
	done <<-EOF
		$OUTPUT
	EOF

	validate_parsing "$TUNE_REISER"
	prepare_resize "$1" "$BLOCKSIZE"
	lvresize_reexec_only && return 0
	if detect_mounted; then
		verbose "ReiserFS resizes only unmounted filesystem."
		try_umount
	fi
	if test "$DRY" -eq 0; then
		REMOUNT=$MOUNTED
	fi
	test "$DRY" -eq 0 || warning "Only ReiserFS geometry was checked; its filesystem checker can replay the journal."
	verbose "Resizing filesystem on device \"$VOLUME\" to $NEWSIZE bytes ($BLOCKCOUNT -> $NEWBLOCKCOUNT blocks of $BLOCKSIZE bytes)."
	if [ -n "$YES" ]; then
		# dry does not read stdin: piping echo y could fail with SIGPIPE
		# under pipefail. Use a here-document without a separate writer.
		dry "$RESIZE_REISER" -s "$NEWSIZE" "$VOLUME" <<-EOF
			y
		EOF
	else
		dry "$RESIZE_REISER" -s "$NEWSIZE" "$VOLUME"
	fi
}

########################
# Resize XFS filesystem
# - mounted for upsize
# - cannot downsize
# Sets: MOUNTPOINT
########################
resize_xfs() {
	local i
	local OUTPUT
	local PARSE_TOOL

	if detect_mounted; then
		MOUNTPOINT=$MOUNTED
	elif [ "$DRY" -ne 0 ]; then
		# In dryrun do not mount the device; xfs_info requires a mount
		# point on older xfsprogs, so read the superblock with xfs_db.
		MOUNTPOINT=$VOLUME
	else
		temp_mount
		MOUNTPOINT=$TEMPDIR
	fi

	if test "$DRY" -ne 0 && test -z "$MOUNTED"; then
		PARSE_TOOL=$XFS_DB
		verbose "Parsing $PARSE_TOOL \"$VOLUME\"."
		OUTPUT=$(LC_ALL=C "$PARSE_TOOL" -r -c "sb 0" -c "print blocksize" -c "print dblocks" "$VOLUME") ||
			error "Cannot read $PARSE_TOOL geometry on \"$VOLUME\"."
	else
		PARSE_TOOL=$TUNE_XFS
		verbose "Parsing $PARSE_TOOL \"$MOUNTPOINT\"."
		OUTPUT=$(LC_ALL=C "$PARSE_TOOL" "$MOUNTPOINT") || error "Cannot read $PARSE_TOOL geometry on \"$MOUNTPOINT\"."
	fi
	BLOCKSIZE='' BLOCKCOUNT=''
	while read -r i; do
		case "$i" in
		  "data"*) BLOCKSIZE=${i##*bsize=}; BLOCKCOUNT=${i##*blocks=} ;;
		  "blocksize"*) BLOCKSIZE=${i##* = } ;;
		  "dblocks"*) BLOCKCOUNT=${i##* = } ;;
		esac
	done <<-EOF
		$OUTPUT
	EOF

	BLOCKSIZE=${BLOCKSIZE%%[!0-9]*}
	BLOCKCOUNT=${BLOCKCOUNT%%[!0-9]*}
	validate_parsing "$PARSE_TOOL"
	prepare_resize "$1" "$BLOCKSIZE"
	lvresize_reexec_only && return 0
	if test "$DRY" -ne 0; then
		if test -z "$MOUNTED"; then
			check_xfs
			warning "XFS remains unmounted; growth requires mounting \"$VOLUME\"."
		elif test "$NEWSIZE" -le "$DEVSIZE"; then
			# xfs_growfs -n is read-only; run it for real (not via dry()).
			"$RESIZE_XFS" -n -D "$NEWBLOCKCOUNT" "$MOUNTPOINT" ||
				error "XFS growth validation failed."
		else
			# LVM test mode may be planning growth of the underlying device.
			"$RESIZE_XFS" -n "$MOUNTPOINT" ||
				error "XFS geometry validation failed."
		fi
	fi
	if [ "$NEWBLOCKCOUNT" -gt "$BLOCKCOUNT" ]; then
		verbose "Resizing XFS on \"$VOLUME\" to $NEWBLOCKCOUNT blocks."
		dry "$RESIZE_XFS" -D "$NEWBLOCKCOUNT" "$MOUNTPOINT"
	elif [ "$NEWBLOCKCOUNT" -eq "$BLOCKCOUNT" ]; then
		verbose "XFS filesystem already has the right size."
	else
		error "XFS filesystem shrinking is unsupported." \
		      "Current size: $(( BLOCKCOUNT * BLOCKSIZE )) bytes, requested: $NEWSIZE bytes."
	fi
}

# Find active LUKS device on original volume
# 1) look for LUKS device with well-known UUID format (CRYPT-LUKS[12]-<uuid>-<dmname>)
# 2) the dm-crypt device has to be on top of original device (don't support detached LUKS headers)
# Sets: CRYPT_NAME, CRYPT_DATA_OFFSET
detect_luks_device() {
	local LUKS_VERSION=
	local LUKS_UUID=

	CRYPT_NAME=""
	CRYPT_DATA_OFFSET=""

	# shellcheck disable=SC2016 # awk field refs, not shell vars
	read -r LUKS_VERSION LUKS_UUID <<-EOF
		$("$CRYPTSETUP" luksDump "$VOLUME" 2>/dev/null |
			"$AWK" -F: '/Version:/ { gsub(/[[:space:]]/, "", $2); v = $2 }
				/UUID:/    { gsub(/[[:space:]-]/, "", $2); u = $2 }
				END        { print v, u }')
	EOF

	case "$LUKS_VERSION" in
	  1|2) ;;
	  *) error "Unsupported LUKS version \"$LUKS_VERSION\" on volume \"$VOLUME\"." ;;
	esac

	case "$LUKS_UUID" in
	  *[!0-9a-fA-F]*|"") error "Invalid LUKS UUID on volume \"$VOLUME\"." ;;
	esac
	LUKS_UUID="CRYPT-LUKS$LUKS_VERSION-${LUKS_UUID}-"

	CRYPT_NAME=$("$DMSETUP" info -c --noheadings -S "UUID=~^$LUKS_UUID&&segments=1&&devnos_used='$MAJORMINOR'" -o name)
	test -n "$CRYPT_NAME" && CRYPT_DATA_OFFSET=$("$DMSETUP" table "$CRYPT_NAME" 2>/dev/null | cut -d ' ' -f 8 || true)

	# LUKS device must be active and mapped over volume where detected
	if [ -z "$CRYPT_NAME" ] || [ -z "$CRYPT_DATA_OFFSET" ]; then
		error "Cannot find active LUKS device for \"$VOLUME\"." \
		      "LUKS device must be unlocked before resizing:" \
		      "  cryptsetup luksOpen \"$VOLUME\" <name>"
	fi
	case "$CRYPT_DATA_OFFSET" in
	  *[!0-9]*) error "Invalid LUKS data offset \"$CRYPT_DATA_OFFSET\" for \"$VOLUME\"." ;;
	esac
}

######################################
# Resize active LUKS device
# - LUKS must be active for fs resize
# Sets: VOLUME, NEWSIZE, CRYPT_RESIZE, CRYPT_RESIZE_BLOCKS
######################################
resize_luks() {
	local L_NEWSIZE
	local L_NEWBLOCKCOUNT
	local NAME
	detect_luks_device

	NAME=$CRYPT_NAME

	verbose "Found active LUKS device \"$NAME\" for volume \"$VOLUME\"."

	prepare_resize "$1" 512
	lvresize_reexec_only && return 0

	test $(( NEWSIZE % 512 )) -eq 0 ||
		error "New size is not sector aligned."

	test $(( NEWBLOCKCOUNT - CRYPT_DATA_OFFSET )) -ge 1 ||
		error "New size is smaller than minimum ($(( (CRYPT_DATA_OFFSET + 1) * 512 )) bytes) for LUKS volume \"$VOLUME\"."

	L_NEWBLOCKCOUNT=$(( NEWBLOCKCOUNT - CRYPT_DATA_OFFSET ))
	L_NEWSIZE=$(( L_NEWBLOCKCOUNT * 512 ))

	VOLUME="$DM_DEV_DIR/mapper/$NAME"
	detect_device_size

	if [ "$DEVSIZE" -gt "$L_NEWSIZE" ]; then
		# shrink fs on LUKS device first
		resize "$DM_DEV_DIR/mapper/$NAME" "$L_NEWSIZE"b
	else
		# grow: validate inner fs tools before LUKS modification
		detect_fstype "$VOLUME"
	fi

	# resize LUKS device
	dry "$CRYPTSETUP" resize "$NAME" --size "$L_NEWBLOCKCOUNT" ||
		error "Failed to resize LUKS device \"$NAME\"." \
		      "Target size: $L_NEWSIZE bytes ($L_NEWBLOCKCOUNT sectors)."

	if [ "$DEVSIZE" -le "$L_NEWSIZE" ]; then
		# grow fs on top of LUKS device
		resize "$DM_DEV_DIR/mapper/$NAME" "$L_NEWSIZE"b
	fi
}

# Sets: CRYPT_RESIZE_BLOCKS, CRYPT_RESIZE (grow|shrink)
detect_crypt_device() {
	local CRYPT_TYPE
	# Decoded size for crypt resize lives here; prepare_resize/decode_size
	# must not replace the global NEWSIZE string used by resize() later.
	local NEWSIZE

	command -v "$CRYPTSETUP" >/dev/null 2>&1 ||
		error "$CRYPTSETUP utility required to resize crypt device." \
		      "Please install cryptsetup package."

	# shellcheck disable=SC2016 # awk field refs, not shell vars
	CRYPT_TYPE=$("$CRYPTSETUP" status "$1" 2>/dev/null | "$AWK" '/type:/ {print $NF; exit}' || true)

	test -n "$CRYPT_TYPE" ||
		error "Failed to detect crypt device type on \"$1\"." \
		      "Device may not be active or not a valid crypt device."

	case "$CRYPT_TYPE" in
	  LUKS[12]|PLAIN)
		verbose "\"$1\" crypt device is type \"$CRYPT_TYPE\"." ;;
	  *)
		error "Unsupported crypt type \"$CRYPT_TYPE\" on device \"$1\"." \
		      "Only LUKS1, LUKS2, and PLAIN types are supported."
	esac

	prepare_resize "$2" 512
	lvresize_reexec_only && return 0

	test $(( NEWSIZE % 512 )) -eq 0 ||
		error "New size is not sector aligned."

	CRYPT_RESIZE_BLOCKS=$NEWBLOCKCOUNT

	if [ "$DEVSIZE" -ge "$NEWSIZE" ]; then
		CRYPT_RESIZE="shrink"
	else
		CRYPT_RESIZE="grow"
	fi
}

#################################
# Resize active crypt device
#  (on direct user request only)
#################################
resize_crypt() {
	dry "$CRYPTSETUP" resize "$1" --size "$CRYPT_RESIZE_BLOCKS" ||
		error "Failed to resize crypt device \"$1\"." \
		      "Target size: $CRYPT_RESIZE_BLOCKS sectors."
}

####################
# Resize filesystem
# Sets: NEWSIZE, NEWSIZE_ORIG
####################
resize() {
	local CMD
	local TARGET

	NEWSIZE=$2
	detect_fs "$1"
	detect_device_size
	verbose "Device \"$VOLUME\" size is $DEVSIZE bytes."

	# if the size parameter is missing use device size
	if test -z "$NEWSIZE"; then
		NEWSIZE=${DEVSIZE}b
	fi
	NEWSIZE_ORIG=${NEWSIZE_ORIG:-$NEWSIZE}

	case "$FSTYPE" in
	  ext[234])	CMD=resize_ext ;;
	  reiserfs)	CMD=resize_reiser ;;
	  xfs)		CMD=resize_xfs ;;
	  crypto_LUKS)	CMD=resize_luks ;;
	  *) error "Filesystem \"$FSTYPE\" on device \"$VOLUME\" is not supported by this tool." ;;
	esac

	if test "$DO_CRYPTRESIZE" -ne 0; then
		# --cryptresize operates on the crypt device only.
		detect_crypt_device "$VOLUME_ORIG" "$NEWSIZE_ORIG"
		lvresize_reexec_only && return 0
	fi

	if test "$CRYPT_RESIZE" = "grow"; then
		resize_crypt "$VOLUME_ORIG"
	fi

	# Nested resize calls (e.g. for LUKS) overwrite the global
	# NEWSIZE, so keep the requested size for the error message.
	TARGET=$NEWSIZE
	"$CMD" "$NEWSIZE" ||
		error "\"$FSTYPE\" resize failed on \"$VOLUME\"." \
		      "Target size: $TARGET bytes."
	lvresize_reexec_only && return 0

	if test "$CRYPT_RESIZE" = "shrink"; then
		resize_crypt "$VOLUME_ORIG"
	fi

	return 0
}

####################################
# Calculate diff between two dates
#  LC_ALL=C input is expected the
#  only one supported
####################################
diff_dates() {
	local D1
	local D2

	if ! D1=$("$DATE" -u -d"$1" +%s 2>/dev/null) ||
	   ! D2=$("$DATE" -u -d"$2" +%s 2>/dev/null); then
		verbose "Cannot parse date \"$1\" or \"$2\"."
		echo 1
		return
	fi
	echo "$(( D1 - D2 ))"
}

check_luks() {
	detect_luks_device

	check "$DM_DEV_DIR/mapper/$CRYPT_NAME"
}

# Both XFS checkers are read-only, including during dry runs.
check_xfs() {
	if command -v "$XFS_REPAIR" >/dev/null 2>&1; then
		"$XFS_REPAIR" -n -o force_geometry "$VOLUME" ||
			error "XFS repair check failed on \"$VOLUME\"." \
			      "Filesystem may have errors requiring repair."
	elif command -v "$XFS_CHECK" >/dev/null 2>&1; then
		"$XFS_CHECK" "$VOLUME" || error "XFS check failed on \"$VOLUME\"."
	else
		error "Neither xfs_repair nor xfs_check found." \
		      "Please install xfsprogs package."
	fi
}

###################
# Check filesystem
###################
check() {
	local FLAG
	local FORCE=${FORCE-}
	local LASTMOUNT
	local LASTCHECKED
	local LASTDIFF
	local i
	local OUTPUT

	detect_fs "$1"
	if detect_mounted; then
		warning "Skipping filesystem check for device \"$VOLUME\" as the filesystem is mounted on \"$MOUNTED\"."
		exit 3
	fi

	case "$FSTYPE" in
	  ext[234])
		LASTMOUNT=""
		LASTCHECKED=""
		OUTPUT=$(LC_ALL=C "$TUNE_EXT" -l "$VOLUME") || error "Cannot read $TUNE_EXT metadata on \"$VOLUME\"."
		while read -r i; do
			case "$i" in
			  "Last mount time"*) LASTMOUNT=${i##*: } ;;
			  "Last checked"*) LASTCHECKED=${i##*: } ;;
			esac
		done <<-EOF
			$OUTPUT
		EOF
		case "$LASTMOUNT" in
		  *"n/a") ;; # nothing to do - system was not mounted yet
		  *)
			LASTDIFF=$(diff_dates "$LASTMOUNT" "$LASTCHECKED")
			if test "$LASTDIFF" -gt 0; then
				verbose "Filesystem has not been checked after the last mount, using fsck -f."
				FORCE="-f"
			fi
			;;
		esac
	esac

	case "$FSTYPE" in
	  xfs) check_xfs
		;;
	  ext[234])
		if test "$DRY" -ne 0; then
			accept_fsck "$FSCK" -f -n "$VOLUME"
			return 0
		fi
		FLAG=$(get_fsck_mode)
		accept_fsck "$FSCK" ${FORCE:+"$FORCE"} ${FLAG:+"$FLAG"} "$VOLUME"
		;;
	  reiserfs)
		if test "$DRY" -ne 0; then
			warning "Skipping ReiserFS check in dry run: even check mode can replay the journal."
			return 0
		fi
		FLAG=$(get_fsck_mode)
		accept_fsck "$FSCK" ${FORCE:+"$FORCE"} ${FLAG:+"$FLAG"} "$VOLUME"
		;;
	  crypto_LUKS)
		check_luks || error "LUKS check failed on \"$VOLUME\"."
		;;
	  *)
		error "Filesystem \"$FSTYPE\" on device \"$VOLUME\" is not supported by this tool." ;;
	esac
}

# TRUSTED_PATH canonical copy (this file). Scripts stay standalone; when
# changing these rules, update validate_path() in:
#   scripts/lvmdump.sh.in scripts/blkdeactivate.sh.in
#   scripts/lvm_import_vdo.sh scripts/lvresize_fs_helper.sh
# and safe_root_dir() in test/lib/inittest.sh (same invariants, bool API).
# Invariants: walk canonical path to root; each component UID 0; reject
# group/world writability unless the directory has the sticky bit set.
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
		MODE=$("$STAT" -c '%u %a' "$NODE") ||
			error "$NAME \"$NODE\" is not accessible."
		test "${MODE%% *}" = "0" ||
			error "$NAME \"$NODE\" must be owned by root."
		MODE=${MODE##* }
		test $(( 0$MODE & 022 )) -eq 0 ||
			{ test -d "$NODE" && test $(( 0$MODE & 01000 )) -ne 0; } ||
			error "$NAME \"$NODE\" must not be group or world writable."
		test "$NODE" = "/" && break
		NODE=${NODE%/*}
		test -n "$NODE" || NODE=/
	done
}

# Validate an optional LVM_BINARY / DMSETUP_BINARY override.  $1 names the
# override for error messages, $2 is its value; an empty value is ignored.
# Only an absolute path to a root-owned executable is accepted and its
# canonical path is printed.  The caller stores the printed value, so a
# symlink cannot be repointed at a different binary after this check.
validate_override() {
	local OPATH VAL

	# Reject anything but a plain variable name before it reaches eval.
	case "$1" in
	  *[!A-Za-z0-9_]*|"") error "Invalid override variable name \"$1\"." ;;
	esac

	# eval keeps this dash compatible (no ${!name} or printf -v).  Only
	# the name is expanded by eval; a value is never parsed as code.
	eval "VAL=\${$1-}"

	test -z "$VAL" && return 0
	test "${VAL#/}" != "$VAL" ||
		error "$1 must be an absolute path."

	OPATH=$("$READLINK" -f "$VAL") ||
		error "$1 \"$VAL\" must be accessible and owned by root."

	validate_path "$1" "$OPATH"

	if ! test -f "$OPATH" || ! test -x "$OPATH"; then
		error "$1 \"$OPATH\" must be an executable file."
	fi

	# Store the validated canonical path, so a symlink cannot be
	# repointed at a different binary after this check.
	eval "$1=\$OPATH"
}

# Verify a binary is present and runnable.
probe_tool() {
	local NAME=$1
	local BINARY=$2

	command -v "$BINARY" >/dev/null 2>&1 ||
		error "Could not find $NAME binary \"$BINARY\"."

	"$BINARY" version >/dev/null 2>&1 ||
		error "Could not run $NAME binary \"$BINARY\"."
}

# Validate optional LVM_BINARY / DMSETUP_BINARY overrides, set DMSETUP and
# LVM command names and verify both binaries are runnable.  fsadm ships
# with lvm2 and expects both utilities to be present.
init_lvm_tools() {
	validate_override DMSETUP_BINARY
	validate_override LVM_BINARY

	DMSETUP=${DMSETUP_BINARY:-dmsetup}
	LVM=${LVM_BINARY:-lvm}

	probe_tool lvm "$LVM"
	probe_tool dmsetup "$DMSETUP"
}

init_environment() {
	"$READLINK" -e / >/dev/null 2>&1 || READLINK_E="-f"
	TEST64BIT=$(( 1000 * 1000000000000 ))
	test "$TEST64BIT" -eq 1000000000000000 ||
		error "Shell does not handle 64bit arithmetic."
	test "$("$DATE" -u -d"Jan 01 00:00:01 1970" +%s)" -eq 1 ||
		error "Date translation does not work."

	# DM_DEV_DIR can be overridden (e.g. by the test suite); make sure a
	# non-root user cannot replace it.  The mapper directory is validated
	# (sticky world-writable parents like /tmp are allowed) because a
	# root-owned control node alone does not make the path trustworthy.
	if test ! -c "$DM_DEV_DIR/mapper/control" ||
	   test "$("$STAT" -c '%u' "$DM_DEV_DIR/mapper/control")" != "0"; then
		DM_DEV_DIR="/dev"
	else
		DM_DEV_DIR=$("$READLINK" -f "$DM_DEV_DIR") ||
			error "Cannot resolve DM_DEV_DIR \"$DM_DEV_DIR\"."
		validate_path DM_DEV_DIR "$DM_DEV_DIR/mapper"
	fi

	init_lvm_tools

	test "$EXTOFF" -eq 1 2>/dev/null || EXTOFF=0
}

#############################
# start point of this script
# - parsing parameters
#############################
trap 'cleanup $?' EXIT
trap 'cleanup 2' HUP INT QUIT ABRT TERM

# test some prerequisites
for i in "$TUNE_EXT" "$RESIZE_EXT" "$TUNE_REISER" "$RESIZE_REISER" \
	"$TUNE_XFS" "$XFS_DB" "$RESIZE_XFS" "$MOUNT" "$UMOUNT" "$MKDIR" \
	"$RMDIR" "$BLOCKDEV" "$BLKID" "$AWK" "$READLINK" "$STAT" \
	"$DATE" "$FSCK" "$XFS_CHECK" "$XFS_REPAIR"; do
	test -n "$i" || error "Required command definitions in the script are missing!"
done

test "$#" -eq 0 && tool_usage

while [ "$#" -ne 0 ]; do
	# Normalize: strip all '-' after leading '--' so e.g. --dry-run matches --dryrun
	case "$1" in
	  --*) ARG="--$(printf '%s' "${1#--}" | tr -d '-')" ;;
	  *) ARG=$1 ;;
	esac
	case "$ARG" in
	  "") ;;
	  -h|--help)		tool_usage ;;
	  -c|--cryptresize)	DO_CRYPTRESIZE=1 ;;
	  -e|--extoffline)	EXTOFF=1 ;;
	  -f|--force)		FORCE="-f" ;;
	  -l|--lvresize)	DO_LVRESIZE=1 ;;
	  -n|--dryrun)		DRY=1 ; YES="-y" ;;
	  -v|--verbose)		VERB="-v" ;;
	  -y|--yes)		YES="-y" ;;
	  check|resize)	test -n "$ACTION" &&
				error "Conflicting actions \"$ACTION\" and \"$1\"."
			ACTION=$1 ;;
	  -*) error "Unknown option \"$1\". (see: $TOOL --help)" ;;
	  *)
		# positional argument: <device>, then <size> for resize
		if test -z "$ACTION"; then
			error "Missing <action>. (see: $TOOL --help)"
		fi
		if test -z "$DEVICE"; then
			DEVICE=$1
		elif test "$ACTION" = resize && test -z "$NEWSIZE"; then
			NEWSIZE=$1
		else
			error "Too many arguments. (see: $TOOL --help)"
		fi
		;;
	esac
	shift
done

case "$ACTION" in
  check|resize)	test -n "$DEVICE" || error "Missing <device>. (see: $TOOL --help)" ;;
esac

# lvresize --fs resize_fsadm re-execs fsadm with FSADM_RUNNING set; skip that
# nested invocation (parent already did the filesystem work).
if test "${FSADM_RUNNING-}" = "$TOOL"; then
	verbose "Skipping, already running (FSADM_RUNNING set)."
	exit 0
fi

init_environment

case "$ACTION" in
  check)  check "$DEVICE"
	  ;;
  resize) export FSADM_RUNNING=$TOOL # see guard above for nested lvresize call
	  resize "$DEVICE" "$NEWSIZE"
	  if test "$DO_LVRESIZE" -eq 1; then DO_LVRESIZE=2; fi
	  ;;
  *)	  error "Missing command. (see: $TOOL --help)"
esac

exit 0
