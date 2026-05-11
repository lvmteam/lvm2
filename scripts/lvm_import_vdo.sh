#!/bin/bash
#
# Copyright (C) 2021-2026 Red Hat, Inc. All rights reserved.
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
# Script for importing VDO volumes to lvm2 managed VDO LVs
#
# Needed utilities:
#  lvm, dmsetup,
#  vdo,
#  grep, awk, sed, blockdev, readlink, stat, truncate
#
# Conversion is using  'vdo convert' support from VDO manager to move
# existing VDO header by 2M which makes space to place in PV header
# and VG metadata area, and then create VDOPOOL LV and VDO LV in such VG.
#

set -euE -o pipefail

TOOL=lvm_import_vdo
IMPORT_NAME="VDO_${TOOL}_${RANDOM}$$"
TEMPDIR=

PATH="/sbin:/usr/sbin:/bin:/usr/bin"

BLOCKDEV="blockdev"
LOSETUP="losetup"
READLINK="readlink"
READLINK_E="-e"
STAT="stat"
TRUNCATE="truncate"

DM_DEV_DIR="${DM_DEV_DIR:-/dev}"
DM_UUID_PREFIX="${DM_UUID_PREFIX-}"
DM_VG_NAME=
DM_LV_NAME=
DEFAULT_VDO_CONFIG="/etc/vdoconf.yml" # Default location of vdo's manager config file
VDO_CONFIG=${VDO_CONFIG-}   # can be overridden with --vdo-config
VDO_CONFIG_RESTORE=

DEVICE=
VGNAME=
LVNAME=
DEVMAJOR=0
DEVMINOR=0
PROMPTING=
USE_VDO_DM_SNAPSHOT="--yes"
VDO_DM_SNAPSHOT_NAME=
VDO_DM_SNAPSHOT_DEVICE=
VDO_SNAPSHOT_LOOP=
VDO_INCONSISTENT=

DRY=0
VERB=
FORCE=
YES=
ABORT_AFTER_VDO_CONVERT=0
VDO_ALLOCATION_PARAMS=

# default name for converted VG and its VDO LV
DEFAULT_NAME="vdovg/vdolvol"
NAME=

# predefine empty
vdo_ackThreads=
vdo_bioRotationInterval=
vdo_bioThreads=
vdo_blockMapCacheSize=
vdo_blockMapPeriod=
vdo_compression=
vdo_cpuThreads=
vdo_deduplication=
vdo_hashZoneThreads=
vdo_indexMemory=
vdo_indexSparse=
vdo_logicalBlockSize=
vdo_logicalSize=
vdo_logicalThreads=
vdo_maxDiscardSize=
vdo_physicalSize=
vdo_physicalThreads=
vdo_slabSize=
vdo_writePolicy=

# help message
tool_usage() {
	cat <<-EOF
	  ${TOOL}: Utility to convert VDO volume to VDO LV.

	  ${TOOL} [options] <vdo_device_path>

	  Options:
	      -f | --force	  Bypass sanity checks
	      -h | --help	  Show this help message
	      -n | --name	  Specifies VG/LV name for converted VDO volume
	      -v | --verbose	  Be verbose
	      -y | --yes	  Answer "yes" at any prompts
		   --dry-run	  Dry run: read devices and config, skip storage changes
		   --no-snapshot  Do not use snapshot for converted VDO device
		   --uuid-prefix  Prefix for DM snapshot uuid
		   --vdo-config   Configuration file for VDO manager

	EOF

	exit
}

verbose() {
	test -z "$VERB" || printf '%s\n' "$TOOL: $*" >&2
}

# Support multi-line error messages
error() {
	local i

	for i in "$@"; do
		printf '%s\n' "$TOOL: $i" >&2
	done

	exit 1
}

