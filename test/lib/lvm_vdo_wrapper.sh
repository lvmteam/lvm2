#!/bin/bash
#
# Wrapper script for 'naive' emulation of vdo manager tool for systems
# that no longer have this tool present
#

set -euE -o pipefail

# tool for formatting 'old' VDO metadata format
LVM_VDO_FORMAT=${LVM_VDO_FORMAT-"oldvdoformat"}
# tool for shifting VDO metadata header by 2MiB
LVM_VDO_PREPARE=${LVM_VDO_PREPARE-"oldvdoprepareforlvm"}
# default vdo conf file
LVM_VDO_DEFAULT_CONF=${LVM_VDO_DEFAULT_CONF-"${TMPDIR:-/tmp}/vdoconf.yml"}

vdo_die_() {
	echo -e "$@" >&2
	return 1
}

vdo_verbose_() {
	test -z "$vdo_verbose" || echo "$0:" "$@"
}

vdo_dry_() {
	if test -n "$vdo_dry"; then
		vdo_verbose_ "Dry execution" "$@"
		return 0
	fi
	vdo_verbose_ "Executing" "$@"
	"$@"
}

vdo_get_kb_size_with_unit_() {
	local sz=${2-1}  # 2nd. arg as unit - default 'k'

	case "$sz" in
	  [mM]) sz=1024 ;;
	esac

	case "$1" in
	  *[kK]) sz=1 ;;
	  *[mM]) sz=1024 ;;
	  *[gG]) sz=$(( 1024 * 1024 )) ;;
	  *[tT]) sz=$(( 1024 * 1024 * 1024 )) ;;
	  *[pP]) sz=$(( 1024 * 1024 * 1024 * 1024 )) ;;
	esac

	echo $(( sz * ${1%[kKmMgGtTpP]} ))
}

#
# Emulate functionality of deprecated 'vdo create'
#
vdo_create_() {
local cachesize=
local devsize=
local emulate512=disabled
local logicalsize=
local maxdiscardsize=
local slabbits=0  # 4k
local slabsize=
local sparse=
local table=
local vdo_compression_msg=
local vdo_dry=
local vdo_index_msg=
local vdo_logicalBlockSize=
local vdo_verbose=

local vdo_ackThreads=${vdo_ackThreads-1}
local vdo_bioRotationInterval=${vdo_bioRotationInterval-64}
local vdo_bioThreads=${vdo_bioThreads-4}
local vdo_blockMapCacheSize=${vdo_blockMapCacheSize-128M}
local vdo_blockMapPeriod=${vdo_blockMapPeriod-16380}
local vdo_compression=${vdo_compression-enabled}
local vdo_confFile=$LVM_VDO_DEFAULT_CONF  # place some file in /tmp
local vdo_cpuThreads=${vdo_cpuThreads-2}
local vdo_deduplication=${vdo_deduplication-enabled}
local vdo_hashZoneThreads=${vdo_hashZoneThreads-1}
local vdo_indexCfreq=${vdo_indexCfreq-0}
local vdo_indexMemory=${vdo_indexMemory-0.25}
local vdo_indexSparse=${vdo_indexSparse-disabled}
local vdo_indexThreads=${vdo_indexThreads-0}
local vdo_logicalSize=${vdo_logicalSize-0}
local vdo_logicalThreads=${vdo_logicalThreads-1}
local vdo_maxDiscardSize=${vdo_maxDiscardSize-4K}
local vdo_name=${vdo_name-VDONAME}
local vdo_physicalThreads=${vdo_physicalThreads-1}
local vdo_slabSize=${vdo_slabSize-2G}
local vdo_writePolicy=${vdo_writePolicy-auto}
local vdo_uuid

vdo_uuid="VDO-$(uuidgen || echo \"f7a3ecdc-40a0-4e43-814c-4a7039a75de4\")"

while [ "$#" -ne 0 ]
do
	case "$1" in
	  "--blockMapCacheSize") shift; vdo_blockMapCacheSize=$1 ;;
	  "--blockMapPeriod") shift; vdo_blockMapPeriod=$1 ;;
	  "--compression") shift; vdo_compression=$1 ;;
	  "--confFile"|"-f") shift; vdo_confFile=$1 ;;
	  "--deduplication") shift; vdo_deduplication=$1 ;;
	  "--device") shift; vdo_device=$1 ;;
	  "--emulate512") shift; emulate512=$1 ;;
	  "--indexMem") shift; vdo_indexMemory=$1 ;;
	  "--maxDiscardSize") shift; vdo_maxDiscardSize=$1 ;;
	  "--name"|"-n") shift; vdo_name=$1 ;;
	  "--sparseIndex") shift; vdo_indexSparse=$1 ;;
	  "--uuid") shift ;;		# ignored
	  "--vdoAckThreads") shift; vdo_ackThreads=$1 ;;
	  "--vdoBioRotationInterval") shift; vdo_bioRotationInterval=$1 ;;
	  "--vdoBioThreads") shift; vdo_bioThreads=$1 ;;
	  "--vdoCpuThreads") shift; vdo_cpuThreads=$1 ;;
	  "--vdoHashZoneThreads") shift; vdo_hashZoneThreads=$1 ;;
	  "--vdoLogicalSize") shift; vdo_logicalSize=$1 ;;
	  "--vdoLogicalThreads") shift; vdo_logicalThreads=$1 ;;
	  "--vdoLogLevel") shift ;;	# ignored
	  "--vdoPhysicalThreads") shift; vdo_physicalThreads=$1 ;;
	  "--vdoSlabSize") shift; vdo_slabSize=$1 ;;
	  "--verbose"|"-d"|"--debug") vdo_verbose="-v" ;;
	  "--writePolicy") shift; vdo_writePolicy=$1 ;;
	esac
	shift
