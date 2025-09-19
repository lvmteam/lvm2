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

# user may override lvm location by setting LVM_BINARY
LVM=${LVM_BINARY:-lvm}

IFS_NL='
'

errorexit() {
	echo "  ${SCRIPTNAME}: $1"
	if [[ "$DO_START" -eq 1 || "$DO_STOP" -eq 1 || "$DO_REMOVE" -eq 1 ]]; then
		logger "${SCRIPTNAME}: $1"
	fi
	exit 1
}

logmsg() {
	echo "  ${SCRIPTNAME}: $1"
	if [[ "$DO_START" -eq 1 || "$DO_STOP" -eq 1 || "$DO_REMOVE" -eq 1 ]]; then
		logger "${SCRIPTNAME}: $1"
	fi
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
		cmdopts=""
		;;
	  /dev/dm-*)
		cmd="mpathpersist"
		cmdopts=""
		;;
	  /dev/mapper*)
		cmd="mpathpersist"
		cmdopts=""
		;;
	  *)
		cmd="sg_persist"
		cmdopts="--no-inquiry"
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
		type="$MPATH_PRTYPE"
		type_str="$MPATH_PRDESC"
		;;
	  /dev/mapper*)
		type="$MPATH_PRTYPE"
		type_str="$MPATH_PRDESC"
		;;
	  *)
		type="$SCSI_PRTYPE"
		type_str="$SCSI_PRDESC"
		;;
	esac
}

key_is_on_device_nvme() {
	FINDKEY_DEC=$(printf '%u' "$FINDKEY")

	if nvme resv-report --eds -o json "$dev" 2>/dev/null | grep -q "\"rkey\"\:${FINDKEY_DEC}"; then
		true
		return
	fi

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmd resv-report error on $dev"
	fi
	
	false
	return
}

key_is_on_device_scsi() {
	# grep with space to avoid matching the line "PR generation=0x..."
	# end-of-line matching required to avoid 0x123ab matching 0x123abc
	FINDKEY=" $FINDKEY$"

	if $cmd $cmdopts --in --read-keys "$dev" 2>/dev/null | grep -q "${FINDKEY}"; then
		true
		return
	fi

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmd read-keys error on $dev"
	fi
	
	false
	return
}

key_is_on_device() {
	dev=$1
	FINDKEY=$2
	set_cmd "$dev"

	if [[ "$cmd" == "nvme" ]]; then
		key_is_on_device_nvme "$dev" "$FINDKEY"
	else
		key_is_on_device_scsi "$dev" "$FINDKEY"
	fi
}

get_key_list_nvme() {
	local IFS=$IFS_NL
	dev=$1
	set_cmd "$dev"

	# json/jq output is only decimal
	KEYS=$(nvme resv-report --eds -o json "$dev" 2>/dev/null | jq '.regctlext[].rkey' | sort | xargs printf '0x%x ')

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmd read-keys error on $dev"
	fi

	if [[ "$KEYS" == "0x0 " ]]; then
		KEYS=""
	fi
}

get_key_list_scsi() {
	local IFS=$IFS_NL
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" == "mpathpersist" ]]; then
		no_keys_msg="0 registered reservation key"
	else
		no_keys_msg="there are NO registered reservation keys"
	fi

	if $cmd $cmdopts --in --read-keys "$dev" 2>/dev/null | grep -q $no_keys_msg; then
		KEYS=""
		return
	fi

	# sort with -u eliminates repeated keys listed with multipath

	KEYS=( $($cmd $cmdopts --in --read-keys "$dev" 2>/dev/null | grep "    0x" | sort -u | xargs ) )

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmd read-keys error on $dev"
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

	# get rkey from the regctlext section with rcsts=1

	str=$(nvme resv-report --eds -o json "$dev" 2>/dev/null | jq '.regctlext | map(select(.rcsts == 1)) | .[].rkey' | xargs printf '0x%x')

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "nvme resv-report error on $dev"
		HOLDER=0
		false
		return
	fi

	if [[ -z $str ]]; then
		logmsg "nvme resv-report holder output not found $dev"
		HOLDER=0
		false
		return
	fi

	HOLDER=$str
}

