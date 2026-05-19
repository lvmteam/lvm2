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

# vdo conf file format version
LVM_VDO_CONF_VERSION=538380551

vdo_die_() {
	echo "${0##*/}: $*" 1>&2
	return 1
}

vdo_verbose_() {
	test -z "$vdo_verbose" || echo "${0##*/}: $*"
}

vdo_dry_() {
	if test -n "$vdo_dry"; then
		vdo_verbose_ "Dry execution $*"
		return 0
	fi
	vdo_verbose_ "Executing $*"
	"$@"
}

vdo_get_kb_size_with_unit_() {
	local num=${1%[kKmMgGtTpP]}
	local unit=${2-k}
	local multiplier

	case "$1" in
	  *[kKmMgGtTpP]) unit=${1##"$num"} ;;
	esac

	case "$unit" in
	  [kK]) multiplier=1 ;;
	  [mM]) multiplier=$(( 1 << 10 )) ;;
	  [gG]) multiplier=$(( 1 << 20 )) ;;
	  [tT]) multiplier=$(( 1 << 30 )) ;;
	  [pP]) multiplier=$(( 1 << 40 )) ;;
	  *) vdo_die_ "Unknown unit: $unit" ;;
	esac

	echo $(( multiplier * num ))
}

vdo_conf_remove_entry_() {
	awk -v vdovolname="$1" 'BEGIN { have=0 }
		index($0, "!VDOService") > 0 { have=0 }
		index($0, vdovolname":") > 0 { have=1 }
		{ if (have==0) { print } }
		' "$2"
}

vdo_conf_get_field_() {
	awk -v vdovolname="$1" -v field="$2" 'BEGIN { have=0 }
		index($0, "!VDOService") > 0 { have=0 }
		index($0, vdovolname":") > 0 { have=1 }
		{ if (have==1 && index($0, field":") > 0) { print $2 } }
		' "$3"
}