done

# Convert when set
case "$emulate512" in
  "enabled") vdo_logicalBlockSize=512 ;;
  "disabled") vdo_logicalBlockSize=4096 ;;
  *) vdo_die_ "Invalid emulate512 setting."
esac

case "$vdo_deduplication" in
  "enabled")  vdo_index_msg="index-enable" ;;
  "disabled") vdo_index_msg="index-disable";;
  *) vdo_die_ "Invalid deduplication setting."
esac

case "$vdo_compression" in
  "enabled")  vdo_compression_msg="compression on" ;;
  "disabled") vdo_compression_msg="compression off";;
  *) vdo_die_ "Invalid compression setting."
esac

test -n "${vdo_device-}" || vdo_die_ "VDO device is missing"

blkid -c /dev/null -s UUID -o value "${vdo_device}" || true

devsize=$(blockdev --getsize64 "$vdo_device")
devsize=$(( devsize / 4096 )) # convert to 4KiB units

logicalsize=$(vdo_get_kb_size_with_unit_ "$vdo_logicalSize" M)
logicalsize=$(( logicalsize * 2 ))	# 512B  units

cachesize=$(vdo_get_kb_size_with_unit_ "$vdo_blockMapCacheSize" M)
cachesize=$(( cachesize / 4 ))		# 4KiB units

maxdiscardsize=$(vdo_get_kb_size_with_unit_ "$vdo_maxDiscardSize" M)
maxdiscardsize=$(( maxdiscardsize / 4 )) # 4KiB units

vdo_link=$(udevadm info --no-pager --query=symlink --name="$vdo_device" 2>/dev/null)
vdo_link=${vdo_link%% *}
if test -n "$vdo_link" ; then
	vdo_link="/dev/$vdo_link"
else
	vdo_link=$vdo_device
fi

test -e "$vdo_confFile" || {
	cat > "$vdo_confFile" <<EOF
####################################################################
# THIS FILE IS MACHINE GENERATED. DO NOT EDIT THIS FILE BY HAND.
####################################################################
config: !Configuration
  vdos:
EOF
}

cat >> "$vdo_confFile" <<EOF
    $vdo_name: !VDOService
      _operationState: finished
      ackThreads: $vdo_ackThreads
      activated: enabled
      bioRotationInterval: $vdo_bioRotationInterval
      bioThreads: $vdo_bioThreads
      blockMapCacheSize: $(( cachesize * 4 ))K
      blockMapPeriod: $vdo_blockMapPeriod
      compression: $vdo_compression
      cpuThreads: $vdo_cpuThreads
      deduplication: $vdo_deduplication
      device: $vdo_link
      hashZoneThreads: $vdo_hashZoneThreads
      indexCfreq: $vdo_indexCfreq
      indexMemory: $vdo_indexMemory
      indexSparse: $vdo_indexSparse
      indexThreads: $vdo_indexThreads
      logicalBlockSize: $vdo_logicalBlockSize
      logicalSize: $(( logicalsize / 2 ))K
      logicalThreads: $vdo_logicalThreads
      maxDiscardSize: $(( maxdiscardsize * 4 ))K
      name: $vdo_name
      physicalSize: $(( devsize * 4 ))K
      physicalThreads: $vdo_physicalThreads
      slabSize: $vdo_slabSize
      uuid: $vdo_uuid
      writePolicy: $vdo_writePolicy
  version: 538380551
EOF

slabsize=$(vdo_get_kb_size_with_unit_ "$vdo_slabSize")
while test "$slabsize" -gt 4 ; do
	slabbits=$(( slabbits + 1 ))
	slabsize=$(( slabsize / 2 ))
done

case "$vdo_indexSparse" in
  "enabled") sparse="--uds-sparse" ;;
esac

vdo_dry_ "$LVM_VDO_FORMAT" $vdo_verbose $sparse\
 --logical-size "$vdo_logicalSize" --slab-bits "$slabbits"\
 --uds-checkpoint-frequency "$vdo_indexCfreq"\
 --uds-memory-size "$vdo_indexMemory" "$vdo_device"