get_dev_reservation_holder_scsi() {
	dev=$1

	# combine with get_dev_reservation to
	# run a single sg_persist for holder and type?

	set_cmd "$dev"

	str=$( $cmd $cmdopts --in --read-reservation "$dev" 2>/dev/null | grep -e "Key\s*=\s*0x" | xargs )

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		if no_reservation_held "$dev"; then
			HOLDER=0
		else
			logmsg "$cmd read-reservation error on $dev"
			HOLDER=0
		fi
		false
		return
	fi

	if [[ -z $str ]]; then
		if no_reservation_held "$dev"; then
			HOLDER=0
		else
			logmsg "$cmd read-reservation holder output not found $dev"
			HOLDER=0
		fi
		false
		return
	fi

	HOLDER="${str:4}"
}

get_dev_reservation_holder() {
	dev=$1
	cur_type=$2

	# holder is not relevant for WEAR/EAAR
	if [[ "$cur_type" == "WEAR" || "$cur_type" == "EAAR" ]]; then
		HOLDER=0
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
	
	str=$(nvme resv-report --eds -o json "$dev" 2>/dev/null | jq '.rtype')

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "nvme resv-report error on $dev"
		DEV_PRDESC=error
		false
		return
	fi

	if [[ -z $str ]]; then
		logmsg "nvme resv-report no reservation type for $dev"
		DEV_PRDESC=error
		false
		return
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
		echo "Unknown PR value"
		exit 1
		;;
	esac

	DEV_PRTYPE="$str"
}

get_dev_reservation_scsi() {
	dev=$1

	str=$( $cmd $cmdopts --in --read-reservation "$dev" 2>/dev/null | grep -e "LU_SCOPE,\s\+type" )

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		if no_reservation_held "$dev"; then
			DEV_PRDESC=none
			DEV_PRTYPE=0
		else
			logmsg "$cmd read-reservation error on $dev"
			DEV_PRDESC=error
			DEV_PRTYPE=0
		fi
		false
		return
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
		false
		return
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

	if $cmd $cmdopts --in --read-reservation "$dev" 2>/dev/null | grep -q "there is NO reservation held"; then
		true
		return
	fi

	false
	return
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

	if nvme resv-report --eds "$dev" > /dev/null 2>&1; then
		true
		return
	fi

	false
	return
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

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "sg_persist report-capabilities error on $dev"
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

check_devices() {
	err=0
	FOUND_MPATH=0
	FOUND_SCSI=0
	FOUND_NVME=0

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
			MAJORMINOR=$(dmsetup info --noheadings -c -o major,minor "$dev")
			read -r <"/sys/dev/block/$MAJORMINOR/dm/uuid" DM_UUID 2>&1
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

	test "$err" -eq 1 && exit 1

	if [[ $FOUND_MPATH -eq 1 ]]; then
		which mpathpersist > /dev/null || errorexit "mpathpersist command not found."
		if ! grep "reservation_key file" /etc/multipath.conf > /dev/null; then
			echo "To use persistent reservations with multipath, run:"
			echo "  mpathconf --option reservation_key:file"
			echo "to configure multipath.conf, and then restart multipathd."
		fi
	fi

	if [[ $FOUND_SCSI -eq 1 ]]; then
		which sg_persist > /dev/null || errorexit "sg_persist command not found."
		which sg_turs > /dev/null || errorexit "sg_turs command not found."
	fi

	if [[ $FOUND_NVME -eq 1 ]]; then
		which nvme > /dev/null || errorexit "nvme command not found."
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
		/dev/mapper*)
			sg_turs "$dev" >/dev/null 2>&1
			ec=$?
			test $ec -eq 0 || logmsg "test unit ready error $ec from $dev"
		esac
	done
}

undo_register() {
	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-register --crkey="$OURKEY" --rrega=1 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --register --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi
		test $? -eq 0 || logmsg "$cmd unregister error on $dev"
	done
}

