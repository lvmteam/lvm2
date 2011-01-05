#!/bin/bash
# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

extend() {
	lvextend --use-policies --config "activation { snapshot_extend_threshold = $1 }" $vg/snap
}

write() {
	mount $DM_DEV_DIR/$vg/snap mnt
	dd if=/dev/zero of=mnt/file$1 bs=1k count=$2
	umount mnt
}

percent() {
	lvs $vg/snap -o snap_percent --noheadings | cut -c4- | cut -d. -f1
}

which mkfs.ext2 || exit 200

aux prepare_vg 2
lvcreate -l 8 -n base $vg
mkfs.ext2 $DM_DEV_DIR/$vg/base

lvcreate -s -l 4 -n snap $vg/base
mkdir mnt

write 1 4096
pre=`percent`
extend 50
post=`percent`

test $pre = $post
write 2 4096
pre=`percent`
extend 50
post=`percent`
test $pre -gt $post
