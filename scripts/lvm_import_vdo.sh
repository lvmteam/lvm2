#!/bin/bash
#
# Copyright (C) 2021-2023 Red Hat, Inc. All rights reserved.
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
#  grep, awk, sed, blockdev, readlink, stat, mkdir, truncate
#
# Conversion is using  'vdo convert' support from VDO manager to move
# existing VDO header by 2M which makes space to place in PV header
# and VG metadata area, and then create VDOPOOL LV and VDO LV in such VG.
#

set -euE -o pipefail

TOOL=lvm_import_vdo
IMPORT_NAME="VDO_${TOOL}_${RANDOM}$$"
test ${#IMPORT_NAME} -lt 100 || error "Random name \"$IMPORT_NAME\" is too long!"
TEMPDIR="${TMPDIR:-/tmp}/$IMPORT_NAME"

_SAVEPATH=$PATH
PATH="/sbin:/usr/sbin:/bin:/usr/sbin:$PATH"

# Set of trapped signals
declare -a SIGNALS=("HUP" "INT" "QUIT" "ABRT" "TERM" "EXIT")

# user may override lvm location by setting LVM_BINARY
LVM=${LVM_BINARY:-lvm}
VDO=${VDO_BINARY:-vdo}
BLOCKDEV="blockdev"
LOSETUP="losetup"
READLINK="readlink"
READLINK_E="-e"
STAT="stat"
MKDIR="mkdir"
TRUNCATE="truncate"
DMSETUP="dmsetup"

DM_DEV_DIR="${DM_DEV_DIR:-/dev}"
DM_UUID_PREFIX="${DM_UUID_PREFIX:-}"
DM_VG_NAME=
DM_LV_NAME=
DEFAULT_VDO_CONFIG="/etc/vdoconf.yml" # Default location of vdo's manager config file
VDO_CONFIG=${VDO_CONFIG:-}   # can be overridden with --vdo-config
VDO_CONFIG_RESTORE=
VDOCONF=
test -n "$VDO_CONFIG" && VDOCONF="-f $VDO_CONFIG"

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
NAME=""

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
vdo_logicalThreads=
vdo_maxDiscardSize=
vdo_physicalThreads=
vdo_slabSize=
vdo_writePolicy=

# help message
tool_usage() {
	echo "${TOOL}: Utility to convert VDO volume to VDO LV."
	echo
	echo "	${TOOL} [options] <vdo_device_path>"
	echo
	echo "	Options:"
	echo "	  -f | --force	      Bypass sanity checks"
	echo "	  -h | --help	      Show this help message"
	echo "	  -n | --name	      Specifies VG/LV name for converted VDO volume"
	echo "	  -v | --verbose      Be verbose"
	echo "	  -y | --yes	      Answer \"yes\" at any prompts"
	echo "	       --dry-run      Print verbosely commands without running them"
	echo "	       --no-snapshot  Do not use snapshot for converted VDO device"
	echo "	       --uuid-prefix  Prefix for DM snapshot uuid"
	echo "	       --vdo-config   Configuration file for VDO manager"

	exit
}

verbose() {
	test -z "$VERB" || echo "$TOOL:" "$@"
}

# Support multi-line error messages
error() {
	for i in "$@" ;  do
		echo "$TOOL: $i" >&2
	done
	return 1
}

warn() {
	echo "$TOOL: WARNING: $i" >&2
}

dry() {
	if [ "$DRY" -ne 0 ]; then
		verbose "Dry execution" "$@"
		return 0
	fi
	verbose "Executing" "$@"
	"$@"
}

cleanup() {
	RC=$?	# Return code + 128  of the last command eg INT=2 + 128 -> 130

	trap '' "${SIGNALS[@]}" # mute trap for all signals to not interrupt cleanup() on any next signal

	[ -z "$PROMPTING" ] || echo "No"

	[ -e "$VDO_CONFIG_RESTORE" ] && { dry cp -a "$VDO_CONFIG_RESTORE" "${VDO_CONFIG:-"$DEFAULT_VDO_CONFIG"}" || true ; }

	if [ -n "$VDO_DM_SNAPSHOT_NAME" ]; then
		dry "$LVM" vgchange -an --devices "$VDO_DM_SNAPSHOT_DEVICE" "$VGNAME" &>/dev/null || true
		for i in {1..20} ; do
			[ "$(dry "$DMSETUP" info --noheading -co open "$VDO_DM_SNAPSHOT_NAME")" = "0" ] && break
			sleep .1
		done
		dry "$DMSETUP" remove "$VDO_DM_SNAPSHOT_NAME" &>/dev/null || true
	fi


	[ -n "$VDO_SNAPSHOT_LOOP" ] && { dry "$LOSETUP" -d "$VDO_SNAPSHOT_LOOP" || true ; }

	[ -z "$VDO_INCONSISTENT" ] || echo "$TOOL: VDO volume import process exited unexpectedly!" >&2

	rm -rf "$TEMPDIR" || true

	exit "$RC"
}

# Create snapshot target like for persistent snapshot with 16KiB chunksize
snapshot_target_line_() {
	echo "0 $("$BLOCKDEV" --getsize "$1") snapshot${3:-} $1 $2 P 32"
}

snapshot_create_() {
	VDO_DM_SNAPSHOT_NAME="${IMPORT_NAME}_snap"
	local file="$TEMPDIR/$VDO_DM_SNAPSHOT_NAME"

	# TODO: maybe use ramdisk via 'brd' device ?)
	"$TRUNCATE" -s 20M "$file"
	VDO_SNAPSHOT_LOOP=$("$LOSETUP" -f --show "$file")
	"$DMSETUP" create "$VDO_DM_SNAPSHOT_NAME" -u "${DM_UUID_PREFIX}${VDO_DM_SNAPSHOT_NAME}-priv" --table "$(snapshot_target_line_ "$1" "$VDO_SNAPSHOT_LOOP")"
	VDO_DM_SNAPSHOT_DEVICE="$DM_DEV_DIR/mapper/$VDO_DM_SNAPSHOT_NAME"
	verbose "Snapshot of VDO device $1 created: $VDO_DM_SNAPSHOT_DEVICE."
}

snapshot_merge_() {
	local status
	local initial_status

	initial_status=( $("$DMSETUP" status "$VDO_DM_SNAPSHOT_NAME") )
	"$DMSETUP" reload "$VDO_DM_SNAPSHOT_NAME" --table "$(snapshot_target_line_ "$1" "$VDO_SNAPSHOT_LOOP" -merge)"
	"$DMSETUP" suspend "$VDO_DM_SNAPSHOT_NAME" || {
		error "ABORTING: Failed to initialize snapshot merge! Origin volume is unchanged."
	}

	verbose "Merging converted VDO volume \"$VDO_DM_SNAPSHOT_NAME\"."
	VDO_INCONSISTENT=1

	# Running merging
	"$DMSETUP" resume "$VDO_DM_SNAPSHOT_NAME"

	#du -h "$TEMPDIR/$VDO_DM_SNAPSHOT_NAME"

	# Loop for a while, till the snapshot is merged.
	# Should be nearly instantaneous.
	# FIXME: Recovery when something prevents merging is hard
	for i in $(seq 1 20) ; do
		status=( $("$DMSETUP" status "$VDO_DM_SNAPSHOT_NAME") )
		# Check if merging is finished
		[ "${status[3]%/*}" = "${status[4]}" ] && break
		# Wait a bit and retry
		sleep .2
	done

	if [ "${status[3]%/*}" != "${status[4]}" ]; then
		# FIXME: Now what shall we do ??? Help....
		# Keep snapshot in DM table for possible analysis...
		VDO_DM_SNAPSHOT_NAME=
		VDO_SNAPSHOT_LOOP=
		echo "$TOOL: Initial snapshot status ${initial_status[*]}"
		echo "$TOOL: Failing merge snapshot status ${status[*]}"
		error "ABORTING: Snapshot failed to merge! (Administrator required...)"
	fi

	VDO_INCONSISTENT=
	VDO_CONFIG_RESTORE=

	verbose "Converted VDO volume is merged to \"$1\"."

	"$DMSETUP" remove "$VDO_DM_SNAPSHOT_NAME" || {
		sleep 1 # sleep and retry once more
		"$DMSETUP" remove "$VDO_DM_SNAPSHOT_NAME" || {
			error "ABORTING: Cannot remove snapshot $VDO_DM_SNAPSHOT_NAME! (check volume autoactivation...)"
		}
	}

	VDO_DM_SNAPSHOT_NAME=
	"$LOSETUP" -d "$VDO_SNAPSHOT_LOOP"
	VDO_SNAPSHOT_LOOP=
}

get_enabled_value_() {
	case "$1" in
	enabled) echo "1" ;;
	*) echo "0" ;;
	esac
}