# V2 format
table="0 $logicalsize vdo V2 $vdo_device\
 $devsize\
 $vdo_logicalBlockSize\
 $cachesize\
 $vdo_blockMapPeriod\
 on\
 $vdo_writePolicy\
 $vdo_name\
 maxDiscard $maxdiscardsize\
 ack $vdo_ackThreads\
 bio $vdo_bioThreads\
 bioRotationInterval $vdo_bioRotationInterval\
 cpu $vdo_cpuThreads\
 hash $vdo_hashZoneThreads\
 logical $vdo_logicalThreads\
 physical $vdo_physicalThreads"

vdo_dry_ dmsetup create "$vdo_name" --uuid "$vdo_uuid" --table "$table"
vdo_dry_ dmsetup message "$vdo_name" 0 "$vdo_index_msg"
vdo_dry_ dmsetup message "$vdo_name" 0 "$vdo_compression_msg"
}

#
# vdo stop
#
vdo_stop_() {
local vdo_confFile=$LVM_VDO_DEFAULT_CONF
local vdo_dry=
local vdo_force=
local vdo_name=
local vdo_verbose=

while [ "$#" -ne 0 ]
do
	case "$1" in
	  "--confFile"|"-f") shift; vdo_confFile=$1 ;;
	  "--name"|"-n") shift; vdo_name=$1 ;;
	  "--verbose"|"-d"|"--debug") vdo_verbose="-v" ;;
	  "--force") vdo_force="--force" ;;
	esac
	shift
done

test -z "$vdo_verbose" || vdo_dry_ dmsetup status --target vdo "$vdo_name" 2>/dev/null || return 0
vdo_dry_ dmsetup remove $vdo_force "$vdo_name" || true
}

#
# vdo remove
#
vdo_remove_() {
local vdo_confFile=$LVM_VDO_DEFAULT_CONF
local vdo_name=

vdo_stop_ "$@"
while [ "$#" -ne 0 ]
do
	case "$1" in
	  "--confFile"|"-f") shift; vdo_confFile=$1 ;;
	  "--name"|"-n") shift; vdo_name=$1 ;;
	esac
	shift
done

# remove entry from conf file
awk -v vdovolname="$vdo_name" 'BEGIN { have=0 }
	$0 ~ "!VDOService" { have=0 }
	$0 ~ vdovolname":" { have=1 }
	{ if (have==0) { print } ;}
	' "$vdo_confFile" >"${vdo_confFile}.new"

mv "${vdo_confFile}.new" "$vdo_confFile"
grep "!VDOService" "$vdo_confFile" || rm -f "$vdo_confFile"
}


#
# print_config_file
#
vdo_print_config_file_() {
local vdo_confFile=$LVM_VDO_DEFAULT_CONF

while [ "$#" -ne 0 ]
do
	case "$1" in
	  "--confFile"|"-f") shift; vdo_confFile=$1 ;;
	  "--verbose"|"-d"|"--debug") ;;
	  "--logfile") shift ;;  # ignore
	esac
	shift
done

cat "$vdo_confFile"
}

#
# vdo convert
#
vdo_convert_() {
local vdo_confFile=$LVM_VDO_DEFAULT_CONF
local vdo_dry=
local vdo_force=
local vdo_name=
local vdo_verbose=
local vdo_device=
local vdo_dry_run=
local vdo_check=
local vdo_version=
local vdo_help=

while [ "$#" -ne 0 ]
do
	case "$1" in
	  "--confFile"|"-f") shift; vdo_confFile=$1 ;;
	  "--name"|"-n") shift; vdo_name=$1 ;;
	  "--verbose"|"-d"|"--debug") vdo_verbose="-v" ;;
	  "--force") vdo_force="--force" ;;
	  "--dry-run") vdo_dry_run="--dry-run" ;;
	  "--check") vdo_check="--check" ;;
	  "--version") vdo_version="--version" ;;
	  "--help") vdo_help="--help" ;;
	esac
	shift
done

vdo_device=$(awk -v vdovolname="$vdo_name" 'BEGIN { have=0 }
     $0 ~ "!VDOService" { have=0 }
     $0 ~ vdovolname":" { have=1 }
     { if (have==1 && $0 ~ "device:" ) {  print $2 } ;}'\
     "$vdo_confFile")

#dmsetup status --target vdo "$vdo_name" || true
vdo_dry_ "$LVM_VDO_PREPARE" $vdo_dry_run $vdo_check $vdo_version $vdo_help "$vdo_device"
vdo_dry_ vdo_remove_ -f "$vdo_confFile" -n "$vdo_name" || true
}

#
# MAIN
#
case "${1-}" in
  "create") shift; vdo_create_ "$@" ;;
  "remove") shift; vdo_remove_ "$@" ;;
  "stop") shift; vdo_stop_ "$@" ;;
  "convert") shift; vdo_convert_ "$@" ;;
  "printConfigFile") shift; vdo_print_config_file_ "$@" ;;
esac