warn() {
	local i
	for i in "$@"; do
		printf '%s\n' "$TOOL: WARNING: $i" >&2
	done
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

cleanup() {
	local i

	trap '' EXIT HUP INT QUIT ABRT TERM # mute trap for all signals to not interrupt cleanup() on any next signal

	[ -z "$PROMPTING" ] || echo "No"

	# Once merging starts, the snapshot may contain the only recoverable data.
	if [ -n "$VDO_INCONSISTENT" ] && [ -n "$VDO_DM_SNAPSHOT_NAME" ]; then
		warn "Snapshot merge did not complete; manual recovery is required." \
		     "Preserving snapshot $VDO_DM_SNAPSHOT_NAME and loop device $VDO_SNAPSHOT_LOOP." \
		     "Preserving backing file $TEMPDIR/${IMPORT_NAME}_snap and configuration files in $TEMPDIR."
		exit "${1:-1}"
	fi

	# Dry runs create temporary files, but no storage resources to unwind.
	if [ "$DRY" -eq 0 ]; then
		if [ -e "$VDO_CONFIG_RESTORE" ]; then
			dry cp -a "$VDO_CONFIG_RESTORE" "${VDO_CONFIG:-"$DEFAULT_VDO_CONFIG"}" || true
		fi

		if [ -n "$VDO_DM_SNAPSHOT_NAME" ]; then
			dry "$LVM" vgchange -an --devices "$VDO_DM_SNAPSHOT_DEVICE" "$VGNAME" >/dev/null 2>&1 || true
			i=0
			while [ "$i" -lt 20 ]; do
				[ "$("$DMSETUP" info --noheadings -co open "$VDO_DM_SNAPSHOT_NAME")" = "0" ] && break
				sleep .1
				i=$(( i + 1 ))
			done
			dry "$DMSETUP" remove "$VDO_DM_SNAPSHOT_NAME" >/dev/null 2>&1 || true
		fi

		if [ -n "$VDO_SNAPSHOT_LOOP" ]; then
			dry "$LOSETUP" -d "$VDO_SNAPSHOT_LOOP" || true
		fi
	fi

	[ -z "$VDO_INCONSISTENT" ] || echo "$TOOL: VDO volume import process exited unexpectedly!" >&2

	if [ -n "$TEMPDIR" ]; then
		rm -f "$TEMPDIR/vdoconf.yml" "$TEMPDIR/vdo_snap.yml" "$TEMPDIR/vdo_selected.yml" \
			"$TEMPDIR/${IMPORT_NAME}_snap" 2>/dev/null || true
		rmdir "$TEMPDIR" 2>/dev/null || true
	fi

	exit "${1:-1}"
}

# Create snapshot target like for persistent snapshot with 16KiB chunksize
snapshot_target_line_() {
	echo "0 $("$BLOCKDEV" --getsize "$1") snapshot${3:-} $1 $2 P 32"
}

snapshot_create_() {
	VDO_DM_SNAPSHOT_NAME="${IMPORT_NAME}_snap"
	local file="$TEMPDIR/$VDO_DM_SNAPSHOT_NAME"

	# TODO: maybe use ramdisk via 'brd' device ?)
	dry "$TRUNCATE" -s 20M "$file"
	VDO_SNAPSHOT_LOOP=$(dry "$LOSETUP" -f --show "$file")
	# A symbolic path for subsequent commands; no loop device was allocated.
	if [ "$DRY" -ne 0 ]; then
		VDO_SNAPSHOT_LOOP="/dev/loop-dry-run"
	fi
	dry "$DMSETUP" create "$VDO_DM_SNAPSHOT_NAME" -u "${DM_UUID_PREFIX}${VDO_DM_SNAPSHOT_NAME}-priv" --table "$(snapshot_target_line_ "$1" "$VDO_SNAPSHOT_LOOP")"
	VDO_DM_SNAPSHOT_DEVICE="$DM_DEV_DIR/mapper/$VDO_DM_SNAPSHOT_NAME"
	verbose "Using VDO snapshot device $VDO_DM_SNAPSHOT_DEVICE for $1."
}

snapshot_merge_() {
	local origin=$1
	local INITIAL_STATUS=
	local STATUS
	local i=0

	if [ "$DRY" -eq 0 ]; then
		INITIAL_STATUS=$("$DMSETUP" status "$VDO_DM_SNAPSHOT_NAME")
	fi
	dry "$DMSETUP" reload "$VDO_DM_SNAPSHOT_NAME" --table "$(snapshot_target_line_ "$origin" "$VDO_SNAPSHOT_LOOP" -merge)"
	dry "$DMSETUP" suspend "$VDO_DM_SNAPSHOT_NAME" ||
		error "ABORTING: Failed to initialize snapshot merge! Origin volume is unchanged."

	verbose "Merging converted VDO volume \"$VDO_DM_SNAPSHOT_NAME\"."
	if [ "$DRY" -eq 0 ]; then
		VDO_INCONSISTENT=1
	fi

	# Running merging
	dry "$DMSETUP" resume "$VDO_DM_SNAPSHOT_NAME"

	# Only a real snapshot needs polling; merging should be nearly instantaneous.
	while [ "$DRY" -eq 0 ]; do
		STATUS=$("$DMSETUP" status "$VDO_DM_SNAPSHOT_NAME")
		# shellcheck disable=SC2086 # intentional word splitting
		set -- $STATUS
		[[ "${4:-}" =~ ^[0-9]+/[0-9]+$ && "${5:-}" =~ ^[0-9]+$ ]] ||
			error "Invalid snapshot merge status: $STATUS." \
			      "ABORTING: Snapshot failed to merge! (Administrator required...)"
		[ "${4%/*}" = "$5" ] && break
		i=$(( i + 1 ))
		if [ "$i" -gt 20 ]; then
			error "Initial snapshot status $INITIAL_STATUS." \
			      "Failing merge snapshot status $STATUS." \
			      "ABORTING: Snapshot failed to merge! (Administrator required...)"
		fi
		sleep .2
	done

	VDO_INCONSISTENT=
	VDO_CONFIG_RESTORE=

	[ "$DRY" -ne 0 ] || verbose "Converted VDO volume is merged to \"$origin\"."

	dry "$DMSETUP" remove "$VDO_DM_SNAPSHOT_NAME" || {
		sleep 1 # sleep and retry once more
		dry "$DMSETUP" remove "$VDO_DM_SNAPSHOT_NAME" ||
			error "ABORTING: Cannot remove snapshot $VDO_DM_SNAPSHOT_NAME! (check volume autoactivation...)"
	}

	VDO_DM_SNAPSHOT_NAME=
	dry "$LOSETUP" -d "$VDO_SNAPSHOT_LOOP"
	VDO_SNAPSHOT_LOOP=
}

get_enabled_value_() {
	case "$1" in
	enabled) echo "1" ;;
	*) echo "0" ;;
	esac
}

