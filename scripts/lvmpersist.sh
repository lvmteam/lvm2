#!/bin/bash
#
# Copyright (C) 2024 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.

set -o pipefail

MISSING_DEV_COUNT=0

usage() {
	cat <<-EOF
	  ${SCRIPTNAME}: use persistent reservations on devices in an LVM VG.

	  ${SCRIPTNAME} start --ourkey KEY DEST
	      Register key and reserve device(s).

	  ${SCRIPTNAME} start --ourkey KEY --removekey REMKEY DEST
	      Register key and reserve device(s), replacing reservation holder by preempt-abort.

	  ${SCRIPTNAME} stop --ourkey KEY DEST
	      Unregister key, dropping reservation.

	  ${SCRIPTNAME} remove --ourkey KEY --removekey REMKEY DEST
	      Preempt-abort a key.

	  ${SCRIPTNAME} clear --ourkey KEY DEST
	      Release reservation and clear registered keys.

	  ${SCRIPTNAME} devtest DEST
	      Test if devices support PR.

	  ${SCRIPTNAME} check-key --key KEY DEST
	      Check if a key is registered.

	  ${SCRIPTNAME} read-keys DEST
	      Display registered keys.

	  ${SCRIPTNAME} read-reservation DEST
	      Display reservation.

	  ${SCRIPTNAME} read DEST
	      Display registered keys and reservation.

	  Options:
	      --ourkey KEY         KEY is the local key.
	      --removekey REMKEY   REMKEY is another host's key to remove.
	      --key KEY            KEY is any key to check.
	      --access ex|sh       Access type: ex (exclusive) for a local VG,
	                           sh (shared) for a shared VG.
	                           Translates to a specific PRTYPE for each device as appropriate
	                           (usually WE for ex and WEAR for sh.)
	      --prtype PRTYPE      Use only a specific PRTYPE, an alternative to --access.
	      --ptpl               Enable persist through power loss when starting.
	      --debug              Enable shell debugging.

	  DEST:
	      --device PATH ...    One or more devices to operate on.
	                           (Repeat this option to use multiple devices.)
	      --vg VGNAME          One VG to operate on. All PVs in the VG are used.
	                           (An lvm command is run to find all the PVs.)
	      --vg VGNAME --device PATH ...
	                           One or more devices to operate on, and a VG name
	                           to use as an identifier for the set of devices.

	  PRTYPE: persistent reservation type (use abbreviation with --prtype).
	      WE:   Write Exclusive
	      EA:   Exclusive Access
	      WERO: Write Exclusive  - registrants only (not yet supported)
	      EARO: Exclusive Access - registrants only (not yet supported)
	      WEAR: Write Exclusive  - all registrants
	      EAAR: Exclusive Access - all registrants

	EOF
}

# errorexit: invalid invocation (stderr only).  die: runtime failure
# (stderr and syslog).  logerror: non-fatal problem or warning.
errorexit() {
	printf '  %s: %s\n' "${SCRIPTNAME}" "$1" >&2
	exit 1
}

die() {
	logerror "$1"
	exit 1
}

# logger is best-effort: it may be missing (minimal or container
# environments), and a logger failure must not affect PR operations.

logerror() {
	printf '  %s: %s\n' "${SCRIPTNAME}" "$1" >&2
	logger "${SCRIPTNAME}: $1" >/dev/null 2>&1 || true
}

logmsg() {
	printf '  %s: %s\n' "${SCRIPTNAME}" "$1" >&2
	[[ "$DO_START" -eq 1 || "$DO_STOP" -eq 1 || "$DO_REMOVE" -eq 1 ||
	   "$DO_CLEAR" -eq 1 ]] && \
		logger "${SCRIPTNAME}: $1" >/dev/null 2>&1 || true
}

require_opt() {
	test -n "${!1-}" || errorexit "Missing required option: --$2."
}

# nvme commands
# register: nvme resv-register --nrkey=$OURKEY --rrega=0
# unregister: nvme resv-register --crkey=$OURKEY --rrega=1
# reserve: nvme resv-acquire --crkey=$OURKEY --rtype=$NVME_PRTYPE --racqa=0
# release: nvme resv-release --crkey=$OURKEY --rtype=$NVME_PRTYPE --rrela=0
# preempt-abort: nvme resv-acquire --crkey=$OURKEY --rtype=$NVME_PRTYPE --racqa=2
# clear: nvme resv-release --crkey=$OURKEY --rrela=1