do_register_nvme() {
	dev=$1

	if [[ $PTPL -eq 1 ]]; then
		cmdopts+='--cptpl=1'
	fi

	# If our previous key is still registered, then we must use
	# rrega=2 and iekey.  If our previous key has been removed,
	# then we must use rrega=0.

	if ! nvme resv-register $cmdopts --nrkey="$OURKEY" --rrega=0 "$dev" >/dev/null 2>&1; then
		if ! nvme resv-register --nrkey="$OURKEY" --rrega=2 --iekey "$dev" >/dev/null 2>&1; then
			logmsg "$cmd register error on $dev"
			false
			return
		fi
	fi
}

do_register_scsi() {
	dev=$1
	set_cmd "$dev"

	if [[ $PTPL -eq 1 ]]; then
		cmdopts+='--param-aptpl'
	fi

	if ! $cmd $cmdopts --out --register-ignore --param-sark="$OURKEY" "$dev" >/dev/null 2>&1; then
		logmsg "$cmd register error on $dev"
		false
		return
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
}

do_takeover() {

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	if [[ -z "$REMKEY" ]]; then
		echo "Missing required option: --removekey."
		exit 1
	fi

	for dev in "${DEVICES[@]}"; do
		if ! key_is_on_device "$dev" "$REMKEY" ; then
			logmsg "start $GROUP specified key to remove $REMKEY not found on $dev."
			exit 1
		fi
	done

	err=0

	# Register our key

	for dev in "${DEVICES[@]}"; do
		if ! do_register "$dev"; then
			err=1
			break
		fi
	done

	if [[ "$err" -eq 1 ]]; then
		logmsg "start $GROUP failed to register our key."
		undo_register
		exit 1
	fi

	# Reserve the device

	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"
		set_type "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-acquire --crkey="$OURKEY" --prkey="$REMKEY" --rtype="$type" --racqa=2 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --preempt-abort --param-sark="$REMKEY" --param-rk="$OURKEY" --prout-type="$type" "$dev" >/dev/null 2>&1
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

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	for dev in "${DEVICES[@]}"; do
		set_type "$dev"
		if ! device_supports_type_str "$dev" "$type_str"; then
			logmsg "start $GROUP $dev does not support reservation type $type_str."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	err=0

	# Register our key on devices

	for dev in "${DEVICES[@]}"; do
		if ! do_register "$dev"; then
			err=1
			break
		fi
	done

	if [[ "$err" -eq 1 ]]; then
		logmsg "start $GROUP failed to register our key."
		undo_register
		exit 1
	fi

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
			$cmd $cmdopts --out --reserve --param-rk="$OURKEY" --prout-type="$type" "$dev" >/dev/null 2>&1
		fi

		if [[ "$?" -ne 0 ]]; then
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

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	# Removing reservation is not needed, we just remove our registration key.
	# The reservation will go away when the last key is removed.
	# sg_persist --out --no-inquiry --release --param-rk=${OURKEY} --prout-type=$SCSI_PRTYPE

	for dev in "${DEVICES[@]}"; do
		# Remove our registration key, we will no longer be able to write
		set_cmd "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-register --crkey="$OURKEY" --rrega=1 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --register --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi

		test $? -eq 0 || logmsg "$cmd unregister error on $dev"

		if key_is_on_device "$dev" "$OURKEY" ; then
			logmsg "stop $GROUP failed to unregister our key $OURKEY from $dev."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	logmsg "stopped $GROUP with key $OURKEY."
	exit 0
}

do_clear() {
	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	# our key must be registered to do clear.
	# we want to clear any/all PR state that we can find on the devs,
	# so just skip any devs that we cannot register with, and clear
	# what we can.

	for dev in "${DEVICES[@]}"; do
		set_type "$dev"
		if ! device_supports_type_str "$dev" "$type_str"; then
			echo "Device $dev: does not support PR"
			continue
		fi
		if ! key_is_on_device "$dev" "$OURKEY" ; then
			if ! do_register "$dev"; then
				logmsg "clear $GROUP skip $dev without registration"
			fi
		fi
	done

	err=0

	# clear releases the reservation and clears all registrations
	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-release --crkey="$OURKEY" --rrela=1 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --clear --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi

		test $? -eq 0 || logmsg "$cmd clear error on $dev"

		# Real result is whether the dev now has no registrations and
		# reservation.

		get_key_list "$dev"
		if [[ -n "$KEYS" ]]; then
			logmsg "clear $GROUP keys not cleared from $dev - ${KEYS[*]}"
			err=1
		fi

		if ! no_reservation_held "$dev"; then
			logmsg "clear $GROUP reservation not cleared from $dev"
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	logmsg "cleared $GROUP reservation and keys"
	exit 0
}