# Print a normalized unsigned decimal that fits signed 64-bit arithmetic.
parse_number() {
	local NUM=$1

	case "$NUM" in
	  *[!0-9]*|"") echo "$TOOL: Expected decimal digits, got \"$1\"." >&2; return 1 ;;
	esac
	NUM=${NUM#"${NUM%%[!0]*}"}
	NUM=${NUM:-0}
	# Compare at most 18 digits: some shells wrap overflowing test operands.
	if test "${#NUM}" -gt 19 ||
	   { test "${#NUM}" -eq 19 && test "$NUM" != "${NUM#9}" &&
	     test "${NUM#?}" -gt 223372036854775807; }; then
		echo "$TOOL: Number \"$1\" exceeds signed 64-bit range." >&2
		return 1
	fi
	printf '%s\n' "$NUM"
}

get_kb_size_with_unit_() {
	local NUM
	local SCALE

	NUM=$(parse_number "${1%[kKmMgGtTpP]}") || error "Invalid size value \"$1\"."
	case "$1" in
	  *[kK]) SCALE=1 ;;
	  *[mM]) SCALE=$(( 1 << 10 )) ;;
	  *[gG]) SCALE=$(( 1 << 20 )) ;;
	  *[tT]) SCALE=$(( 1 << 30 )) ;;
	  *[pP]) SCALE=$(( 1 << 40 )) ;;
	  *) error "Unknown size unit in \"$1\"." ;;
	esac
	test "$NUM" -le "$(( 9223372036854775807 / SCALE ))" || error "Size value overflow."
	printf '%s\n' "$(( NUM * SCALE ))"
}

# Validate YAML values before constructing LVM configuration or changing devices.
# Normalize numbers, sizes to KiB, booleans to 0/1 and index memory to MiB.
validate_vdo_config_() {
	local FIELD VALUE

	for FIELD in logicalBlockSize blockMapPeriod ackThreads bioThreads \
		bioRotationInterval cpuThreads hashZoneThreads logicalThreads physicalThreads; do
		FIELD=vdo_$FIELD
		VALUE=$(parse_number "${!FIELD}") || error "Invalid VDO ${FIELD#vdo_}."
		printf -v "$FIELD" '%s' "$VALUE"
	done

	for FIELD in logicalSize physicalSize blockMapCacheSize slabSize maxDiscardSize; do
		FIELD=vdo_$FIELD
		VALUE=$(get_kb_size_with_unit_ "${!FIELD}") || error "Invalid VDO ${FIELD#vdo_}."
		printf -v "$FIELD" '%s' "$VALUE"
	done

	for FIELD in compression deduplication indexSparse; do
		FIELD=vdo_$FIELD
		case "${!FIELD}" in
		  enabled|disabled) ;;
		  *) error "Invalid VDO ${FIELD#vdo_}: expected enabled or disabled." ;;
		esac
		printf -v "$FIELD" '%s' "$(get_enabled_value_ "${!FIELD}")"
	done

	case "$vdo_writePolicy" in
	  [aA][uU][tT][oO]|[sS][yY][nN][cC]|[aA][sS][yY][nN][cC]|[aA][sS][yY][nN][cC]-[uU][nN][sS][aA][fF][eE]) ;;
	  *) error "Invalid VDO writePolicy: expected auto, sync, async or async-unsafe." ;;
	esac

	# The manager expresses index memory in GiB, including fractions like 0.25.
	# Reject numeric prefixes with trailing garbage and avoid awk's rounding or
	# scientific notation when producing an integer LVM configuration value.
	[[ "$vdo_indexMemory" =~ ^([0-9]+([.][0-9]*)?|[.][0-9]+)$ ]] ||
		error "Invalid VDO indexMemory: expected a decimal GiB value."
	vdo_indexMemory=$(LC_ALL=C awk -v val="$vdo_indexMemory" 'BEGIN {
		mb = val * 1024
		if (mb < 256 || mb > 1048576 || mb != int(mb)) exit 1
		printf "%.0f\n", mb
	}') || error "Invalid VDO indexMemory: expected whole MiB in the range 256..1048576."
}

# Figure out largest possible extent size usable for VG
# $1   physical size
# $2   logical size
get_largest_extent_size_() {
	local max=4
	local i
	local d

	for i in 8 16 32 64 128 256 512 1024 2048 4096; do
		d=$(( $1 / i ))
		[ $(( d * i )) -eq "$1" ] || break
		d=$(( $2 / i ))
		[ $(( d * i )) -eq "$2" ] || break
		max=$i
	done
	echo "$max"
}

