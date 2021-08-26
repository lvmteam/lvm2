#!/bin/bash
#
# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
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
# Script for converting VDO volumes to lvm2 VDO LVs
#
# Needed utilities:
#  lvm, dmsetup,
#  vdo, vdo2lvm,
#  grep, awk, sed, blockdev, readlink, mkdir
#
# Conversion is using  'vdo convert' support from VDO manager to move
# existing VDO header by 2M which makes space to place in PV header
# and VG metadata area, and then create VDOPOOL LV and VDO LV in such VG.
#

set -euE -o pipefail

TOOL=lvm_import_vdo

_SAVEPATH=$PATH
PATH="/sbin:/usr/sbin:/bin:/usr/sbin:$PATH"

# user may override lvm location by setting LVM_BINARY
LVM=${LVM_BINARY:-lvm}
VDO=${VDO_BINARY:-vdo}
VDOCONF=${VDOCONF:-}
BLOCKDEV="blockdev"
READLINK="readlink"
READLINK_E="-e"
MKDIR="mkdir"

TEMPDIR="${TMPDIR:-/tmp}/${TOOL}_${RANDOM}$$"
DM_DEV_DIR="${DM_DEV_DIR:-/dev}"

DRY=0
VERB=""
FORCE=""
YES=""

# default name for converted VG and its VDO LV
NAME="vdovg/vdolvol"

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
	echo "	       --dry-run      Print commands without running them"

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
	cleanup 1
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
	trap '' 2

	rm -rf "$TEMPDIR"
	# error exit status for break
	exit "${1:-1}"
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

get_mb_size_with_unit_() {
	case "$1" in
	*[mM]) echo $(( ${1%[mM]} )) ;;
	*[gG]) echo $(( ${1%[gG]} * 1024 )) ;;
	*[tT]) echo $(( ${1%[tT]} * 1024 * 1024 )) ;;
	*[pP]) echo $(( ${1%[pP]} * 1024 * 1024 * 1024 )) ;;
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
		test $(( d * i )) -eq "$1" || break
		d=$(( $2 / i ))
		test $(( d * i )) -eq "$2" || break
		max=$i
	done
	echo "$max"
}