do_remove() {
	err=0

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	if [[ -z "$REMKEY" ]]; then
		echo "Missing required option: --removekey."
		exit 1
	fi

	for dev in "${DEVICES[@]}"; do
		if ! key_is_on_device "$dev" "$OURKEY" ; then
			logmsg "cannot remove $REMKEY from $dev without ourkey $OURKEY being registered"
			err=1
			continue
		fi

		set_cmd "$dev"
		# get the current pr type to use as the type for the preempt-abort command
		get_dev_reservation "$dev"

		if [[ "$cmd" == "nvme" ]]; then
			nvme resv-acquire --crkey="$OURKEY" --prkey="$REMKEY" --rtype="$DEV_PRTYPE" --racqa=2 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --preempt-abort --param-sark="$REMKEY" --param-rk="$OURKEY" --prout-type="$DEV_PRTYPE" "$dev" >/dev/null 2>&1
		fi

		test $? -eq 0 || logmsg "$cmd preempt-abort error on $dev"

		if key_is_on_device "$dev" "$REMKEY" ; then
			logmsg "failed to remove key $REMKEY from $dev in $GROUP."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	logmsg "removed key $REMKEY for $GROUP."
	exit 0
}

do_devtest() {
	err=0

	for dev in "${DEVICES[@]}"; do
		set_type "$dev"

		if device_supports_type_str "$dev" "$type_str"; then
			echo "Device $dev: supports type $type_str"
		else
			echo "Device $dev: does not support type $type_str"
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	exit 0
}

do_checkkey() {
	err=0

	for dev in "${DEVICES[@]}"; do
		if key_is_on_device "$dev" "$OURKEY" ; then
			echo "Device $dev: has key $OURKEY"
		else
			echo "Device $dev: does not have key $OURKEY"
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	exit 0
}

do_readkeys() {
	for dev in "${DEVICES[@]}"; do
		set_type "$dev"
		if ! device_supports_type_str "$dev" "$type_str"; then
			echo "Device $dev: does not support PR"
			continue
		fi
		get_key_list "$dev"
		if [[ -z "$KEYS" ]]; then
			echo "Device $dev: registered keys: none"
		else
			echo "Device $dev: registered keys: ${KEYS[*]}"
		fi
	done
}

do_readreservation() {
	for dev in "${DEVICES[@]}"; do
		set_type "$dev"
		if ! device_supports_type_str "$dev" "$type_str"; then
			echo "Device $dev: does not support PR"
			continue
		fi
		get_dev_reservation "$dev"
		get_dev_reservation_holder "$dev" $DEV_PRDESC
		if [[ "$DEV_PRDESC" == "WEAR" || "$DEV_PRDESC" == "EAAR" ]]; then
			echo "Device $dev: reservation: $DEV_PRDESC"
		else
			echo "Device $dev: reservation: $DEV_PRDESC holder $HOLDER"
		fi
	done
}