# detect LV on the given device
# dereference device name if it is a symbolic link
detect_lv_() {
	local DEVICE=$1
	local SYSVOLUME
	local MAJORMINOR

	local DEV

	DEVICE=${1/#"${DM_DEV_DIR}/"/}
	DEVICE=$("$READLINK" "$READLINK_E" "$DM_DEV_DIR/$DEVICE" || true)
	[ -n "$DEVICE" ] || error "Readlink cannot access device \"$1\"."
	RDEVICE=$DEVICE
	case "$RDEVICE" in
	  # hardcoded /dev  since udev does not create these entries elsewhere
	  /dev/dm-[0-9]*)
		read -r SYSVOLUME <"/sys/block/${RDEVICE#/dev/}/dm/name" 2>/dev/null &&
			DEVICE="$DM_DEV_DIR/mapper/$SYSVOLUME"
		read -r MAJORMINOR <"/sys/block/${RDEVICE#/dev/}/dev" 2>/dev/null ||
			error "Cannot get major:minor for \"$DEVICE\"."
		DEVMAJOR=${MAJORMINOR%%:*}
		DEVMINOR=${MAJORMINOR##*:}
		;;
	  *)
		MAJORMINOR=$("$STAT" --format '0x%t:0x%T' "$RDEVICE") ||
			error "Cannot get major:minor for \"$DEVICE\"."
		DEVMAJOR=$(( ${MAJORMINOR%%:*} ))
		DEVMINOR=$(( ${MAJORMINOR#*:} ))
		;;
	esac

	[ "$DEVMAJOR" != "$(awk '/device-mapper/ {print $1}' /proc/devices)" ] && return

	DEV="$("$DMSETUP" info -c -j "$DEVMAJOR" -m "$DEVMINOR" -o uuid,name --noheadings --nameprefixes --separator ' ')"
	case "$DEV" in
	Device*)  ;; # no devices
	*)	eval "$DEV" ;;
	esac
}

# parse yaml config files into 'prefix_yaml_part_names=("value")' strings
parse_yaml_() {
	local yaml_file=$1
	local prefix=$2
	local s
	local w
	local fs

	s='[[:space:]]*'
	w='[a-zA-Z0-9_.-]*'
	fs=$(printf '\034')

	(
	    # shellcheck disable=SC2016 # literal backtick replacement in sed
	    # Escape backslashes first, so a backslash already present in the
	    # input cannot combine with a backslash inserted below to cancel
	    # the $/`/" escaping once this is eval'd.
	    sed -ne '/^--/s|--||g; s/\\/\\\\/g; s|\"|\\\"|g; s/[[:space:]]*$//g;' \
		-e 's/\$/\\\$/g' -e 's/`/\\`/g' \
		-e "/#.*[\"\']/!s| #.*||g; /^#/s|#.*||g;" \
		-e "s|^\($s\)\($w\)$s:$s\"\(.*\)\"$s\$|\1$fs\2$fs\3|p" \
		-e "s|^\($s\)\($w\)${s}[:-]$s\(.*\)$s\$|\1$fs\2$fs\3|p" |

	    awk -F"$fs" '{
		indent = length($1)/2;
		if (length($2) == 0) { conj[indent]="+";} else {conj[indent]="";}
		vname[indent] = $2;
		for (i in vname) {if (i > indent) {delete vname[i]}}
		    if (length($3) > 0) {
			vn=""; for (i=0; i<indent; i++) {vn=(vn)(vname[i])("_")}
			printf("%s%s%s%s=\"%s\"\n", "'"$prefix"'",vn, $2, conj[indent-1], $3);
		    }
		}' |

	    sed -e 's/_=/+=/g' |

	    awk 'BEGIN {
		    FS="=";
		    OFS="="
		}
		/(-|\.).*=/ {
		    gsub("-|\\.", "_", $1)
		}
		{ print }'
	) < "$yaml_file"
}

#
# Convert VDO volume on LV to VDOPool within this VG
#
# This conversion requires the size of VDO virtual volume has to be expressed in the VG's extent size.
# Currently this enforces a user to reduce the VG extent size to the smaller size (up to 4KiB).
#
# TODO: We may eventually relax this condition just like we are doing rounding for convert_non_lv_()
#       Let's see if there would be any single user requiring this feature.
#       It may allow to better use larger VDO volume size (in TiB ranges).
#
convert_lv_() {
	local vdo_logicalSize=$1
	local vg_extent_size
	local extent_size
	local pvfree

	pvfree=$("$LVM" lvs -o size --units b --nosuffix --noheadings "$DM_VG_NAME/$DM_LV_NAME")
	pvfree=$(( pvfree / 1024 ))		# to KiB
	# select largest possible extent size that can exactly express both sizes
	extent_size=$(get_largest_extent_size_ "$pvfree" "$vdo_logicalSize")

	# validate existing  VG extent_size can express virtual VDO size
	vg_extent_size=$("$LVM" vgs -o vg_extent_size --units b --nosuffix --noheadings "$VGNAME")
	vg_extent_size=$(( vg_extent_size / 1024 ))

	[ "$vg_extent_size" -le "$extent_size" ] || {
		error "Please vgchange extent_size to at most $extent_size KiB or extend and align virtual size of VDO device on $vg_extent_size KiB before retrying conversion."
	}

	verbose "Renaming existing LV to be used as _vdata volume for VDO pool LV."
	dry "$LVM" lvrename ${YES:+"$YES"} ${VERB:+"$VERB"} "$VGNAME/$DM_LV_NAME" "$VGNAME/${LVNAME}_vpool" || {
		error "Rename of LV \"$VGNAME/$DM_LV_NAME\" failed, while VDO header has been already moved!"
	}

	verbose "Converting to VDO pool."
	dry "$LVM" lvconvert ${YES:+"$YES"} ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} --config "$VDO_ALLOCATION_PARAMS" -Zn -V "${vdo_logicalSize}k" -n "$LVNAME" --type vdo-pool "$VGNAME/${LVNAME}_vpool"

	verbose "Removing now unused VDO entry from VDO configuration."
	dry "$VDO" remove ${VDO_CONFIG:+-f} ${VDO_CONFIG:+"$VDO_CONFIG"} ${VERB:+"$VERB"} --force --name "$VDONAME"
}

