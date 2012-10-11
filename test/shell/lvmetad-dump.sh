#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test
test -e LOCAL_LVMETAD || skip

aux prepare_pvs 2
vgcreate $vg1 $dev1 $dev2
lvcreate -n bar -l 1 $vg1

lvmetad_talk() {
    if type -p socat >& /dev/null; then
	socat "unix-connect:$1" -
    elif echo | nc -U "$1"; then
	nc -U "$1"
    else
	echo "WARNING: Neither socat nor nc -U seems to be available." 1>&2
	echo "# DUMP FAILED"
	return 1
    fi
}

lvmetad_dump() {
    (echo 'request="dump"'; echo '##') | lvmetad_talk "$@"
}

(echo | lvmetad_talk ./lvmetad.socket) || skip
lvmetad_dump ./lvmetad.socket | tee lvmetad.txt

grep $vg1 lvmetad.txt