usage() {
	echo "${SCRIPTNAME}: use persistent reservations on devices in an LVM VG."
	echo ""
	echo "${SCRIPTNAME} start --ourkey KEY DEST"
	echo "	Register key and reserve device(s)."
	echo ""
	echo "${SCRIPTNAME} start --ourkey KEY --removekey REMKEY DEST"
	echo "	Register key and reserve device(s), replacing reservation holder by prempt-abort."
	echo ""
	echo "${SCRIPTNAME} stop --ourkey KEY DEST"
	echo "	Unregister key, dropping reservation."
	echo ""
	echo "${SCRIPTNAME} remove --ourkey KEY --removekey REMKEY DEST"
	echo "	Preempt-abort a key."
	echo ""
	echo "${SCRIPTNAME} clear --ourkey KEY DEST" 
	echo "	Release reservation and clear registered keys."
	echo ""
	echo "${SCRIPTNAME} devtest DEST"
	echo "	Test if devices support PR."
	echo ""
	echo "${SCRIPTNAME} check-key --key KEY DEST"
	echo "	Check if a key is registered."
	echo ""
	echo "${SCRIPTNAME} read-keys DEST"
	echo "	Display registered keys."
	echo ""
	echo "${SCRIPTNAME} read-reservation DEST" 
	echo "	Display reservation."
	echo ""
	echo "${SCRIPTNAME} read DEST"
	echo "	Display registered keys and reservation."
	echo ""
	echo "Options:"
	echo "    --ourkey KEY"
	echo "        KEY is the local key."
	echo "    --removekey REMKEY"
	echo "        REMKEY is a another host's key to remove."
	echo "    --key KEY"
	echo "        KEY is any key to check."
	echo "    --access ex|sh"
	echo "        Access type: ex (exclusive) for a local VG, sh (shared) for a shared VG."
	echo "        Translates to a specific PRTYPE for each device as appropriate (usually"
	echo "        WE for ex and WEAR for sh.)"
	echo "    --prtype PRTYPE"
	echo "        Use only a specific PRTYPE, an alternative to --access."
	echo "    --ptpl"
	echo "        Enable persist through power loss when starting."
	echo "    --debug"
	echo "        Enable shell debugging."
	echo ""
	echo "DEST:"
	echo "    --device PATH ..."
	echo "        One or more devices to operate on."
	echo "        (Repeat this option to use multiple devices.)"
	echo "    --vg VGNAME"
	echo "        One VG to operate on. All PVs in the VG are used."
	echo "        (An lvm command is run to find all the PVs.)"
	echo "    --vg VGNAME --device PATH ..."
	echo "        One or more devices to operate on, and a VG name to use"
	echo "        as an identifier for the set of devices."
	echo ""
	echo "PRTYPE: persistent reservation type (use abbreviation with --prtype)."
	echo "        WE: Write Exclusive"
	echo "        EA: Exclusive Access"
	echo "        WERO: Write Exclusive – registrants only (not yet supported)"
	echo "        EARO: Exclusive Access – registrants only (not yet supported)"
	echo "        WEAR: Write Exclusive – all registrants"
	echo "        EAAR: Exclusive Access – all registrants"
	echo ""
}

#
# BEGIN SCRIPT
#
PATH="/sbin:/usr/sbin:/bin:/usr/sbin:$PATH"
SCRIPTNAME=$(basename "$0")

if [ $# -lt 1 ]; then
	usage
	exit 0
fi

DO_START=0
DO_STOP=0
DO_REMOVE=0
DO_CLEAR=0
DO_DEVTEST=0
DO_CHECKKEY=0
DO_READKEYS=0
DO_READRESERVATION=0
DO_READ=0

CMD=$1
shift

case $CMD in
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
	help)
		usage
		exit 0
		;;
	-h)
		usage
		exit 0
		;;
	*)
		echo "Unknown command: $CMD."
		exit 1
		;;
esac

if [ "$UID" != 0 ] && [ "$EUID" != 0 ] && [ "$CMD" != "help" ]; then
	echo "${SCRIPTNAME} must be run as root."
	exit 1
fi

GETOPT="getopt"

OPTIONS=$("$GETOPT" -o h -l help,ourkey:,removekey:,key:,prtype:,access:,ptpl,debug,device:,vg: -n "${SCRIPTNAME}" -- "$@")
eval set -- "$OPTIONS"