#
# Emulate functionality of deprecated 'vdo create'
#
vdo_create_() {
	local vdo_cachesize_kb=
	local vdo_compression_msg=
	local vdo_dedup_state=on
	local vdo_devsize_kb=
	local vdo_dry=
	local vdo_emulate512=disabled
	local vdo_index_msg=
	local vdo_logicalBlockSize=
	local vdo_logicalsize_kb=
	local vdo_maxdiscardsize_kb=
	local vdo_slabbits=0  # 4k
	local vdo_slabsize=
	local vdo_format_opts=()
	local vdo_table=
	local vdo_ver
	local vdo_verbose=

	local vdo_ackThreads=${vdo_ackThreads-1}
	local vdo_bioRotationInterval=${vdo_bioRotationInterval-64}
	local vdo_bioThreads=${vdo_bioThreads-4}
	local vdo_blockMapCacheSize=${vdo_blockMapCacheSize-128M}
	local vdo_blockMapPeriod=${vdo_blockMapPeriod-16380}
	local vdo_compression=${vdo_compression-enabled}
	local vdo_confFile=$LVM_VDO_DEFAULT_CONF
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

	vdo_uuid="VDO-$(uuidgen || echo 'f7a3ecdc-40a0-4e43-814c-4a7039a75de4')"

	while [ "$#" -ne 0 ]
	do
		case "$1" in
		  -d|--debug|--verbose) vdo_verbose="-v" ; vdo_format_opts+=("-v") ;;
		  -f|--confFile) shift; vdo_confFile=$1 ;;
		  -n|--name) shift; vdo_name=$1 ;;
		  --blockMapCacheSize) shift; vdo_blockMapCacheSize=$1 ;;
		  --blockMapPeriod) shift; vdo_blockMapPeriod=$1 ;;
		  --compression) shift; vdo_compression=$1 ;;
		  --deduplication) shift; vdo_deduplication=$1 ;;
		  --device) shift; vdo_device=$1 ;;
		  --dry-run) vdo_dry=1 ;;
		  --indexMem) shift; vdo_indexMemory=$1 ;;
		  --maxDiscardSize) shift; vdo_maxDiscardSize=$1 ;;
		  --sparseIndex) shift; vdo_indexSparse=$1 ;;
		  --uuid) shift ;;		# ignored
		  --vdoAckThreads) shift; vdo_ackThreads=$1 ;;
		  --vdoBioRotationInterval) shift; vdo_bioRotationInterval=$1 ;;
		  --vdoBioThreads) shift; vdo_bioThreads=$1 ;;
		  --vdoCpuThreads) shift; vdo_cpuThreads=$1 ;;
		  --vdoHashZoneThreads) shift; vdo_hashZoneThreads=$1 ;;
		  --vdoLogicalSize) shift; vdo_logicalSize=$1 ;;
		  --vdoLogicalThreads) shift; vdo_logicalThreads=$1 ;;
		  --vdoLogLevel) shift ;;	# ignored
		  --vdoPhysicalThreads) shift; vdo_physicalThreads=$1 ;;
		  --vdoSlabSize) shift; vdo_slabSize=$1 ;;
		  --emulate512) shift; vdo_emulate512=$1 ;;
		  --writePolicy) shift; vdo_writePolicy=$1 ;;
		  *) vdo_die_ "Unknown option: $1" ;;
		esac
		shift
	done

	case "$vdo_emulate512" in
	  enabled)  vdo_logicalBlockSize=512  ;;
	  disabled) vdo_logicalBlockSize=4096 ;;
	  *) vdo_die_ "Invalid emulate512 setting."
	esac

	case "$vdo_deduplication" in
	  enabled)  vdo_index_msg="index-enable" ;;
	  disabled) vdo_index_msg="index-disable";;
	  *) vdo_die_ "Invalid deduplication setting."
	esac

	case "$vdo_compression" in
	  enabled)  vdo_compression_msg="compression on" ;;
	  disabled) vdo_compression_msg="compression off";;
	  *) vdo_die_ "Invalid compression setting."
	esac

	test -n "${vdo_device-}" || vdo_die_ "VDO device is missing."

	vdo_cachesize_kb=$(vdo_get_kb_size_with_unit_ "$vdo_blockMapCacheSize" M)
	vdo_devsize_kb=$(( $(blockdev --getsize64 "$vdo_device") / 1024 ))
	vdo_logicalsize_kb=$(vdo_get_kb_size_with_unit_ "$vdo_logicalSize" M)
	vdo_maxdiscardsize_kb=$(vdo_get_kb_size_with_unit_ "$vdo_maxDiscardSize" M)

	# Truncate to 4KiB boundary (DM table uses 4KiB blocks)
	vdo_cachesize_kb=$(( (vdo_cachesize_kb / 4) * 4 ))
	vdo_devsize_kb=$(( (vdo_devsize_kb / 4) * 4 ))
	vdo_maxdiscardsize_kb=$(( (vdo_maxdiscardsize_kb / 4) * 4 ))

	vdo_link=$(udevadm info --no-pager --query=symlink --name="$vdo_device" 2>/dev/null) || true
	vdo_link=${vdo_link%% *}
	if test -n "$vdo_link" ; then
		vdo_link="/dev/$vdo_link"
	else
		vdo_link="$vdo_device"
	fi

	# Kernel v9+ VDO rewrites UDS index in incompatible format,
	# activate non-LV devices with dedup off to keep index readable
	vdo_ver=$(dmsetup targets 2>/dev/null | awk '/^vdo /{sub(/^.*v/,""); print}')
	if test -n "$vdo_ver" && test "${vdo_ver%%.*}" -ge 9 2>/dev/null; then
		local dev_uuid
		dev_uuid=$(dmsetup info -c --noheadings -o uuid "$vdo_device" 2>/dev/null) || true
		case "$dev_uuid" in
		  LVM-*) ;;
		  *) vdo_dedup_state="off"
		     vdo_deduplication=disabled
		     vdo_verbose_ "Disabling dedup (kernel VDO v$vdo_ver UDS index format incompatible)"
		     ;;
		esac
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
      blockMapCacheSize: ${vdo_cachesize_kb}K
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
      logicalSize: ${vdo_logicalsize_kb}K
      logicalThreads: $vdo_logicalThreads
      maxDiscardSize: ${vdo_maxdiscardsize_kb}K
      name: $vdo_name
      physicalSize: ${vdo_devsize_kb}K
      physicalThreads: $vdo_physicalThreads
      slabSize: $vdo_slabSize
      uuid: $vdo_uuid
      writePolicy: $vdo_writePolicy
  version: $LVM_VDO_CONF_VERSION
EOF

	# slab-bits = log2(slab size in 4KiB units)
	vdo_slabsize=$(vdo_get_kb_size_with_unit_ "$vdo_slabSize")
	while test "$vdo_slabsize" -gt 4 ; do
		vdo_slabbits=$(( vdo_slabbits + 1 ))
		vdo_slabsize=$(( vdo_slabsize / 2 ))
	done

	case "$vdo_indexSparse" in
	  enabled) vdo_format_opts+=("--uds-sparse") ;;
	esac

	vdo_dry_ "$LVM_VDO_FORMAT" "${vdo_format_opts[@]}"\
		--logical-size "$vdo_logicalSize" --slab-bits "$vdo_slabbits"\
		--uds-checkpoint-frequency "$vdo_indexCfreq"\
		--uds-memory-size "$vdo_indexMemory" "$vdo_device"

	# V2 format (sizes: logicalsize in 512B sectors, others in 4KiB blocks)
	vdo_table=(
		0 $(( vdo_logicalsize_kb * 2 )) vdo V2 "$vdo_device"
		$(( vdo_devsize_kb / 4 ))
		"$vdo_logicalBlockSize"
		$(( vdo_cachesize_kb / 4 ))
		"$vdo_blockMapPeriod"
		"$vdo_dedup_state"
		"$vdo_writePolicy"
		"$vdo_name"
		maxDiscard $(( vdo_maxdiscardsize_kb / 4 ))
		ack "$vdo_ackThreads"
		bio "$vdo_bioThreads"
		bioRotationInterval "$vdo_bioRotationInterval"
		cpu "$vdo_cpuThreads"
		hash "$vdo_hashZoneThreads"
		logical "$vdo_logicalThreads"
		physical "$vdo_physicalThreads"
	)

	vdo_dry_ dmsetup create "$vdo_name" --uuid "$vdo_uuid" --table "${vdo_table[*]}"

	if test "$vdo_dedup_state" = "on"; then
		vdo_dry_ dmsetup message "$vdo_name" 0 "$vdo_index_msg"
	fi
	vdo_dry_ dmsetup message "$vdo_name" 0 "$vdo_compression_msg"
}