#
# Convert VDO volume on a device to VG with VDOPool LV
#
# Convert device with the use of snapshot on top of original VDO volume (can be optionally disabled)
# Once the whole conversion is finished, snapshot is merged (During the short period time of merging
# user must ensure there will be no power-off!)
#
# For best use the latest version of  vdoprepareforlvm tool is required.
convert_non_lv_() {
	local vdo_logicalSize=$1
	local vdo_logicalSizeRounded
	local extent_size
	local output
	local pvfree

	# Check the original device before creating a snapshot (also in dry runs).
	"$LVM" pvs --devices "$DEVICE" "$DEVICE" 2>/dev/null && {
		error "Cannot convert volume \"$DEVICE\" with existing PV header."
	}

	if [ -n "$USE_VDO_DM_SNAPSHOT" ]; then
		snapshot_create_ "$DEVICE"
		awk -v old="$DEVICE" -v new="$VDO_DM_SNAPSHOT_DEVICE" \
			'{
				if ($1 == "device:" && $2 == old) {
					i = index($0, old)
					$0 = substr($0, 1, i - 1) new substr($0, i + length(old))
				}
				print
			}' "$TEMPDIR/vdoconf.yml" > "$TEMPDIR/vdo_snap.yml"
		# In case of error in the middle of conversion restore original config file
		VDO_CONFIG_RESTORE="$TEMPDIR/vdoconf.yml"
		# Let VDO manager operate on snapshot volume
		dry cp -a "$TEMPDIR/vdo_snap.yml" "${VDO_CONFIG:-"$DEFAULT_VDO_CONFIG"}"
	else
		# If error in the following section, report possible problems ahead
		if [ "$DRY" -eq 0 ]; then
			VDO_INCONSISTENT=1
		fi
	fi

	# In case we operate with snapshot, all lvm2 operation will also run on top of snapshot
	local device=${VDO_DM_SNAPSHOT_DEVICE:-$DEVICE}

	verbose "Moving VDO header on \"$device\"."

	output=$(dry "$VDO" convert ${VDO_CONFIG:+-f} ${VDO_CONFIG:+"$VDO_CONFIG"} ${VERB:+"$VERB"} --force --name "$VDONAME" 2>&1) || {
		local rc=$?
		echo "$output"
		error "Failed to convert VDO volume \"$DEVICE\" (exit code $rc)."
	}

	if [ "$DRY" -ne 0 ]; then
		# The captured preview is diagnostic output, not conversion metadata.
		printf '%s\n' "$output" >&2
		output=
	else
		printf '%s\n' "$output"
	fi

	if [ "$ABORT_AFTER_VDO_CONVERT" != "0" ]; then
		if [ "$DRY" -eq 0 ]; then
			warn "Aborting VDO conversion after moving VDO header, volume is useless!"
		else
			verbose "Dry run: stopping after VDO conversion preview."
		fi
		return 0
	fi

	# Parse result from VDO preparation/conversion tool
	# New version of the tool provides output with alignment and offset
	# shellcheck disable=SC2034 # parsed for possible future use
	#local vdo_length=0
	#local vdo_non_converted=0
	local vdo_aligned=0
	local vdo_offset=0
	local line
	if [ "$DRY" -ne 0 ]; then
		# No conversion output to parse; use assumed values for the preview.
		vdo_aligned=2048
		vdo_offset=1048576
		warn "Dry run: assuming 2 KiB alignment and 1 MiB offset."
	fi
	while IFS=  read -r line; do
		# trim leading spaces
		case "${line#"${line%%[! ]*}"}" in
		#"Non converted"*) vdo_non_converted=1 ;;
		#"Length"*) vdo_length=${line##* = } ;;
		"Conversion completed"*)
			   vdo_aligned=${line##*aligned on }
			   vdo_aligned=${vdo_aligned%%[!0-9]*}
			   vdo_offset=${line##*offset }
			   # backward compatibility with report from older version
			   vdo_offset=${vdo_offset##*by }
			   vdo_offset=${vdo_offset%%[!0-9]*}
			   ;;
		esac
	done <<-EOF
		$output
	EOF

	# Validate before writing PV metadata. Keep zero as the legacy fallback
	# for conversion tools which do not report alignment and offset.
	vdo_aligned=$(parse_number "$vdo_aligned") || error "Invalid VDO conversion alignment."
	vdo_offset=$(parse_number "$vdo_offset") || error "Invalid VDO conversion offset."
	if [ "$vdo_aligned" -ne 0 ]; then
		[ "$vdo_aligned" -ge 1024 ] && [ $(( vdo_aligned & (vdo_aligned - 1) )) -eq 0 ] ||
			error "Invalid VDO conversion alignment $vdo_aligned bytes."
	fi

	# Obtain free space in this new PV
	# after 'vdo convert' call there is ~(1-2)M free space at the front of the device
	pvfree=$("$BLOCKDEV" --getsize64 "$DEVICE")
	pvfree=$(parse_number "$pvfree") || error "Invalid device size for \"$DEVICE\"."
	[ "$vdo_offset" -lt "$pvfree" ] && [ $(( vdo_offset % 512 )) -eq 0 ] ||
		error "Invalid VDO conversion offset $vdo_offset bytes for device size $pvfree."
	pvfree=$(( ( pvfree - vdo_offset ) / 1024 ))	# to KiB
	if [ "${vdo_aligned:-0}" -ne 0 ]; then
		extent_size=$(( vdo_aligned / 1024 ))
	else
		extent_size=$(get_largest_extent_size_ "$pvfree" "$vdo_logicalSize")
	fi

	# Round virtual size to the LOWER size expressed in extent units.
	# lvm is parsing VDO metadata and can read real full size and use it instead of this smaller value.
	# To precisely byte-synchronize the size of VDO LV, user can lvresize such VDO LV later.
	vdo_logicalSizeRounded=$(( ( vdo_logicalSize / extent_size ) * extent_size ))
	[ "$vdo_logicalSizeRounded" -gt 0 ] && [ "$pvfree" -ge "$extent_size" ] ||
		error "VDO conversion sizes are too small for extent size $extent_size KiB."

	dry "$LVM" pvcreate ${YES:+"$YES"} ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} --devices "$device" --dataalignment "$vdo_offset"b "$device"

	verbose "Creating volume group \"$VGNAME\" with the extent size $extent_size KiB."
	dry "$LVM" vgcreate ${YES:+"$YES"} ${VERB:+"$VERB"} --devices "$device" -s "${extent_size}k" "$VGNAME" "$device"

	verbose "Creating VDO pool data LV from all extents in the volume group \"$VGNAME\"."
	dry "$LVM" lvcreate -Zn -Wn -an ${YES:+"$YES"} ${VERB:+"$VERB"} --devices "$device" -l100%VG -n "${LVNAME}_vpool" "$VGNAME" "$device"

	verbose "Converting to VDO pool."
	dry "$LVM" lvconvert ${USE_VDO_DM_SNAPSHOT:-"$YES"} ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} --devices "$device" --config "$VDO_ALLOCATION_PARAMS" -Zn -V "${vdo_logicalSizeRounded}k" -n "$LVNAME" --type vdo-pool "$VGNAME/${LVNAME}_vpool"

	if [ "$vdo_logicalSizeRounded" -lt "$vdo_logicalSize" ]; then
		# need to extend virtual size to be covering all the converted area
		# let lvm2 to round to the proper virtual size of VDO LV
		dry "$LVM" lvextend ${YES:+"$YES"} ${VERB:+"$VERB"} --devices "$device" -L "$vdo_logicalSize"k "$VGNAME/$LVNAME"
	fi

	VDO_INCONSISTENT=

	[ -z "$USE_VDO_DM_SNAPSHOT" ] && return # no-snapshot case finished

	dry "$LVM" vgchange -an ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} --devices "$device" "$VGNAME"

	# Prevent unwanted auto activation when VG is merged
	dry "$LVM" vgchange --setautoactivation n ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} --devices "$device" "$VGNAME"

	if [ -z "$YES" ]; then
		PROMPTING=yes
		warn "Do not interrupt merging process once it starts (VDO data may become irrecoverable)!"
		echo -n "$TOOL: Do you want to merge converted VDO device \"$DEVICE\" to VDO LV \"$VGNAME/$LVNAME\"? [y|N]: "
		read -r -n 1 -s ANSWER
		case "${ANSWER:0:1}" in
		  y|Y )  echo "Yes" ;;
		    * )  echo "No" ; PROMPTING=""; return 1 ;;
		esac
		PROMPTING=""
		YES="-y" # From now, no prompting
	fi

	snapshot_merge_ "$DEVICE"

	# For systems using devicesfile add 'merged' PV into system.devices.
	# Bypassing use of --valuesonly to keep compatibility with older lvm.
	local usedev
	usedev=$("$LVM" lvmconfig --typeconfig full devices/use_devicesfile || true)
	[ "${usedev#*=}" = "1" ] && dry "$LVM" lvmdevices --adddev "$DEVICE"

	# Restore auto activation for a VG
	dry "$LVM" vgchange --setautoactivation y ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} "$VGNAME"

	dry "$LVM" lvchange -ay ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} "$VGNAME/$LVNAME"
}