set_cmd() {
	dev=$1
	case "$dev" in
	  /dev/nvme*)
		cmd="nvme"
		cmdopts=()
		;;
	  /dev/dm-*)
		;&
	  /dev/mapper/*)
		cmd="mpathpersist"
		cmdopts=()
		;;
	  *)
		cmd="sg_persist"
		cmdopts=("--no-inquiry")
		;;
	esac
}

# When using --access, the PR type used in the
# command depends on the dev type, i.e. mpath
# devs will use WEAR while scsi uses WE.

set_type() {
	dev=$1
	case "$dev" in
	  /dev/nvme*)
		type="$NVME_PRTYPE"
		type_str="$NVME_PRDESC"
		;;
	  /dev/dm-*)
		;&
	  /dev/mapper/*)
		type="$MPATH_PRTYPE"
		type_str="$MPATH_PRDESC"
		;;
	  *)
		type="$SCSI_PRTYPE"
		type_str="$SCSI_PRDESC"
		;;
	esac
}

# Return 0 if the key is found, 1 if not found, 2 if the
# query command itself failed (caller must not treat 2 the
# same as 1: the presence of the key could not be determined).

key_is_on_device() {
	local op FINDKEY FINDKEY_DEC

	dev=$1
	FINDKEY=$2
	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		FINDKEY_DEC=$(printf '%u' "$FINDKEY")
		op="resv-report"

		# jq -e: exit non-zero when the filter matches nothing (key absent).
		nvme resv-report --eds -o json "$dev" 2>/dev/null \
			| jq -e ".regctlext[] | select(.rkey == ${FINDKEY_DEC})" > /dev/null 2>&1 && return 0
	else
		op="read-keys"

		# grep with space to avoid matching the line "PR generation=0x..."
		# end-of-line matching required to avoid 0x123ab matching 0x123abc
		"$cmd" "${cmdopts[@]}" --in --read-keys "$dev" 2>/dev/null \
			| grep -q " $FINDKEY$" && return 0
	fi

	# Inspect PIPESTATUS immediately: nothing may run between the pipeline
	# above and here, since any command, even a plain assignment, resets it.
	# A successful command whose filter matched nothing means the key is
	# absent (jq -e exits 1 or 4, grep exits 1; grep never exits 4).
	case "${PIPESTATUS[0]}" in
	0)
		case "${PIPESTATUS[1]:-0}" in
		1|4) return 1 ;;
		*)
			logmsg "$cmd $op error on $dev"
			return 2
			;;
		esac
		;;
	*)
		logmsg "$cmd $op error on $dev"
		return 2
		;;
	esac
}

get_key_list_nvme() {
	local keys_str
	dev=$1
	set_cmd "$dev"

	KEYS=()

	# json/jq output is only decimal; xargs -r skips printf when there are no keys.
	# Do not use jq -e here: no registrants is success (empty output), not an error.

	keys_str=$(
		nvme resv-report --eds -o json "$dev" 2>/dev/null \
		| jq -r '.regctlext[].rkey' \
		| sort -n \
		| xargs -r printf '0x%x\n'
	) || {
		logmsg "$cmd read-keys error on $dev"
		KEYS=()
		return 1
	}

	if [[ -n $keys_str ]]; then
		mapfile -t KEYS <<< "$keys_str"
	fi
}

get_key_list_scsi() {
	local keys_str
	dev=$1
	set_cmd "$dev"

	KEYS=()

	if [[ "$cmd" == "mpathpersist" ]]; then
		no_keys_msg="0 registered reservation key"
	else
		no_keys_msg="there are NO registered reservation keys"
	fi

	"$cmd" "${cmdopts[@]}" --in --read-keys "$dev" 2>/dev/null \
		| grep -q "$no_keys_msg" && return

	# sort -u eliminates repeated keys listed with multipath
	# grep -oE (ERE): in basic regex -oe, '+' is literal and never matches.

	keys_str=$(
		"$cmd" "${cmdopts[@]}" --in --read-keys "$dev" 2>/dev/null \
		| grep "    0x" | grep -oE '0x[0-9a-fA-F]+' | sort -u
	) || {
		logmsg "$cmd read-keys error on $dev"
		KEYS=()
		return 1
	}

	if [[ -n $keys_str ]]; then
		mapfile -t KEYS <<< "$keys_str"
	fi
}

get_key_list() {
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		get_key_list_nvme "$dev"
	else
		get_key_list_scsi "$dev"
	fi
}

get_dev_reservation_holder_nvme() {
	dev=$1
	HOLDER=0

	# get rkey from the regctlext section with rcsts=1
	# jq without -e: no holder is an empty list, handled below (not a jq failure).

	str=$(nvme resv-report --eds -o json "$dev" 2>/dev/null \
		| jq -r '.regctlext | map(select(.rcsts == 1)) | .[].rkey' \
		| xargs -r printf '0x%x')
	if [ $? -ne 0 ]; then
		logmsg "nvme resv-report error on $dev"
		return 1
	fi

	if [[ -z $str ]]; then
		logmsg "nvme resv-report holder output not found $dev"
		return 1
	fi

	HOLDER=$str
}

get_dev_reservation_holder_scsi() {
	dev=$1
	HOLDER=0

	# combine with get_dev_reservation to
	# run a single sg_persist for holder and type?

	set_cmd "$dev"

	# Match the reservation key line case-insensitively (the key= format
	# differs between sg_persist and mpathpersist) and extract just the
	# hex key, so the HOLDER value is not polluted by ", scope: ...".
	# grep -oE (ERE): in basic regex -oe, '+' is literal and never matches.
	str=$("$cmd" "${cmdopts[@]}" --in --read-reservation "$dev" 2>/dev/null \
		| grep -ie "key\s*[:=]\s*0x" | grep -oE '0x[0-9a-fA-F]+')
	if [ $? -ne 0 ]; then
		if ! no_reservation_held "$dev"; then
			logmsg "$cmd read-reservation error on $dev"
		fi
		return 1
	fi

	# Take the first line here instead of piping through head -1: head
	# exits early and can SIGPIPE the upstream command, which pipefail
	# would then report as a failed query.  multipath may repeat the
	# key, and the reservation holder is the first.
	str=${str%%$'\n'*}

	if [[ -z $str ]]; then
		if ! no_reservation_held "$dev"; then
			logmsg "$cmd read-reservation holder output not found $dev"
		fi
		return 1
	fi

	HOLDER=$str
}

get_dev_reservation_holder() {
	dev=$1
	cur_type=$2
	HOLDER=0

	# holder is not relevant for WEAR/EAAR
	if [[ "$cur_type" == "WEAR" || "$cur_type" == "EAAR" ]]; then
		return
	fi

	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		get_dev_reservation_holder_nvme "$dev"
	else
		get_dev_reservation_holder_scsi "$dev"
	fi
}

get_dev_reservation_nvme() {
	dev=$1

	DEV_PRTYPE=0
	DEV_PRDESC=error

	str=$(nvme resv-report --eds -o json "$dev" 2>/dev/null | jq '.rtype')
	if [ $? -ne 0 ]; then
		logmsg "nvme resv-report error on $dev"
		return 1
	fi

	# jq prints null and exits 0 when rtype is missing; an
	# out-of-range value is equally unusable.  Both are a
	# per-device query failure, not a reason to abort the
	# whole command.
	if ! [[ "$str" =~ ^[0-9]$ ]]; then
		logmsg "nvme resv-report unexpected reservation type '$str' for $dev"
		return 1
	fi

	case "$str" in
	0)
		DEV_PRDESC=none
		false
		;;
	1)
		DEV_PRDESC=WE
		true
		;;
	2)
		DEV_PRDESC=EA
		true
		;;
	3)
		DEV_PRDESC=WERO
		true
		;;
	4)
		DEV_PRDESC=EARO
		true
		;;
	5)
		DEV_PRDESC=WEAR
		true
		;;
	6)
		DEV_PRDESC=EAAR
		true
		;;
	*)
		logmsg "nvme resv-report unknown reservation type '$str' for $dev"
		return 1
		;;
	esac

	DEV_PRTYPE="$str"
}

get_dev_reservation_scsi() {
	dev=$1
	set_cmd "$dev"

	str=$("$cmd" "${cmdopts[@]}" --in --read-reservation "$dev" 2>/dev/null | grep -e "LU_SCOPE,\s\+type")
	if [ $? -ne 0 ]; then
		if no_reservation_held "$dev"; then
			DEV_PRDESC=none
			DEV_PRTYPE=0
		else
			logmsg "$cmd read-reservation error on $dev"
			DEV_PRDESC=error
			DEV_PRTYPE=0
		fi
		return 1
	fi

	if [[ -z $str ]]; then
		if no_reservation_held "$dev"; then
			DEV_PRDESC=none
			DEV_PRTYPE=0
		else
			logmsg "$cmd read-reservation type output not found $dev"
			DEV_PRDESC=error
			DEV_PRTYPE=0
		fi
		return 1
	fi

	# Output format differs between commands:
	# sg_persist:   "scope: LU_SCOPE,  type: "
	# mpathpersist: "scope = LU_SCOPE, type = "

	if [[ "$str" == *"Exclusive Access, all registrants"* ]]; then
		# scsi type 8
		DEV_PRDESC=EAAR
		DEV_PRTYPE=8
		true
	elif [[ "$str" == *"Write Exclusive, all registrants"* ]]; then
		# scsi type 7
		DEV_PRDESC=WEAR
		DEV_PRTYPE=7
		true
	elif [[ "$str" == *"Exclusive Access, registrants only"* ]]; then
		# scsi type 6
		DEV_PRDESC=EARO
		DEV_PRTYPE=6
		true
	elif [[ "$str" == *"Write Exclusive, registrants only"* ]]; then
		# scsi type 5
		DEV_PRDESC=WERO
		DEV_PRTYPE=5
		true
	elif [[ "$str" == *"Exclusive Access"* ]]; then
		# scsi type 3
		DEV_PRDESC=EA
		DEV_PRTYPE=3
		true
	elif [[ "$str" == *"Write Exclusive"* ]]; then
		# scsi type 1
		DEV_PRDESC=WE
		DEV_PRTYPE=1
		true
	else
		DEV_PRDESC=unknown
		DEV_PRTYPE=0
		false
	fi
}

# Set DEV_PRDESC and DEV_PRTYPE to whatever is
# currently found on the device arg.

get_dev_reservation() {
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		get_dev_reservation_nvme "$dev"
	else
		get_dev_reservation_scsi "$dev"
	fi
}

no_reservation_held_nvme() {
	dev=$1

	get_dev_reservation_nvme "$dev"

	if [[ "$DEV_PRDESC" == "none" ]]; then
		true
		return
	fi

	false
	return
}

no_reservation_held_scsi() {
	dev=$1

	# sg_persist and mpathpersist word the message differently and with
	# different capitalization, so match case-insensitively.
	"$cmd" "${cmdopts[@]}" --in --read-reservation "$dev" 2>/dev/null \
		| grep -qi "no reservation held"
}

no_reservation_held() {
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		no_reservation_held_nvme "$dev"
	else
		no_reservation_held_scsi "$dev"
	fi
}

device_supports_type_str_nvme() {
	dev=$1
	str=${2-}

	case "$str" in
	"")
		;&
	WE|EA|WERO|EARO|WEAR|EAAR)
		;;
	*)
		logmsg "unknown type string (choose WE/EA/WERO/EARO/WEAR/EAAR)."
		return 1
		;;
	esac

	# NVMe has no per-type report-capabilities output like SCSI.  When
	# resv-report succeeds, the namespace supports persistent reservations
	# and the standard reservation types (1-6) map to the WE..EAAR strings.
	if ! nvme resv-report --eds "$dev" > /dev/null 2>&1; then
		logmsg "nvme resv-report error on $dev"
		return 2
	fi

	return 0
}

device_supports_type_str_scsi() {
	dev=$1
	str=$2

	case "$str" in
	WE)
		SUPPORTED="Write Exclusive: 1"
		;;
	EA)
		SUPPORTED="Exclusive Access: 1"
		;;
	WERO)
		SUPPORTED="Write Exclusive, registrants only: 1"
		;;
	EARO)
		SUPPORTED="Exclusive Access, registrants only: 1"
		;;
	WEAR)
		SUPPORTED="Write Exclusive, all registrants: 1"
		;;
	EAAR)
		SUPPORTED="Exclusive Access, all registrants: 1"
		;;
	*)
		logmsg "unknown type string (choose WE/EA/WERO/EARO/WEAR/EAAR)."
		false
		return
		;;
	esac

	# Do not set_cmd here because for report-capabilities,
	# sg_persist works on mpath devs, but mpathpersist doesn't work.

	if sg_persist --in --report-capabilities "$dev" 2>/dev/null | grep -q "${SUPPORTED}"; then
		true
		return
	fi

	# PIPESTATUS[1] is grep's exit: 1 just means the type is not supported,
	# which is the normal "no" answer, not a command error.
	if [[ "${PIPESTATUS[0]}" -ne "0" ]]; then
		logmsg "sg_persist report-capabilities error on $dev"
		return 2
	fi

	false
	return
}

device_supports_type_str() {
	dev=$1
	str=$2
	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		device_supports_type_str_nvme "$dev" "$str"
	else
		device_supports_type_str_scsi "$dev" "$str"
	fi
}

device_supports_pr() {
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		device_supports_type_str_nvme "$dev"
	else
		sg_persist --in --report-capabilities "$dev" >/dev/null 2>&1
	fi
}

check_devices() {
	err=0
	FOUND_MPATH=0
	FOUND_SCSI=0
	FOUND_NVME=0

	# Reject non-block paths up front: the rest of the script (and the
	# PR tools) assumes each entry is a real device, otherwise they fail
	# with a less clear error.
	for dev in "${DEVICES[@]}"; do
		test -b "$dev" || errorexit "not a block device: $dev."
	done

	for dev in "${DEVICES[@]}"; do
		case "$dev" in
		/dev/nvme*)
			FOUND_NVME=1
			;;
		/dev/sd*)
			FOUND_SCSI=1
			;;
		/dev/dm-*)
			;&
		/dev/mapper*)
			MAJORMINOR=$("$DMSETUP" info --noheadings -c -o major,minor "$dev")
			case "$MAJORMINOR" in
			  *[!0-9:]*|"") die "unexpected dmsetup output for $dev" ;;
			esac
			DM_UUID=
			read -r DM_UUID 2>/dev/null <"/sys/dev/block/$MAJORMINOR/dm/uuid"
			if [[ $DM_UUID == *"mpath-"* ]]; then
				FOUND_MPATH=1
			else
				logmsg "device $dev dm uuid does not appear to be multipath ($DM_UUID)"
				err=1
			fi
			;;
		*)
			logmsg "device type not supported for $dev."
			err=1
		esac
	done

	test "$err" -eq 0 || errorexit "unsupported or invalid device(s)."

	if [[ $FOUND_MPATH -eq 1 ]]; then
		command -v mpathpersist > /dev/null || die "mpathpersist command not found."
		if ! grep "reservation_key file" /etc/multipath.conf > /dev/null 2>&1; then
			echo "To use persistent reservations with multipath, run:"
			echo "  mpathconf --option reservation_key:file"
			echo "to configure multipath.conf, and then restart multipathd."
		fi
	fi

	# sg_persist is used for report-capabilities on both scsi and
	# multipath devices, and sg_turs is run on both to clear unit
	# attention errors.
	if [[ $FOUND_SCSI -eq 1 || $FOUND_MPATH -eq 1 ]]; then
		command -v sg_persist > /dev/null || die "sg_persist command not found."
		command -v sg_turs > /dev/null || die "sg_turs command not found."
	fi

	if [[ $FOUND_NVME -eq 1 ]]; then
		command -v nvme > /dev/null || die "nvme command not found."
		# jq >= 1.7 is required: it represents 64-bit PR keys exactly.
		# jq <= 1.6, and 1.7+ built with --disable-decnum, use IEEE-754
		# doubles: keys above 2^53 lose precision and distinct keys can
		# compare equal.
		command -v jq > /dev/null || die "jq command not found."
	fi

	# Sometimes a device will return a Unit Attention error
	# for an sg_persist/mpathpersist command, e.g. after the
	# host's key was cleared.  A single tur command clears
	# the error.  Alternatively, each command in the script
	# could be retried if it fails due to a UA error.

	for dev in "${DEVICES[@]}"; do
		case "$dev" in
		/dev/sd*)
			;&
		/dev/dm-*)
			;&
		/dev/mapper/*)
			sg_turs "$dev" >/dev/null 2>&1
			ec=$?
			test $ec -eq 0 || logmsg "test unit ready error $ec from $dev"
		esac
	done
}

# udevadm is best-effort: if it is missing or settle fails, report the
# problem and continue.  The wait only reduces the chance of an LIO
# deadlock during start/takeover (see do_takeover/do_start); it is not
# required for correctness.
# TODO: consider failing hard when udevadm is unavailable or settle
# fails, if this turns out to be a real point of failure in the field.
settle_udev() {
	if ! command -v udevadm > /dev/null; then
		logmsg "udevadm not found: cannot wait for udev before reserving $GROUP."
		return 0
	fi
	udevadm settle || logmsg "failed to settle udev events before reserving $GROUP."
}

undo_register() {
	# Only devices where our key was actually registered are undone, so
	# an interrupt or a failure before any registration touches nothing.
	for dev in "${REGISTERED_DEVICES[@]}"; do
		set_cmd "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-register --crkey="$OURKEY" --rrega=1 "$dev" >/dev/null 2>&1
		else
			"$cmd" "${cmdopts[@]}" --out --register --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi
		if [ $? -ne 0 ]; then
			logmsg "$cmd unregister error on $dev"
		fi
	done
}

do_register_nvme() {
	dev=$1
	set_cmd "$dev"

	if [[ $PTPL -eq 1 ]]; then
		cmdopts+=("--cptpl=3")
	fi

	# If our previous key is still registered, then we must use
	# rrega=2 and iekey.  If our previous key has been removed,
	# then we must use rrega=0.

	if ! nvme resv-register "${cmdopts[@]}" --nrkey="$OURKEY" --rrega=0 "$dev" >/dev/null 2>&1; then
		if ! nvme resv-register "${cmdopts[@]}" --nrkey="$OURKEY" --rrega=2 --iekey "$dev" >/dev/null 2>&1; then
			logmsg "$cmd register error on $dev"
			return 1
		fi
	fi
}

do_register_scsi() {
	dev=$1
	set_cmd "$dev"

	if [[ $PTPL -eq 1 ]]; then
		cmdopts+=("--param-aptpl")
	fi

	if ! "$cmd" "${cmdopts[@]}" --out --register-ignore --param-sark="$OURKEY" "$dev" >/dev/null 2>&1; then
		logmsg "$cmd register error on $dev"
		return 1
	fi
}

do_register() {
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		do_register_nvme "$dev"
	else
		do_register_scsi "$dev"
	fi
	# Only record a device our key was successfully registered on.
	if [ $? -ne 0 ]; then
		return 1
	fi

	REGISTERED_DEVICES+=("$dev")
}

do_takeover() {

	require_opt OURKEY ourkey
	require_opt REMKEY removekey

	err=0

	for dev in "${DEVICES[@]}"; do
		set_type "$dev"
		device_supports_type_str "$dev" "$type_str"
		rc=$?
		if [ "$rc" -ne 0 ]; then
			if [ "$rc" -eq 2 ]; then
				logmsg "start $GROUP $dev failed to query reservation type $type_str."
			else
				logmsg "start $GROUP $dev does not support reservation type $type_str."
			fi
			err=1
		fi
	done

	if [ "$err" -ne 0 ]; then
		errorexit "start $GROUP failed."
	fi

	for dev in "${DEVICES[@]}"; do
		key_is_on_device "$dev" "$REMKEY"
		rc=$?
		if [ "$rc" -eq 1 ]; then
			die "start $GROUP specified key to remove $REMKEY not found on $dev."
		elif [ "$rc" -eq 2 ]; then
			die "start $GROUP failed to check for key $REMKEY on $dev."
		fi
	done

	# Register our key

	for dev in "${DEVICES[@]}"; do
		if ! do_register "$dev"; then
			logmsg "start $GROUP failed to register our key."
			undo_register
			exit 1
		fi
	done

	# The register above triggers udev to re-probe the device (blkid,
	# scsi_id).  Those probing reads share the iSCSI connection with
	# the preempt-abort below.  If probing is still in-flight when the
	# preempt-abort arrives at the LIO target, the target deadlocks in
	# core_tmr_drain_state_list (waiting for in-flight reads) vs
	# iscsit_close_connection (waiting for RX thread exit).
	settle_udev

	# Reserve the device

	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"
		set_type "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-acquire --crkey="$OURKEY" --prkey="$REMKEY" --rtype="$type" --racqa=2 "$dev" >/dev/null 2>&1
		else
			"$cmd" "${cmdopts[@]}" --out --preempt-abort --param-sark="$REMKEY" --param-rk="$OURKEY" --prout-type="$type" "$dev" >/dev/null 2>&1
		fi

		if [[ "$?" -ne 0 ]]; then
			logmsg "start $GROUP failed to preempt-abort $REMKEY on $dev."
			undo_register
			exit 1
		fi
	done

	logmsg "started $GROUP with key $OURKEY."
	exit 0
}

do_start() {
	err=0

	require_opt OURKEY ourkey

	for dev in "${DEVICES[@]}"; do
		set_type "$dev"
		device_supports_type_str "$dev" "$type_str"
		rc=$?
		if [ "$rc" -ne 0 ]; then
			if [ "$rc" -eq 2 ]; then
				logmsg "start $GROUP $dev failed to query reservation type $type_str."
			else
				logmsg "start $GROUP $dev does not support reservation type $type_str."
			fi
			err=1
		fi
	done

	if [ "$err" -ne 0 ]; then
		errorexit "start $GROUP failed."
	fi

	# Register our key on devices

	for dev in "${DEVICES[@]}"; do
		if ! do_register "$dev"; then
			logmsg "start $GROUP failed to register our key."
			undo_register
			exit 1
		fi
	done

	# The register above triggers udev to re-probe the device.
	# Wait for probing to finish before issuing the reservation
	# to avoid the LIO deadlock (same issue fixed in do_takeover).
	settle_udev

	# Reserve devices

	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"
		set_type "$dev"

		# For type WEAR/EAAR, once it's acquired (by the first
		# host), it cannot be acquired again by other hosts
		# (the command fails for nvme), so if WEAR/EAAR is
		# requested, first check if that reservation type already
		# exists.

		if [[ "$type_str" == "WEAR" || "$type_str" == "EAAR" ]]; then
			get_dev_reservation "$dev"
			if [[ "$DEV_PRDESC" == "$type_str" ]]; then
				continue
			fi
		fi

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-acquire --crkey="$OURKEY" --rtype="$type" --racqa=0 "$dev" >/dev/null 2>&1
		else
			"$cmd" "${cmdopts[@]}" --out --reserve --param-rk="$OURKEY" --prout-type="$type" "$dev" >/dev/null 2>&1
		fi

		if [[ "$?" -ne 0 ]]; then
			# For WEAR/EAAR, another host may have acquired the
			# reservation between our check and our acquire attempt.
			# Re-check: if the reservation now exists, that's fine.
			if [[ "$type_str" == "WEAR" || "$type_str" == "EAAR" ]]; then
				get_dev_reservation "$dev"
				if [[ "$DEV_PRDESC" == "$type_str" ]]; then
					continue
				fi
			fi
			logmsg "start $GROUP failed to reserve $dev."
			undo_register
			exit 1
		fi
	done

	logmsg "started $GROUP with key $OURKEY."
	exit 0
}

do_stop() {
	err=0

	require_opt OURKEY ourkey

	# Removing reservation is not needed, we just remove our registration key.
	# The reservation will go away when the last key is removed.
	# sg_persist --out --no-inquiry --release --param-rk=${OURKEY} --prout-type=$SCSI_PRTYPE

	for dev in "${DEVICES[@]}"; do
		# Remove our registration key, we will no longer be able to write
		set_cmd "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-register --crkey="$OURKEY" --rrega=1 "$dev" >/dev/null 2>&1
		else
			"$cmd" "${cmdopts[@]}" --out --register --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi

		# test $? -eq 0 || logmsg "$cmd unregister error on $dev"

		key_is_on_device "$dev" "$OURKEY"
		rc=$?
		if [ "$rc" -eq 0 ]; then
			logmsg "stop $GROUP failed to unregister our key $OURKEY from $dev."
			err=1
		elif [ "$rc" -eq 2 ]; then
			logmsg "stop $GROUP failed to verify our key $OURKEY was unregistered from $dev."
			err=1
		fi
	done

	if [ "$err" -ne 0 ]; then
		errorexit "stop $GROUP failed."
	fi

	logmsg "stopped $GROUP with key $OURKEY."
	exit 0
}

do_clear() {
	local err=0

	require_opt OURKEY ourkey

	# our key must be registered to do clear.
	# we want to clear any/all PR state that we can find on the devs,
	# so just skip any devs that we cannot register with, and clear
	# what we can.

	# Only PR-capable devices where our key is registered (or we registered
	# it below) are cleared; skip unsupported devices and any we cannot
	# register on or query.
	CLEAR_DEVICES=()

	for dev in "${DEVICES[@]}"; do
		if ! device_supports_pr "$dev"; then
			logerror "Device $dev: does not support PR"
			continue
		fi

		key_is_on_device "$dev" "$OURKEY"
		rc=$?
		if [ "$rc" -eq 0 ]; then
			CLEAR_DEVICES+=("$dev")
		elif [ "$rc" -eq 1 ]; then
			if do_register "$dev"; then
				CLEAR_DEVICES+=("$dev")
			else
				logmsg "clear $GROUP skip $dev without registration"
			fi
		else
			logmsg "clear $GROUP failed to check for our key $OURKEY on $dev."
			err=1
		fi
	done

	# clear releases the reservation and clears all registrations
	for dev in "${CLEAR_DEVICES[@]}"; do
		set_cmd "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-release --crkey="$OURKEY" --rrela=1 "$dev" >/dev/null 2>&1
		else
			"$cmd" "${cmdopts[@]}" --out --clear --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi

		test $? -eq 0 || logmsg "$cmd clear error on $dev"

		# Real result is whether the dev now has no registrations and
		# reservation.

		if ! get_key_list "$dev"; then
			logmsg "clear $GROUP failed to read keys from $dev"
			err=1
		elif [[ ${#KEYS[@]} -gt 0 ]]; then
			logmsg "clear $GROUP keys not cleared from $dev - ${KEYS[*]}"
			err=1
		fi

		if ! no_reservation_held "$dev"; then
			logmsg "clear $GROUP reservation not cleared from $dev"
			err=1
		fi
	done

	if [ "$err" -ne 0 ]; then
		errorexit "clear $GROUP failed."
	fi

	logmsg "cleared $GROUP reservation and keys"
	exit 0
}

do_remove() {
	err=0

	require_opt OURKEY ourkey
	require_opt REMKEY removekey

	for dev in "${DEVICES[@]}"; do
		key_is_on_device "$dev" "$OURKEY"
		rc=$?
		if [ "$rc" -eq 1 ]; then
			logmsg "cannot remove $REMKEY from $dev without ourkey $OURKEY being registered"
			err=1
			continue
		elif [ "$rc" -eq 2 ]; then
			logmsg "cannot remove $REMKEY from $dev, failed to check ourkey $OURKEY."
			err=1
			continue
		fi

		set_cmd "$dev"
		# Use the current reservation type when one is held; otherwise use
		# the configured type (--access or --prtype) for preempt-abort.
		get_dev_reservation "$dev"
		if [[ "$DEV_PRDESC" == "error" ]]; then
			logmsg "cannot remove $REMKEY from $dev, failed to read reservation type."
			err=1
			continue
		fi

		remove_type=$DEV_PRTYPE
		if [[ "$DEV_PRDESC" == "none" ]]; then
			set_type "$dev"
			remove_type=$type
		fi

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-acquire --crkey="$OURKEY" --prkey="$REMKEY" --rtype="$remove_type" --racqa=2 "$dev" >/dev/null 2>&1
		else
			"$cmd" "${cmdopts[@]}" --out --preempt-abort --param-sark="$REMKEY" --param-rk="$OURKEY" --prout-type="$remove_type" "$dev" >/dev/null 2>&1
		fi

		test $? -eq 0 || logmsg "$cmd preempt-abort error on $dev"

		key_is_on_device "$dev" "$REMKEY"
		rc=$?
		if [ "$rc" -eq 0 ]; then
			logmsg "failed to remove key $REMKEY from $dev in $GROUP."
			err=1
		elif [ "$rc" -eq 2 ]; then
			logmsg "failed to verify key $REMKEY was removed from $dev in $GROUP."
			err=1
		fi
	done

	# Fencing (remove) requires removing the target's PR key from all
	# PV devices.  If any device is missing we cannot fully fence the
	# target: it may retain its key on the missing device and still do
	# I/O there.  Remove the key from available devices above, but
	# still fail.
	# TODO: --allow-missing option may be added for cases where an
	# admin knows a missing device is physically destroyed.
	if [[ "$MISSING_DEV_COUNT" -gt 0 ]]; then
		logmsg "remove failed: $MISSING_DEV_COUNT missing device(s) in VG $VGNAME."
		err=1
	fi

	test "$err" -eq 0 || errorexit "remove $GROUP failed."

	logmsg "removed key $REMKEY for $GROUP."
	exit 0
}

do_devtest() {
	err=0

	for dev in "${DEVICES[@]}"; do
		set_type "$dev"

		device_supports_type_str "$dev" "$type_str"
		rc=$?
		if [ "$rc" -eq 0 ]; then
			echo "Device $dev: supports type $type_str"
		elif [ "$rc" -eq 2 ]; then
			logerror "Device $dev: failed to query type $type_str"
			err=1
		else
			logerror "Device $dev: does not support type $type_str"
			err=1
		fi
	done

	if [ "$err" -ne 0 ]; then
		errorexit "devtest failed."
	fi

	exit 0
}

do_checkkey() {
	err=0

	for dev in "${DEVICES[@]}"; do
		key_is_on_device "$dev" "$OURKEY"
		rc=$?
		if [ "$rc" -eq 0 ]; then
			echo "Device $dev: has key $OURKEY"
		elif [ "$rc" -eq 2 ]; then
			logerror "Device $dev: failed to check for key $OURKEY"
			err=1
		else
			logerror "Device $dev: does not have key $OURKEY"
			err=1
		fi
	done

	if [ "$err" -ne 0 ]; then
		errorexit "check-key failed."
	fi

	exit 0
}

do_readkeys() {
	local err=0

	for dev in "${DEVICES[@]}"; do
		set_type "$dev"
		device_supports_type_str "$dev" "$type_str"
		rc=$?
		if [ "$rc" -eq 1 ]; then
			echo "Device $dev: does not support PR"
			continue
		elif [ "$rc" -eq 2 ]; then
			logerror "Device $dev: failed to query reservation type"
			err=1
			continue
		fi
		if ! get_key_list "$dev"; then
			logerror "Device $dev: failed to read registered keys"
			err=1
		elif [[ ${#KEYS[@]} -eq 0 ]]; then
			echo "Device $dev: registered keys: none"
		else
			echo "Device $dev: registered keys: ${KEYS[*]}"
		fi
	done

	if [ "$err" -ne 0 ]; then
		errorexit "read-keys failed."
	fi
}

do_readreservation() {
	local err=0

	for dev in "${DEVICES[@]}"; do
		set_type "$dev"
		device_supports_type_str "$dev" "$type_str"
		rc=$?
		if [ "$rc" -eq 1 ]; then
			echo "Device $dev: does not support PR"
			continue
		elif [ "$rc" -eq 2 ]; then
			logerror "Device $dev: failed to query reservation type"
			err=1
			continue
		fi
		get_dev_reservation "$dev"
		if [[ "$DEV_PRDESC" == "error" ]]; then
			logerror "Device $dev: failed to read reservation"
			err=1
		elif [[ "$DEV_PRDESC" == "none" ]]; then
			echo "Device $dev: reservation: none"
		elif [[ "$DEV_PRDESC" == "WEAR" || "$DEV_PRDESC" == "EAAR" ]]; then
			echo "Device $dev: reservation: $DEV_PRDESC"
		elif ! get_dev_reservation_holder "$dev" "$DEV_PRDESC"; then
			logerror "Device $dev: failed to read reservation holder"
			err=1
		else
			echo "Device $dev: reservation: $DEV_PRDESC holder $HOLDER"
		fi
	done

	if [ "$err" -ne 0 ]; then
		errorexit "read-reservation failed."
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
		if [ $(( 0$MODE & 022 )) -ne 0 ]; then
			if ! test -d "$NODE" || [ $(( 0$MODE & 01000 )) -eq 0 ]; then
				errorexit "$NAME \"$NODE\" must not be group or world writable."
			fi
		fi
		test "$NODE" = "/" && break
		NODE=${NODE%/*}
		test -n "$NODE" || NODE=/
	done
}

validate_override() {
	local OPATH VAL

	VAL=${!1-}
	test -z "$VAL" && return 0
	test "${VAL#/}" != "$VAL" ||
		errorexit "$1 must be an absolute path."

	OPATH=$(readlink -f "$VAL") ||
		errorexit "$1 \"$VAL\" must be accessible and owned by root."

	validate_path "$1" "$OPATH"

	if ! test -f "$OPATH" || ! test -x "$OPATH"; then
		errorexit "$1 \"$OPATH\" must be an executable file."
	fi

	# Run the validated canonical path, not the original one, so that a
	# symlink cannot be repointed at a different binary after this check.
	printf -v "$1" '%s' "$OPATH"
}

#
# BEGIN SCRIPT
#
PATH="/sbin:/usr/sbin:/bin:/usr/bin"
SCRIPTNAME=$(basename "$0")

if [ $# -lt 1 ]; then
	usage
	exit 0
fi

# user may override lvm and dmsetup location by setting LVM_BINARY
# and DMSETUP_BINARY; overrides must be root-owned executables
validate_override DMSETUP_BINARY
validate_override LVM_BINARY

DMSETUP=${DMSETUP_BINARY:-dmsetup}
LVM=${LVM_BINARY:-lvm}

DO_START=0
DO_STOP=0
DO_REMOVE=0
DO_CLEAR=0
DO_DEVTEST=0
DO_CHECKKEY=0
DO_READKEYS=0
DO_READRESERVATION=0
DO_READ=0

# Records the devices where our key was actually registered, so the
# signal cleanup can undo exactly those and never touches (or warns
# about) a device we did not register on.
REGISTERED_DEVICES=()

CMD=$1
shift

case "$CMD" in
	start)
		DO_START=1
		;;
	stop)
		DO_STOP=1
		;;
	remove)
		DO_REMOVE=1
		;;
	clear)
		DO_CLEAR=1
		;;
	devtest)
		DO_DEVTEST=1
		;;
	check-key)
		DO_CHECKKEY=1
		;;
	read-keys)
		DO_READKEYS=1
		;;
	read-reservation)
		DO_READRESERVATION=1
		;;
	read)
		DO_READ=1
		;;
	help|-h|--help)
		usage
		exit 0
		;;
	*)
		errorexit "Unknown command: $CMD."
		;;
esac

if [ "$UID" != 0 ] && [ "$EUID" != 0 ] && [ "$CMD" != "help" ]; then
	errorexit "must be run as root."
fi

GETOPT="getopt"

OPTIONS=$("$GETOPT" -o h -l help,ourkey:,removekey:,key:,prtype:,access:,ptpl,debug,device:,vg: -n "${SCRIPTNAME}" -- "$@") ||
	errorexit "invalid option."
eval set -- "$OPTIONS"

while true
do
	case "$1" in
	--ourkey)
		OURKEY=$2; shift
		;;
	--key)
		KEY=$2; shift
		;;
	--removekey)
		REMKEY=$2; shift
		;;
	--ptpl)
		PTPL=1
		;;
	--access)
		ACCESS=$2; shift
		;;
	--prtype)
		PRTYPE_ARG=$2; shift
		;;
	--device)
		LAST_DEVICE=$2
		DEVICES+=("$LAST_DEVICE"); shift
		;;
	--vg)
		VGNAME=$2; shift
		;;
	--debug)
		set -x
		;;
	-h|--help)
		usage
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
	shift
done

#
# Missing required options
#

if [[ -z "$LAST_DEVICE" && -z "$VGNAME" ]]; then
	errorexit "Missing required option: --vg or --device."
fi

if [[ -n "$PRTYPE_ARG" && -n "$ACCESS" ]]; then
	errorexit "Set --prtype or --access, not both."
fi

if [[ "$DO_CHECKKEY" -eq 1 ]]; then
	require_opt KEY key
fi

if [[ "$DO_CHECKKEY" -eq 0 && -n "$KEY" ]]; then
	errorexit "Invalid option: --key."
fi

if [[ "$DO_CHECKKEY" -eq 1 ]]; then
	OURKEY="$KEY"
fi

# Verify valid digits in keys.
# Convert hex keys to lowercase (to match output of sg_persist)
# Convert decimal keys (without 0x prefix) to hex strings with 0x prefix.
# Leading 0s are not allowed because sg_persist drops them in output, so
# subsequent string matching of keys fails.

DECDIGITS='^[0-9]+$'
HEXDIGITS='^[0-9a-fA-F]+$'

# Decimal keys are converted with shell arithmetic, which is 64-bit and
# wraps silently above 2^64-1 (e.g. 2^64 becomes 0x0).  Reject values
# that do not fit so we never register a key other than the one asked
# for.  Leading zeros are insignificant and stripped for the range test.
key_from_decimal() {
	local dec=${1#"${1%%[!0]*}"}

	dec=${dec:-0}

	# Compare as strings, not integers: an out-of-range value would
	# overflow the very arithmetic used by -gt.  Both sides are
	# equal-length digit strings without leading zeros, so lexicographic
	# order matches numeric order here.
	# shellcheck disable=SC2071
	if [[ ${#dec} -gt 20 ||
	      ( ${#dec} -eq 20 && "$dec" > "18446744073709551615" ) ]]; then
		return 1
	fi

	# Force base-10 before printf %x: shell arithmetic reads a leading
	# zero as octal (010 -> 8).  Leading zeros are stripped above, but
	# keep the explicit base so the conversion does not depend on that.
	printf '0x%x' "$((10#$dec))"
}

if [[ -n "$OURKEY" && "$OURKEY" != "0x"* ]]; then
	if [[ "$OURKEY" =~ $DECDIGITS ]]; then
		key_hex=$(key_from_decimal "$OURKEY") ||
			errorexit "Key is out of range: $OURKEY"
		OURKEY=$key_hex
		if [[ -n "$KEY" ]]; then
			echo "Using key: $OURKEY"
		else
			echo "Using ourkey: $OURKEY"
		fi
	else
		errorexit "Invalid decimal digits in key: $OURKEY (use 0x prefix for hex key)"
	fi
fi

if [[ -n "$OURKEY" && "$OURKEY" == "0x"* ]]; then
	if [[ ! "${OURKEY:2}" =~ $HEXDIGITS ]]; then
		errorexit "Invalid hex digits in key: $OURKEY"
	fi
	# PR keys are 64-bit; a longer hex key cannot be represented
	# and the tools would truncate or saturate it to a different key.
	if [[ ${#OURKEY} -gt 18 ]]; then
		errorexit "Key is out of range: $OURKEY"
	fi
	OURKEY="${OURKEY,,}"
fi

if [[ -n "$REMKEY" && "$REMKEY" != "0x"* ]]; then
	if [[ "$REMKEY" =~ $DECDIGITS ]]; then
		key_hex=$(key_from_decimal "$REMKEY") ||
			errorexit "Key is out of range: $REMKEY"
		REMKEY=$key_hex
		echo "Using removekey: $REMKEY"
	else
		errorexit "Invalid decimal digits in key: $REMKEY (use 0x prefix for hex key)"
	fi
fi

if [[ -n "$REMKEY" && "$REMKEY" == "0x"* ]]; then
	if [[ ! "${REMKEY:2}" =~ $HEXDIGITS ]]; then
		errorexit "Invalid hex digits in key: $REMKEY"
	fi
	# PR keys are 64-bit; a longer hex key cannot be represented
	# and the tools would truncate or saturate it to a different key.
	if [[ ${#REMKEY} -gt 18 ]]; then
		errorexit "Key is out of range: $REMKEY"
	fi
	REMKEY="${REMKEY,,}"
fi

# Reject leading-zero hex keys (e.g. 0x01 or decimal 0 -> 0x0).
if [[ -n "$OURKEY" && "$OURKEY" == "0x0"* ]]; then
	errorexit "Leading 0s are not permitted in keys."
fi

if [[ -n "$REMKEY" && "$REMKEY" == "0x0"* ]]; then
	errorexit "Leading 0s are not permitted in keys."
fi

if [[ -z "$PRTYPE_ARG" && -z "$ACCESS" ]]; then
	ACCESS="ex"
fi

# When --access is set, the actual PR type is set
# according to the device type (mpath needs to use
# WEAR when others use WE.)

if [[ -n "$ACCESS" ]]; then
	# ex: scsi, nvme use WE; mpath uses WEAR
	# sh: scsi, nvme, mpath all use WEAR

	if [[ "$ACCESS" == "ex" ]]; then
		SCSI_PRTYPE=1
		SCSI_PRDESC=WE
		NVME_PRTYPE=1
		NVME_PRDESC=WE
		MPATH_PRTYPE=7
		MPATH_PRDESC=WEAR
	elif [[ "$ACCESS" == "sh" ]]; then
		SCSI_PRTYPE=7
		SCSI_PRDESC=WEAR
		NVME_PRTYPE=5
		NVME_PRDESC=WEAR
		MPATH_PRTYPE=7
		MPATH_PRDESC=WEAR
	else
		errorexit "Invalid access mode (use ex or sh)."
	fi
fi

# When --prtype is set, all device types use the
# specified type.

if [[ -n "$PRTYPE_ARG" ]]; then
	case "$PRTYPE_ARG" in
	WE)
		# Write Exclusive
		SCSI_PRTYPE=1
		MPATH_PRTYPE=1
		NVME_PRTYPE=1
		;;
	EA)
		# Exclusive Access
		SCSI_PRTYPE=3
		MPATH_PRTYPE=3
		NVME_PRTYPE=2
		;;
	WERO)
		# Write Exclusive - registrants only
		SCSI_PRTYPE=5
		MPATH_PRTYPE=5
		NVME_PRTYPE=3
		# TODO: figure out the model of usage when
		# the reservation holder goes away.
		errorexit "WERO is not yet supported."
		;;
	EARO)
		# Exclusive Access - registrants only
		SCSI_PRTYPE=6
		MPATH_PRTYPE=6
		NVME_PRTYPE=4
		# TODO: figure out the model of usage when
		# the reservation holder goes away.
		errorexit "EARO is not yet supported."
		;;
	WEAR)
		# Write Exclusive - all registrants
		SCSI_PRTYPE=7
		MPATH_PRTYPE=7
		NVME_PRTYPE=5
		;;
	EAAR)
		# Exclusive Access - all registrants
		SCSI_PRTYPE=8
		MPATH_PRTYPE=8
		NVME_PRTYPE=6
		;;
	*)
		errorexit "Unknown PRTYPE string (choose WE/EA/WERO/EARO/WEAR/EAAR)."
		;;
	esac

	SCSI_PRDESC="$PRTYPE_ARG"
	NVME_PRDESC="$PRTYPE_ARG"
	MPATH_PRDESC="$PRTYPE_ARG"
fi

#
# Set devices
#

# Add a --devicesfile option that can be used for this vgs command?
get_devices_from_vg() {
	local IFS=:
	local ALL_DEVS
	# Split on ':' without pathname expansion, so a PV name containing
	# a glob character is not expanded against the filesystem.
	set -f
	# shellcheck disable=SC2207 # intentional split of device list
	if ! ALL_DEVS=( $("$LVM" vgs --nolocking --noheadings --separator : --sort pv_uuid --o pv_name --rows --config log/prefix=\"\" "$VGNAME") ); then
		die "failed to get devices from VG $VGNAME."
	fi
	set +f

	DEVICES=()
	MISSING_DEV_COUNT=0
	for dev in "${ALL_DEVS[@]}"; do
		if [[ "$dev" == "/dev/"* ]]; then
			DEVICES+=("$dev")
		else
			logmsg "missing device $dev in VG $VGNAME."
			MISSING_DEV_COUNT=$((MISSING_DEV_COUNT + 1))
		fi
	done
}

if [[ -z "$LAST_DEVICE" && -n "$VGNAME" ]]; then
	get_devices_from_vg
fi

FIRST_DEVICE="${DEVICES[0]}"

if [[ -z "$FIRST_DEVICE" ]]; then
	errorexit "Missing required --vg or --device."
fi

# Prefix some log messages with VGNAME, or if no VGNAME is set,
# use "sda" (for one device), or "sda:sdz" (for multiple devices).
if [[ -n "$VGNAME" ]]; then
	GROUP=$VGNAME
else
	if [[ "$FIRST_DEVICE" == "$LAST_DEVICE" ]]; then
		GROUP=$(basename "$FIRST_DEVICE")
	else
		GROUP="$(basename "$FIRST_DEVICE"):$(basename "$LAST_DEVICE")"
	fi
fi

#
# Main program function
#

check_devices

cleanup() {
	trap '' HUP INT TERM
	# Signal handler.  undo_register only unregisters devices listed in
	# REGISTERED_DEVICES, which do_register appends to during start and
	# takeover.  Other commands never populate the array, so a signal
	# during read-keys, stop, remove, or clear does not unregister keys.
	#
	# undo_register is keyed to OURKEY (SCSI: --register --param-rk=OURKEY
	# with sark=0; NVMe: resv-register --crkey=OURKEY --rrega=1), so it can
	# only remove our own key, never another host's.  It runs only over
	# the devices in REGISTERED_DEVICES, never one we did not register
	# on.  Unregistering the local key also drops the local
	# reservation (see "stop" in lvmpersist(8)), so no explicit
	# release/clear is needed; a --clear here would wrongly wipe other
	# hosts' keys.
	# The trap is installed after option validation, so OURKEY is already
	# a valid key here.
	undo_register
	exit 1
}

trap "cleanup" HUP INT TERM

if [[ "$DO_START" -eq 1 && -n "$REMKEY" ]]; then
	do_takeover
elif [[ "$DO_START" -eq 1 ]]; then
	do_start
elif [[ "$DO_STOP" -eq 1 ]]; then
	do_stop
elif [[ "$DO_REMOVE" -eq 1 ]]; then
	do_remove
elif [[ "$DO_CLEAR" -eq 1 ]]; then
	do_clear
elif [[ "$DO_DEVTEST" -eq 1 ]]; then
	do_devtest
elif [[ "$DO_CHECKKEY" -eq 1 ]]; then
	do_checkkey
elif [[ "$DO_READKEYS" -eq 1 ]]; then
	do_readkeys
elif [[ "$DO_READRESERVATION" -eq 1 ]]; then
	do_readreservation
elif [[ "$DO_READ" -eq 1 ]]; then
	do_readkeys
	do_readreservation
fi