#
# vdo stop
#
vdo_stop_() {
	local vdo_confFile=$LVM_VDO_DEFAULT_CONF
	local vdo_dry=
	local vdo_name=
	local vdo_opts=()
	local vdo_verbose=

	while [ "$#" -ne 0 ]
	do
		case "$1" in
		  -d|--debug|--verbose) vdo_verbose="-v" ;;
		  -f|--confFile) shift; vdo_confFile=$1 ;;
		  -n|--name) shift; vdo_name=$1 ;;
		  --dry-run) vdo_dry=1 ;;
		  --force) vdo_opts+=("--force") ;;
		  *) vdo_die_ "Unknown option: $1" ;;
		esac
		shift
	done

	if test -n "$vdo_verbose"; then
		vdo_dry_ dmsetup status --target vdo "$vdo_name" 2>/dev/null || return 0
	fi
	vdo_dry_ dmsetup remove "${vdo_opts[@]}" "$vdo_name" || true
}

#
# vdo remove
#
vdo_remove_() {
	local vdo_confFile=$LVM_VDO_DEFAULT_CONF
	local vdo_dry=
	local vdo_name=
	local vdo_verbose=

	vdo_stop_ "$@"
	while [ "$#" -ne 0 ]
	do
		case "$1" in
		  -d|--debug|--verbose) vdo_verbose="-v" ;;
		  -f|--confFile) shift; vdo_confFile=$1 ;;
		  -n|--name) shift; vdo_name=$1 ;;
		  --dry-run) vdo_dry=1 ;;
		  --force) ;;
		  *) vdo_die_ "Unknown option: $1" ;;
		esac
		shift
	done

	vdo_conf_remove_entry_ "$vdo_name" "$vdo_confFile" >"${vdo_confFile}.new"

	vdo_dry_ mv "${vdo_confFile}.new" "$vdo_confFile"
	grep -q "!VDOService" "$vdo_confFile" || vdo_dry_ rm -f "$vdo_confFile"
}


#
# print_config_file
#
vdo_print_config_file_() {
	local vdo_confFile=$LVM_VDO_DEFAULT_CONF

	while [ "$#" -ne 0 ]
	do
		case "$1" in
		  -d|--debug|--verbose) ;;
		  -f|--confFile) shift; vdo_confFile=$1 ;;
		  --logfile) shift ;;  # ignore
		  *) vdo_die_ "Unknown option: $1" ;;
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
	local vdo_device=
	local vdo_dry=
	local vdo_name=
	local vdo_opts=()
	local vdo_verbose=

	while [ "$#" -ne 0 ]
	do
		case "$1" in
		  -d|--debug|--verbose) vdo_verbose="-v" ;;
		  -f|--confFile) shift; vdo_confFile=$1 ;;
		  -n|--name) shift; vdo_name=$1 ;;
		  --check|--help|--version) vdo_opts+=("$1") ;;
		  --dry-run) vdo_dry=1 ;;
		  --force) ;; # required by real vdo convert; prepare ignores it
		  *) vdo_die_ "Unknown option: $1" ;;
		esac
		shift
	done

	vdo_device=$(vdo_conf_get_field_ "$vdo_name" "device" "$vdo_confFile")

	vdo_dry_ "$LVM_VDO_PREPARE" "${vdo_opts[@]}" "$vdo_device"
	vdo_dry_ vdo_remove_ -f "$vdo_confFile" -n "$vdo_name" || true
}

#
# MAIN
#
case "${1-}" in
  convert) shift; vdo_convert_ "$@" ;;
  create) shift; vdo_create_ "$@" ;;
  printConfigFile) shift; vdo_print_config_file_ "$@" ;;
  remove) shift; vdo_remove_ "$@" ;;
  stop) shift; vdo_stop_ "$@" ;;
  *) vdo_die_ "Unknown command: ${1-}" ;;
esac