# Convert existing VDO volume into lvm2 volume
convert2lvm_() {
	local VDONAME
	local FOUND=
	local MAJOR=0
	local MINOR=0

	local LASTVGNAME
	local DM_OPEN
	local ANSWER

	VGNAME=${NAME%/*}
	LVNAME=${NAME#*/}
	DM_UUID=
	detect_lv_ "$DEVICE"
	case "$DM_UUID" in
		LVM-*)	eval "$("$DMSETUP" splitname --nameprefixes --noheadings --separator ' ' "$DM_NAME")"
			if [ -z "$VGNAME" ] || [ "$VGNAME" = "$LVNAME" ]; then
				VGNAME=$DM_VG_NAME
				verbose "Using existing volume group name \"$VGNAME\"."
				[ -n "$LVNAME" ] || LVNAME=$DM_LV_NAME
			elif [ "$VGNAME" != "$DM_VG_NAME" ]; then
				error "Volume group name \"$VGNAME\" does not match name \"$DM_VG_NAME\" for VDO device \"$DEVICE\"."
			fi
			;;
		*)
			# Check if we need to generate unused $VGNAME
			if [ -z "$VGNAME" ] || [ "$VGNAME" = "$LVNAME" ]; then
				VGNAME=${DEFAULT_NAME%/*}
				# Find largest numbered variant of our 'default' vgname.
				# Match the literal name with awk; a regex would let
				# metacharacters in VGNAME (e.g. '.') match other names.
				LASTVGNAME=$(LC_ALL=C "$LVM" vgs -oname --noheadings |
					awk -v base="$VGNAME" '
						index($1, base) == 1 {
							suffix = substr($1, length(base) + 1)
							if (suffix ~ /^[0-9]+$/) print suffix
						}' | sort -n | tail -1 || true)
				# If only the bare name exists (no numbered variants),
				# start numbering from 0 so a "1" suffix is generated.
				if [ -z "$LASTVGNAME" ] && "$LVM" vgs "$VGNAME" >/dev/null 2>&1; then
					LASTVGNAME=0
				fi
				if [ -n "$LASTVGNAME" ]; then
					# If the number is becoming too high, try some random number
					LASTVGNAME=$(parse_number "$LASTVGNAME") || error "Invalid numeric suffix for volume group \"$VGNAME\"."
					if [ "$LASTVGNAME" -gt 99999999 ]; then
						LASTVGNAME=$RANDOM
					fi
					# Generate new unused VG name
					VGNAME="${VGNAME}$(( LASTVGNAME + 1 ))"
					verbose "Selected unused volume group name \"$VGNAME\"."
				fi
			fi
			# New VG is created, LV name should be always unused.
			[ -n "$LVNAME" ] || LVNAME=${DEFAULT_NAME#*/}
			"$LVM" vgs "$VGNAME" >/dev/null 2>&1 && error "Cannot use already existing volume group name \"$VGNAME\"."
			;;
	esac

	verbose "Checked whether device \"$DEVICE\" is already a logical volume."

	if test -n "${TMPDIR-}" && test "${TMPDIR#/}" = "$TMPDIR"; then
		error "TMPDIR must be an absolute path."
	fi
	if test -n "${TMPDIR-}"; then
		TMPDIR=$("$READLINK" -f "$TMPDIR") ||
			error "Cannot resolve TMPDIR \"$TMPDIR\"."
		validate_path TMPDIR "$TMPDIR"
	fi
	TEMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/${TOOL}_XXXXXXXXXX") ||
		error "Failed to create temporary directory."

	# TODO: might use directly  /etc/vdoconf.yml (avoiding need of 'vdo' manager)
	verbose "Getting YAML VDO configuration."
	"$VDO" printConfigFile ${VDO_CONFIG:+-f} ${VDO_CONFIG:+"$VDO_CONFIG"} >"$TEMPDIR/vdoconf.yml"
	[ -s "$TEMPDIR/vdoconf.yml" ] || error "Cannot work without VDO configuration."

	# Check list of devices in VDO configuration file for their major:minor
	# and match with given $DEVICE devmajor:devminor
	local i
	while read -r i; do
		local DEV
		local MAJORMINOR
		local MAJOR
		local MINOR
		DEV=$("$READLINK" "$READLINK_E" "$i") || continue
		MAJORMINOR=$("$STAT" --format '0x%t:0x%T' "$DEV" 2>/dev/null) || continue
		MAJOR=$(( ${MAJORMINOR%%:*} ))
		MINOR=$(( ${MAJORMINOR#*:} ))
		if [ "$MAJOR" = "$DEVMAJOR" ] && [ "$MINOR" = "$DEVMINOR" ]; then
			[ -z "$FOUND" ] || error "VDO configuration contains duplicate entries $FOUND and $i."
			FOUND=$i
		fi
	done <<-EOF
		$(awk '/.*device:/ {print $2}' "$TEMPDIR/vdoconf.yml")
	EOF

	[ -n "$FOUND" ] || error "Can't find matching device in VDO configuration file."
	# $MAJOR/$MINOR are from the config scan loop, not the matched device.
	verbose "Found matching device $FOUND  $DEVMAJOR:$DEVMINOR."

	VDONAME=$(awk -v DNAME="$FOUND" '/.*VDOService$/ {VNAME=substr($1, 1, length($1) - 1)} $1 == "device:" && $2 == DNAME {print VNAME}' "$TEMPDIR/vdoconf.yml")
	[ -n "$VDONAME" ] || error "Cannot find VDO service for device $FOUND."

	# When VDO volume is 'active', check it's not mounted/being used
	DM_OPEN="$("$DMSETUP" info -c -o open  "$VDONAME" --noheadings --nameprefixes 2>/dev/null || true)"
	case "$DM_OPEN" in
	Device*) ;; # no devices
	*)	eval "$DM_OPEN"
		[ "${DM_OPEN:-0}" -eq 0 ] || error "Cannot convert in use VDO volume \"$VDONAME\"!"
		;;
	esac

	# Select the exact service before normalizing YAML keys. Names such as
	# foo-bar and foo_bar must never contribute settings to each other.
	awk -v name="$VDONAME:" '
		/[^[:space:]]/ && !/^[[:space:]]*#/ {
			indent = match($0, /[^ ]/) - 1
			if (selected && indent <= base) selected = 0
		}
		$1 == name && $2 == "!VDOService" {
			selected = 1; base = indent
			print "vdo:"
			next
		}
		selected { print substr($0, base + 1) }
	' "$TEMPDIR/vdoconf.yml" > "$TEMPDIR/vdo_selected.yml"
	eval "$(parse_yaml_ "$TEMPDIR/vdo_selected.yml" "")"

	validate_vdo_config_

	verbose "Converted VDO device has logical/physical size $vdo_logicalSize/$vdo_physicalSize KiB."

	VDO_ALLOCATION_PARAMS=$(cat <<-EOF
	allocation {
		vdo_use_compression = $vdo_compression
		vdo_use_deduplication = $vdo_deduplication
		vdo_use_metadata_hints=1
		vdo_minimum_io_size = $vdo_logicalBlockSize
		vdo_block_map_cache_size_mb = $(( vdo_blockMapCacheSize / 1024 ))
		vdo_block_map_period = $vdo_blockMapPeriod
		vdo_use_sparse_index = $vdo_indexSparse
		vdo_index_memory_size_mb = $vdo_indexMemory
		vdo_slab_size_mb = $(( vdo_slabSize / 1024 ))
		vdo_ack_threads = $vdo_ackThreads
		vdo_bio_threads = $vdo_bioThreads
		vdo_bio_rotation = $vdo_bioRotationInterval
		vdo_cpu_threads = $vdo_cpuThreads
		vdo_hash_zone_threads = $vdo_hashZoneThreads
		vdo_logical_threads = $vdo_logicalThreads
		vdo_physical_threads = $vdo_physicalThreads
		vdo_write_policy = $vdo_writePolicy
		vdo_max_discard = $(( vdo_maxDiscardSize / 4 ))
		vdo_pool_header_size = 0
	}
	EOF
	)
	verbose "VDO conversion parameters: $VDO_ALLOCATION_PARAMS"

	verbose "Stopping VDO volume."
	dry "$VDO" stop ${VDO_CONFIG:+-f} ${VDO_CONFIG:+"$VDO_CONFIG"} --name "$VDONAME" ${VERB:+"$VERB"}

	# If user has not provided '--yes', prompt before conversion
	if [ -z "$YES" ] && [ -z "$USE_VDO_DM_SNAPSHOT" ]; then
		PROMPTING=yes
		echo -n "$TOOL: Convert VDO device \"$DEVICE\" to VDO LV \"$VGNAME/$LVNAME\"? [y|N]: "
		read -r -n 1 -s ANSWER
		case "${ANSWER:0:1}" in
		  y|Y )  echo "Yes" ;;
		    * )  echo "No" ; PROMPTING=""; return 1 ;;
		esac
		PROMPTING=""
		YES="-y" # From now, no prompting
	fi

	# Make a backup of the existing VDO yaml configuration file
	[ -e "$VDO_CONFIG" ] && dry cp -a "$VDO_CONFIG" "${VDO_CONFIG}.backup"

	DEVICE=$FOUND
	case "$DM_UUID" in
		LVM-*) convert_lv_ "$vdo_logicalSize" ;;
		*)     convert_non_lv_ "$vdo_logicalSize" ;;
	esac
}