get_kb_size_with_unit_() {
	case "$1" in
	*[kK]) echo $(( ${1%[kK]} )) ;;
	*[mM]) echo $(( ${1%[mM]} * 1024 )) ;;
	*[gG]) echo $(( ${1%[gG]} * 1024 * 1024 )) ;;
	*[tT]) echo $(( ${1%[tT]} * 1024 * 1024 * 1024 )) ;;
	*[pP]) echo $(( ${1%[pP]} * 1024 * 1024 * 1024 * 1024 )) ;;
	esac
}

# Figure out largest possible extent size usable for VG
# $1   physical size
# $2   logical size
get_largest_extent_size_() {
	local max=4
	local i
	local d

	for i in 8 16 32 64 128 256 512 1024 2048 4096 ; do
		d=$(( $1 / i ))
		[ $(( d * i )) -eq "$1" ] || break
		d=$(( $2 / i ))
		[ $(( d * i )) -eq "$2" ] || break
		max=$i
	done
	echo "$max"
}

# detect LV on the given device
# deference device name if it is symbolic link
detect_lv_() {
	local DEVICE=$1
	local SYSVOLUME
	local MAJORMINOR

	DEVICE=${1/#"${DM_DEV_DIR}/"/}
	DEVICE=$("$READLINK" $READLINK_E "$DM_DEV_DIR/$DEVICE" || true)
	[ -n "$DEVICE" ] || error "Readlink cannot access device \"$1\"."
	RDEVICE=$DEVICE
	case "$RDEVICE" in
	  # hardcoded /dev  since udev does not create these entries elsewhere
	  /dev/dm-[0-9]*)
		read -r <"/sys/block/${RDEVICE#/dev/}/dm/name" SYSVOLUME 2>&1 && DEVICE="$DM_DEV_DIR/mapper/$SYSVOLUME"
		read -r <"/sys/block/${RDEVICE#/dev/}/dev" MAJORMINOR 2>&1 || error "Cannot get major:minor for \"$DEVICE\"."
		DEVMAJOR=${MAJORMINOR%%:*}
		DEVMINOR=${MAJORMINOR##*:}
		;;
	  *)
		RSTAT=$("$STAT" --format "DEVMAJOR=\$((0x%t)) DEVMINOR=\$((0x%T))" "$RDEVICE" || true)
		[ -n "$RSTAT" ] || error "Cannot get major:minor for \"$DEVICE\"."
		eval "$RSTAT"
		;;
	esac

	[ "$DEVMAJOR" != "$(grep device-mapper /proc/devices | cut -f1 -d' ')" ] && return

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
	fs="$(echo @|tr @ '\034')"

	(
	    sed -ne '/^--/s|--||g; s|\"|\\\"|g; s/[[:space:]]*$//g;' \
		-e 's/\$/\\\$/g' \
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
			printf("%s%s%s%s=(\"%s\")\n", "'"$prefix"'",vn, $2, conj[indent-1], $3);
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
#       Let's if there would be any singly user requiring this feature.
#       It may allow to better use larger VDO volume size (in TiB ranges).
#
convert_lv_() {
	local vdo_logicalSize=$1
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
	dry "$LVM" lvrename $YES $VERB "$VGNAME/$DM_LV_NAME" "$VGNAME/${LVNAME}_vpool" || {
		error "Rename of LV \"$VGNAME/$DM_LV_NAME\" failed, while VDO header has been already moved!"
	}

	verbose "Converting to VDO pool."
	dry "$LVM" lvconvert $YES $VERB $FORCE --config "$VDO_ALLOCATION_PARAMS" -Zn -V "${vdo_logicalSize}k" -n "$LVNAME" --type vdo-pool "$VGNAME/${LVNAME}_vpool"

	verbose "Removing now unused VDO entry from VDO configuration."
	dry "$VDO" remove $VDOCONF $VERB --force --name "$VDONAME"
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

	if [ -n "$USE_VDO_DM_SNAPSHOT" ]; then
		dry snapshot_create_ "$DEVICE"
		sed "s|$DEVICE|$VDO_DM_SNAPSHOT_DEVICE|" "$TEMPDIR/vdoconf.yml" > "$TEMPDIR/vdo_snap.yml"
		# In case of error in the middle of conversion restore original config file
		VDO_CONFIG_RESTORE="$TEMPDIR/vdoconf.yml"
		# Let VDO manager operate on snapshot volume
		dry cp -a "$TEMPDIR/vdo_snap.yml" "${VDO_CONFIG:-"$DEFAULT_VDO_CONFIG"}"
	else
		# If error in the following section, report possible problems ahead
		VDO_INCONSISTENT=1
	fi

	# In case we operate with snapshot, all lvm2 operation will also run on top of snapshot
	local device=${VDO_DM_SNAPSHOT_DEVICE:-$DEVICE}

	# Check if there is not already an existing PV header, this would have fail on pvcreate after conversion
	"$LVM" pvs --devices "$device" "$device" 2>/dev/null && {
		error "Cannot convert volume \"$DEVICE\" with existing PV header."
	}

	verbose "Moving VDO header on \"$device\"."

	output=$(dry "$VDO" convert $VDOCONF $VERB --force --name "$VDONAME" 2>&1) || {
		local rc=$?
		echo "$output"
		error "Failed to convert VDO volume \"$DEVICE\" (exit code $rc)."
	}

	echo "$output"

	if [ "$ABORT_AFTER_VDO_CONVERT" != "0" ]; then
		warn "Aborting VDO conversion after moving VDO header, volume is useless!"
		return 0
	fi

	# Parse result from VDO preparation/conversion tool
	# New version of the tool provides output with alignment and offset
	local vdo_length=0
	local vdo_aligned=0
	local vdo_offset=0
	local vdo_non_converted=0
	while IFS=  read -r line ; do
		# trim leading spaces
		case "$(echo $line)" in
		"Non converted"*) vdo_non_converted=1 ;;
		"Length"*) vdo_length=${line##* = } ;;
		"Conversion completed"*)
			   vdo_aligned=${line##*aligned on }
			   vdo_aligned=${vdo_aligned%%[!0-9]*}
			   vdo_offset=${line##*offset }
			   # backward compatibility with report from older version
			   vdo_offset=${vdo_offset##*by }
			   vdo_offset=${vdo_offset%%[!0-9]*}
			   ;;
		esac
	done <<< "$output"

	dry "$LVM" pvcreate $YES $VERB $FORCE --devices "$device" --dataalignment "$vdo_offset"b "$device"

	# Obtain free space in this new PV
	# after 'vdo convert' call there is ~(1-2)M free space at the front of the device
	pvfree=$("$BLOCKDEV" --getsize64 "$DEVICE")
	pvfree=$(( ( pvfree - vdo_offset ) / 1024 ))	# to KiB
	if [ -n "$vdo_aligned" ] && [ "$vdo_aligned" != "0" ]; then
		extent_size=$(( vdo_aligned / 1024 ))
	else
		extent_size=$(get_largest_extent_size_ "$pvfree" "$vdo_logicalSize")
	fi

	# Round virtual size to the LOWER size expressed in extent units.
	# lvm is parsing VDO metadata and can read real full size and use it instead of this smaller value.
	# To precisely byte-synchronize the size of VDO LV, user can lvresize such VDO LV later.
	vdo_logicalSizeRounded=$(( ( vdo_logicalSize / extent_size ) * extent_size ))

	verbose "Creating volume group \"$VGNAME\" with the extent size $extent_size KiB."
	dry "$LVM" vgcreate $YES $VERB --devices "$device" -s "${extent_size}k" "$VGNAME" "$device"

	verbose "Creating VDO pool data LV from all extents in the volume group \"$VGNAME\"."
	dry "$LVM" lvcreate -Zn -Wn -an $YES $VERB --devices "$device" -l100%VG -n "${LVNAME}_vpool" "$VGNAME" "$device"

	verbose "Converting to VDO pool."
	dry "$LVM" lvconvert ${USE_VDO_DM_SNAPSHOT:-"$YES"} $VERB $FORCE --devices "$device" --config "$VDO_ALLOCATION_PARAMS" -Zn -V "${vdo_logicalSizeRounded}k" -n "$LVNAME" --type vdo-pool "$VGNAME/${LVNAME}_vpool"

	if [ "$vdo_logicalSizeRounded" -lt "$vdo_logicalSize" ]; then
		# need to extend virtual size to be covering all the converted area
		# let lvm2 to round to the proper virtual size of VDO LV
		dry "$LVM" lvextend $YES $VERB --devices "$device" -L "$vdo_logicalSize"k "$VGNAME/$LVNAME"
	fi

	VDO_INCONSISTENT=

	[ -z "$USE_VDO_DM_SNAPSHOT" ] && return # no-snapshot case finished

	dry "$LVM" vgchange -an $VERB $FORCE --devices "$device" "$VGNAME"

	# Prevent unwanted auto activation when VG is merged
	dry "$LVM" vgchange --setautoactivation n $VERB $FORCE --devices "$device" "$VGNAME"

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
		YES="-y" # From now, now prompting
	fi

	dry snapshot_merge_ "$DEVICE"

	# For systems using devicesfile add 'merged' PV into system.devices.
	# Bypassing use of --valuesonly to keep compatibility with older lvm.
	local usedev=$("$LVM" lvmconfig --typeconfig full devices/use_devicesfile || true)
	[ "${usedev#*=}" = "1" ] && dry "$LVM" lvmdevices --adddev "$DEVICE"

	# Restore auto activation for a VG
	dry "$LVM" vgchange --setautoactivation y $VERB $FORCE "$VGNAME"

	dry "$LVM" lvchange -ay $VERB $FORCE "$VGNAME/$LVNAME"
}

# Convert existing VDO volume into lvm2 volume
convert2lvm_() {
	local VDONAME
	local TRVDONAME
	local FOUND=""
	local MAJOR=0
	local MINOR=0

	VGNAME=${NAME%/*}
	LVNAME=${NAME#*/}
	DM_UUID=""
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
			# Check if we need to generate unused $VGNANE
			if [ -z "$VGNAME" ] || [ "$VGNAME" = "$LVNAME" ]; then
				VGNAME=${DEFAULT_NAME%/*}
				# Find largest matching VG name to our 'default' vgname
				LASTVGNAME=$(LC_ALL=C "$LVM" vgs -oname -O-name --noheadings -S name=~"${VGNAME}" | grep -m 1 -E "${VGNAME}[0-9]? ?" || true)
				if [ -n "$LASTVGNAME" ]; then
					LASTVGNAME=${LASTVGNAME#*"${VGNAME}"}
					# If the number is becoming too high, try some random number
					[ -n "$LASTVGNAME" ] && [ "$LASTVGNAME" -gt 99999999 ] && LASTVGNAME=$RANDOM
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

	verbose "Checked whether device \"$DEVICE\" is already logical volume."

	"$MKDIR" -p -m 0000 "$TEMPDIR" || error "Failed to create \"$TEMPDIR\"."

	# TODO: might use directly  /etc/vdoconf.yml (avoiding need of 'vdo' manager)
	verbose "Getting YAML VDO configuration."
	"$VDO" printConfigFile $VDOCONF >"$TEMPDIR/vdoconf.yml"
	[ -s "$TEMPDIR/vdoconf.yml" ] || error "Cannot work without VDO configuration."

	# Check list of devices in VDO configure file for their major:minor
	# and match with given $DEVICE devmajor:devminor
	for i in $(awk '/.*device:/ {print $2}' "$TEMPDIR/vdoconf.yml") ; do
		local DEV
		DEV=$("$READLINK" $READLINK_E "$i") || continue
		RSTAT=$("$STAT" --format "MAJOR=\$((0x%t)) MINOR=\$((0x%T))" "$DEV" 2>/dev/null) || continue
		eval "$RSTAT"
		if [ "$MAJOR" = "$DEVMAJOR" ] && [ "$MINOR" = "$DEVMINOR" ]; then
			[ -z "$FOUND" ] || error "VDO configuration contains duplicate entries $FOUND and $i."
			FOUND=$i
		fi
	done

	[ -n "$FOUND" ] || error "Can't find matching device in VDO configuration file."
	verbose "Found matching device $FOUND  $MAJOR:$MINOR."

	VDONAME=$(awk -v DNAME="$FOUND" '/.*VDOService$/ {VNAME=substr($1, 0, length($1) - 1)} /[[:space:]]*device:/ { if ($2 ~ DNAME) {print VNAME}}' "$TEMPDIR/vdoconf.yml")
	TRVDONAME=$(echo "$VDONAME" | tr '-' '_')

	# When VDO volume is 'active', check it's not mounted/being used
	DM_OPEN="$("$DMSETUP" info -c -o open  "$VDONAME" --noheadings --nameprefixes 2>/dev/null || true)"
	case "$DM_OPEN" in
	Device*) ;; # no devices
	*)	eval "$DM_OPEN"
		[ "${DM_OPEN:-0}" -eq 0 ] || error "Cannot convert in use VDO volume \"$VDONAME\"!"
		;;
	esac

	#parse_yaml_ "$TEMPDIR/vdoconf.yml" _
	eval "$(parse_yaml_ "$TEMPDIR/vdoconf.yml" _ | grep "$TRVDONAME" | sed -e "s/_config_vdos_$TRVDONAME/vdo/g")"

	vdo_logicalSize=$(get_kb_size_with_unit_ "$vdo_logicalSize")
	vdo_physicalSize=$(get_kb_size_with_unit_ "$vdo_physicalSize")

	verbose "Converted VDO device has logical/physical size $vdo_logicalSize/$vdo_physicalSize KiB."

	VDO_ALLOCATION_PARAMS=$(cat <<EOF
allocation {
	vdo_use_compression = $(get_enabled_value_ "$vdo_compression")
	vdo_use_deduplication = $(get_enabled_value_ "$vdo_deduplication")
	vdo_use_metadata_hints=1
	vdo_minimum_io_size = $vdo_logicalBlockSize
	vdo_block_map_cache_size_mb = $(( $(get_kb_size_with_unit_ "$vdo_blockMapCacheSize") / 1024 ))
	vdo_block_map_period = $vdo_blockMapPeriod
	vdo_use_sparse_index = $(get_enabled_value_ "$vdo_indexSparse")
	vdo_index_memory_size_mb = $(awk "BEGIN {print $vdo_indexMemory * 1024}")
	vdo_slab_size_mb = $(( $(get_kb_size_with_unit_ "$vdo_slabSize") / 1024 ))
	vdo_ack_threads = $vdo_ackThreads
	vdo_bio_threads = $vdo_bioThreads
	vdo_bio_rotation = $vdo_bioRotationInterval
	vdo_cpu_threads = $vdo_cpuThreads
	vdo_hash_zone_threads = $vdo_hashZoneThreads
	vdo_logical_threads = $vdo_logicalThreads
	vdo_physical_threads = $vdo_physicalThreads
	vdo_write_policy = $vdo_writePolicy
	vdo_max_discard = $(( $(get_kb_size_with_unit_ "$vdo_maxDiscardSize") / 4 ))
	vdo_pool_header_size = 0
}
EOF
)
	verbose "VDO conversion parameters: $VDO_ALLOCATION_PARAMS"

	verbose "Stopping VDO volume."
	dry "$VDO" stop $VDOCONF --name "$VDONAME" $VERB

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

#############################
# start point of this script
# - parsing parameters
#############################
trap "cleanup" "${SIGNALS[@]}"

[ "$#" -eq 0 ] && tool_usage

while [ "$#" -ne 0 ]
do
	 case "$1" in
	  "") ;;
	  "-f"|"--force"  ) FORCE="-f" ;;
	  "-h"|"--help"   ) tool_usage ;;
	  "-n"|"--name"   ) shift; NAME=$1 ;;
	  "-v"|"--verbose") VERB="--verbose" ;;
	  "-y"|"--yes"    ) YES="-y" ;;
	  "--abort-after-vdo-convert"|"--abortaftervdoconvert" ) ABORT_AFTER_VDO_CONVERT=1; USE_VDO_DM_SNAPSHOT= ;; # For testing only
	  "--dry-run"|"--dryrun" ) DRY="1" ; VERB="-v" ;;
	  "--no-snapshot"|"--nosnapshot" ) USE_VDO_DM_SNAPSHOT= ;;
	  "--uuid-prefix"|"--uuidprefix" ) shift; DM_UUID_PREFIX=$1 ;; # For testing only
	  "--vdo-config"|"--vdoconfig" ) shift; VDO_CONFIG=$1 ; VDOCONF="-f $VDO_CONFIG" ;;
	  -* ) error "Wrong argument \"$1\". (see: $TOOL --help)" ;;
	  *) DEVICE=$1 ;;  # device name does not start with '-'
	esac
	shift
done

[ -n "$DEVICE" ] || error "Device name is not specified. (see: $TOOL --help)"

convert2lvm_