# detect LV on the given device
# dereference device name if it is symbolic link
detect_lv_() {
	local DEVICE=$1
	local MAJOR
	local MINOR
	local SYSVOLUME
	local MAJORMINOR

	DEVICE=${1/#"${DM_DEV_DIR}/"/}
	DEVICE=$("$READLINK" $READLINK_E "$DM_DEV_DIR/$DEVICE")
	test -n "$DEVICE" || error "Cannot get readlink \"$1\"."
	RDEVICE=$DEVICE
	case "$RDEVICE" in
	  # hardcoded /dev  since udev does not create these entries elsewhere
	  /dev/dm-[0-9]*)
		read -r <"/sys/block/${RDEVICE#/dev/}/dm/name" SYSVOLUME 2>&1 && DEVICE="$DM_DEV_DIR/mapper/$SYSVOLUME"
		read -r <"/sys/block/${RDEVICE#/dev/}/dev" MAJORMINOR 2>&1 || error "Cannot get major:minor for \"$DEVICE\"."
		MAJOR=${MAJORMINOR%%:*}
		MINOR=${MAJORMINOR##*:}
		;;
	  *)
		STAT=$(stat --format "MAJOR=\$((0x%t)) MINOR=\$((0x%T))" "$RDEVICE")
		test -n "$STAT" || error "Cannot get major:minor for \"$DEVICE\"."
		eval "$STAT"
		;;
	esac

	eval "$(dmsetup info -c -j "$MAJOR" -m "$MINOR" -o uuid,name --noheadings --nameprefixes --separator ' ')"
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

# convert existing VDO volume into lvm2 volume
convert2lvm_() {
	local DEVICE=$1
	local VGNAME=${NAME%/*}
	local LVNAME=${NAME#*/}
	local VDONAME
	local TRVDONAME
	local EXTENTSZ
	local IS_LV=1

	DM_UUID=""
	detect_lv_ "$DEVICE"
	case "$DM_UUID" in
		LVM-*)	eval "$(dmsetup splitname --nameprefixes --noheadings --separator ' ' "$DM_NAME")"
			if [ -z "$VGNAME" ] || [ "$VGNAME" = "$LVNAME" ]  ; then
				VGNAME=$DM_VG_NAME
			elif test "$VGNAME" != "$DM_VG_NAME" ; then
				error "Volume group name \"$VGNAME\" does not match name \"$DM_VG_NAME\" for device \"$DEVICE\"."
			fi
			;;
		*) IS_LV=0
			# Check $VGNANE does not already exists
			"$LVM" vgs "$VGNAME" && error "Cannot use already existing volume group name \"$VGNAME\"."
			;;
	esac

	verbose "Checked whether device $1 is already LV ($IS_LV)."

	"$MKDIR" -p -m 0000 "$TEMPDIR" || error "Failed to create $TEMPDIR."

	verbose "Getting YAML VDO configuration."
	"$VDO" printConfigFile $VDOCONF >"$TEMPDIR/vdoconf.yml"

	VDONAME=$(awk -v DNAME="$DEVICE" '/.*VDOService$/ {VNAME=substr($1, 0, length($1) - 1)} /[[:space:]]*device:/ { if ($2 ~ DNAME) {print VNAME}}' "$TEMPDIR/vdoconf.yml")
	TRVDONAME=$(echo "$VDONAME" | tr '-' '_')

	# When VDO volume is 'active', check it's not mounted/being used
	eval "$(dmsetup info -c -o open  "$VDONAME" --noheadings --nameprefixes || true)"
	test "${DM_OPEN:-0}" -eq 0 || error "Cannot converted VDO volume \"$VDONAME\" which is in use!"

	#parse_yaml_ "$TEMPDIR/vdoconf.yml" _
	eval "$(parse_yaml_ "$TEMPDIR/vdoconf.yml" _ | grep "$TRVDONAME" | sed -e "s/_config_vdos_$TRVDONAME/vdo/g")"

	vdo_logicalSize=$(get_kb_size_with_unit_ "$vdo_logicalSize")
	vdo_physicalSize=$(get_kb_size_with_unit_ "$vdo_physicalSize")

	verbose "Going to convert physical sized VDO device $vdo_physicalSize KiB."
	verbose "With logical volume of size $vdo_logicalSize KiB."

	PARAMS=$(cat <<EOF
allocation {
	vdo_use_compression = $(get_enabled_value_ "$vdo_compression")
	vdo_use_deduplication = $(get_enabled_value_ "$vdo_deduplication")
	vdo_use_metadata_hints=1
	vdo_minimum_io_size = $vdo_logicalBlockSize
	vdo_block_map_cache_size_mb = $(get_mb_size_with_unit_ "$vdo_blockMapCacheSize")
	vdo_block_map_period = $vdo_blockMapPeriod
	vdo_check_point_frequency = $vdo_indexCfreq
	vdo_use_sparse_index = $(get_enabled_value_ "$vdo_indexSparse")
	vdo_index_memory_size_mb = $(awk "BEGIN {print $vdo_indexMemory * 1024}")
	vdo_slab_size_mb = $(get_mb_size_with_unit_ "$vdo_blockMapCacheSize")
	vdo_ack_threads = $vdo_ackThreads
	vdo_bio_threads = $vdo_bioThreads
	vdo_bio_rotation = $vdo_bioRotationInterval
	vdo_cpu_threads = $vdo_cpuThreads
	vdo_hash_zone_threads = $vdo_hashZoneThreads
	vdo_logical_threads = $vdo_logicalThreads
	vdo_physical_threads = $vdo_physicalThreads
	vdo_write_policy = $vdo_writePolicy
	vdo_max_discard = $(( $(get_kb_size_with_unit_ "$vdo_maxDiscardSize") * 1024 ))
	vdo_pool_header_size = 0
}
EOF
)
	verbose "VDO conversion paramaters: $PARAMS"

	verbose "Stopping VDO volume."
	dry "$VDO" stop $VDOCONF --name "$VDONAME"

	if [ "$IS_LV" = "0" ]; then
		verbose "Moving VDO header by 2MiB."
		dry "$VDO" convert $VDOCONF --force --name "$VDONAME"

		dry "$LVM" pvcreate $YES --dataalignment 2M "$DEVICE" || {
			error "Creation of PV on \"$DEVICE\" failed, while VDO header has been already moved!"
		}

		# Obtain free space in this new PV
		# after 'vdo convert/vdo2lvm' call there is +2M free space at the front of the device
		case "$DRY" in
		0) pvfree=$("$LVM" pvs -o devsize --units b --nosuffix --noheadings "$DEVICE") ;;
		*) pvfree=$("$BLOCKDEV" --getsize64 "$DEVICE") ;;
		esac

		pvfree=$(( pvfree / 1024 - 2048 ))	# to KiB
	else
		pvfree=$("$LVM" lvs -o size --units b --nosuffix --noheadings "$VGNAME/$LVNAME")
		pvfree=$(( pvfree / 1024 ))		# to KiB
	fi

	# select largest possible extent size that can exactly express both sizes
	EXTENTSZ=$(get_largest_extent_size_ "$pvfree" "$vdo_logicalSize")

	if [ "$IS_LV" = "0" ]; then
		verbose "Creating VG \"${NAME%/*}\" with extent size $EXTENTSZ KiB."
		dry "$LVM" vgcreate $YES $VERB -s "${EXTENTSZ}k" "$VGNAME" "$DEVICE" || {
			error "Creation of VG \"$VGNAME\" failed, while VDO header has been already moved!"
		}

		verbose "Creating VDO pool data LV from all extents in volume group $VGNAME."
		dry "$LVM" lvcreate -Zn -Wn $YES $VERB -l100%VG -n "${LVNAME}_vpool" "$VGNAME"
	else
		# validate existing  VG extent_size can express virtual VDO size
		vg_extent_size=$("$LVM" vgs -o vg_extent_size --units b --nosuffix --noheadings "$VGNAME" || true)
		vg_extent_size=$(( vg_extent_size / 1024 ))

		test "$vg_extent_size" -le "$EXTENTSZ" || {
			error "Please vgchange extent_size to at most $EXTENTSZ KiB or extend and align virtual size on $vg_extent_size KiB."
		}
		verbose "Renaming existing LV to be used as _vdata volume for VDO pool LV."
		dry "$LVM" lvrename $YES $VERB "$VGNAME/$LVNAME" "$VGNAME/${LVNAME}_vpool" || {
			error "Rename of LV \"$VGNAME/$LVNAME\" failed, while VDO header has been already moved!"
		}
	fi

	verbose "Converting to VDO pool."
	dry "$LVM" lvconvert $YES $VERB $FORCE --config "$PARAMS" -Zn -V "${vdo_logicalSize}k" -n "$LVNAME" --type vdo-pool "$VGNAME/${LVNAME}_vpool"

	rm -fr "$TEMPDIR"
}

#############################
# start point of this script
# - parsing parameters
#############################
trap "cleanup 2" 2

test "$#" -eq 0 && tool_usage

while [ "$#" -ne 0 ]
do
	 case "$1" in
	  "") ;;
	  "-f"|"--force"  ) FORCE="-f" ;;
	  "-h"|"--help"   ) tool_usage ;;
	  "-n"|"--name"   ) shift; NAME=$1 ;;
	  "-v"|"--verbose") VERB="-v" ;;
	  "-y"|"--yes"    ) YES="-y" ;;
	  "--dry-run"     ) DRY="1" ;;
	  "-*") error "Wrong argument \"$1\". (see: $TOOL --help)" ;;
	  *) DEVICENAME=$1 ;;  # device name does not start with '-'
	esac
	shift
done

# do conversion
convert2lvm_ "$DEVICENAME"