# TRUSTED_PATH: keep validate_path() in sync with scripts/fsadm.sh.
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

validate_override() {
	local OPATH VAL

	# Read the override by name without a bash-only indirection.  Only
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

	# Run the validated canonical path, not the original one, so that a
	# symlink cannot be repointed at a different binary after this check.
	eval "$1=\$OPATH"
}

#############################
# start point of this script
# - parsing parameters
#############################

[ "$#" -eq 0 ] && tool_usage

while [ "$#" -ne 0 ]
do
	# Normalize: strip all '-' after leading '--' so e.g. --dry-run matches --dryrun
	case "$1" in
	  --*) ARG="--$(printf '%s' "${1#--}" | tr -d '-')" ;;
	  *) ARG=$1 ;;
	esac
	case "$ARG" in
	  -n|--name|--uuidprefix|--vdoconfig)
		[ "$#" -ge 2 ] && [ -n "$2" ] || error "Option $1 requires a value."
		case "$2" in -*) error "Option $1 requires a value." ;; esac
		;;
	esac
	case "$ARG" in
	  "") ;;
	  -h|--help   ) tool_usage ;;
	  -n|--name   ) shift; NAME=$1 ;;
	  -f|--force  ) FORCE="-f" ;;
	  -v|--verbose) VERB="--verbose" ;;
	  -y|--yes    ) YES="-y" ;;
	  --dryrun    ) DRY=1 ; YES="-y" ;;
	  --nosnapshot) USE_VDO_DM_SNAPSHOT= ;;
	  --uuidprefix) shift; DM_UUID_PREFIX=$1 ;; # For testing only
	  --vdoconfig ) shift; VDO_CONFIG=$1 ;;
	  --abortaftervdoconvert) ABORT_AFTER_VDO_CONVERT=1; USE_VDO_DM_SNAPSHOT= ;; # For testing only
	  -* ) error "Wrong argument \"$1\". (see: $TOOL --help)" ;;
	  *) DEVICE=$1 ;;  # device name does not start with '-'
	esac
	shift
