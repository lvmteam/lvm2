#!/bin/sh
# Copyright (C) 2008-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Set up things to run tests with dlm'

. lib/utils
. lib/inittest

[ -z "$LVM_TEST_LOCK_TYPE_DLM" ] && skip;

COROSYNC_CONF="/etc/corosync/corosync.conf"
COROSYNC_NODE="$(hostname)"
create_corosync_conf() {
	if test -a $COROSYNC_CONF; then
		if ! grep "created by lvm test suite" $COROSYNC_CONF; then
			rm $COROSYNC_CONF
		else
			mv $COROSYNC_CONF $COROSYNC_CONF.prelvmtest
		fi
	fi

	sed -e "s/@LOCAL_NODE@/$COROSYNC_NODE/" lib/test-corosync-conf > $COROSYNC_CONF
	echo "created new $COROSYNC_CONF"
}

DLM_CONF="/etc/dlm/dlm.conf"
create_dlm_conf() {
	if test -a $DLM_CONF; then
		if ! grep "created by lvm test suite" $DLM_CONF; then
			rm $DLM_CONF
		else
			mv $DLM_CONF $DLM_CONF.prelvmtest
		fi
	fi

	cp lib/test-dlm-conf $DLM_CONF
	echo "created new $DLM_CONF"
}

prepare_lvmlockd_dlm() {
	if pgrep lvmlockd ; then
		echo "Cannot run while existing lvmlockd process exists"
		exit 1
	fi

	if pgrep dlm_controld ; then
		echo "Cannot run while existing dlm_controld process exists"
		exit 1
	fi

	if pgrep corosync; then
		echo "Cannot run while existing corosync process exists"
		exit 1
	fi

	create_corosync_conf
	create_dlm_conf

	systemctl start corosync
	sleep 1
	if ! pgrep corosync; then
		echo "Failed to start corosync"
		exit 1
	fi

	systemctl start dlm
	sleep 1
	if ! pgrep dlm_controld; then
		echo "Failed to start dlm"
		exit 1
	fi

	lvmlockd
	sleep 1
	if ! pgrep lvmlockd ; then
		echo "Failed to start lvmlockd"
		exit 1
	fi
}

prepare_lvmlockd_dlm