while true
do
	case $1 in
	--ourkey)
		OURKEY=$2;
		shift; shift
		;;
	--key)
		KEY=$2;
		shift; shift
		;;
	--removekey)
		REMKEY=$2;
		shift; shift
		;;
	--ptpl)
		PTPL=1
		shift
		;;
	--access)
		ACCESS=$2
		shift; shift;
		;;
	--prtype)
		PRTYPE_ARG=$2
		shift; shift;
		;;
	--device)
		LAST_DEVICE=$2
		DEVICES+=("$LAST_DEVICE")
		shift; shift
		;;
	--vg)
		VGNAME=$2;
		shift; shift
		;;
	--debug)
		set -x
		shift
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
		echo "Unknown option \"$1\."
		exit 1
		;;
    esac
done

#
# Missing required options
#

if [[ -z "$LAST_DEVICE" && -z "$VGNAME" ]]; then
	echo "Missing required option: --vg or --device."
	exit 1
fi

if [[ -n "$PRTYPE_ARG" && -n "$ACCESS" ]]; then
	echo "Set --prtype or --access, not both."
	exit 1
fi

if [[ "$DO_CHECKKEY" -eq 1 && -z "$KEY" ]]; then
	echo "Missing required option: --key"
	exit 1
fi

if [[ "$DO_CHECKKEY" -eq 0 && -n "$KEY" ]]; then
	echo "Invalid option: --key"
	exit 1
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

if [[ -n "$OURKEY" && "$OURKEY" == "0x0"* ]]; then
	echo "Leading 0s are not permitted in keys."
	exit 1
fi

if [[ -n "$REMKEY" && "$REMKEY" == "0x0"* ]]; then
	echo "Leading 0s are not permitted in keys."
	exit 1
fi


if [[ -n "$OURKEY" && "$OURKEY" != "0x"* ]]; then
	if [[ "$OURKEY" =~ $DECDIGITS ]]; then
		OURKEY=$(printf '%x\n' "$OURKEY")
		OURKEY=0x${OURKEY}
		if [[ -n "$KEY" ]]; then
			echo "Using key: $OURKEY"
		else
			echo "Using ourkey: $OURKEY"
		fi
	else
		echo "Invalid decimal digits in key: $OURKEY (use 0x prefix for hex key)"
		exit 1
	fi
fi

if [[ -n "$OURKEY" && "$OURKEY" == "0x"* ]]; then
	if [[ ! "${OURKEY:2}" =~ $HEXDIGITS ]]; then
		echo "Invalid hex digits in key: $OURKEY"
		exit 1
	fi
	OURKEY="${OURKEY,,}"
fi

if [[ -n "$REMKEY" && "$REMKEY" != "0x"* ]]; then
	if [[ "$REMKEY" =~ $DECDIGITS ]]; then
		REMKEY=$(printf '%x\n' "$REMKEY")
		REMKEY=0x${REMKEY}
		echo "Using removekey: $REMKEY"
	else
		echo "Invalid decimal digits in key: $REMKEY (use 0x prefix for hex key)"
		exit 1
	fi
fi

if [[ -n "$REMKEY" && "$REMKEY" == "0x"* ]]; then
	if [[ ! "${REMKEY:2}" =~ $HEXDIGITS ]]; then
		echo "Invalid hex digits in key: $REMKEY"
		exit 1
	fi
	REMKEY="${REMKEY,,}"
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
		echo "Invalid access mode (use ex or sh)."
		exit 1
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
		echo "WERO is not yet supported."
		exit 1
		;;
	EARO)
		# Exclusive Access - registrants only
		SCSI_PRTYPE=6
		MPATH_PRTYPE=6
		NVME_PRTYPE=4
		# TODO: figure out the model of usage when
		# the reservation holder goes away.
		echo "EARO is not yet supported."
		exit 1
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
		echo "Unknown PRTYPE string (choose WE/EA/WERO/EARO/WEAR/EAAR)."
		exit 1
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
	DEVICES=( $("$LVM" vgs --nolocking --noheadings --separator : --sort pv_uuid --o pv_name --rows --config log/prefix=\"\" "$VGNAME") )
}

if [[ -z "$LAST_DEVICE" && -n "$VGNAME" ]]; then
	get_devices_from_vg
fi

FIRST_DEVICE="$DEVICES"

if [[ -z "$FIRST_DEVICE" ]]; then
	echo "Missing required --vg or --device."
	exit 1
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