done

[ "${#IMPORT_NAME}" -lt 100 ] ||
	error "Random name \"$IMPORT_NAME\" is too long!"

[ -n "$DEVICE" ] ||
	error "Device name is not specified. (see: $TOOL --help)"

# Validate DM_DEV_DIR by checking its control device is root-owned
if test ! -c "$DM_DEV_DIR/mapper/control" ||
   test "$("$STAT" -c '%u' "$DM_DEV_DIR/mapper/control")" != "0" ; then
	DM_DEV_DIR="/dev" # fallback to /dev
fi

# user override by setting DMSETUP_BINARY,LVM_BINARY and VDO_BINARY
# overridden binaries must be absolute paths and owned by root for security
validate_override DMSETUP_BINARY
validate_override LVM_BINARY
validate_override VDO_BINARY

DMSETUP=${DMSETUP_BINARY:-dmsetup}
LVM=${LVM_BINARY:-lvm}
VDO=${VDO_BINARY:-vdo}

"$DMSETUP" version >/dev/null 2>&1 ||
	error "Could not run dmsetup binary \"$DMSETUP\"."

"$LVM" version >/dev/null 2>&1 ||
	error "Could not run lvm binary \"$LVM\"."

trap 'cleanup $?' EXIT
trap 'cleanup 2' HUP INT QUIT ABRT TERM

convert2lvm_
