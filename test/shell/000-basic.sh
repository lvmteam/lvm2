# Copyright (C) 2009-2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

lvm version

v=$abs_top_builddir/lib/misc/lvm-version.h
sed -n "/#define LVM_VERSION ./s///p" "$v" | sed "s/ .*//" > expected

lvm pvmove --version|sed -n "1s/.*: *\([0-9][^ ]*\) .*/\1/p" > actual

# ensure they are the same
diff -u actual expected

# ensure we can create devices (uses dmsetup, etc)
aux prepare_devs 5

# ensure we do not crash on a bug in config file
aux lvmconf 'log/prefix = 1""'
not lvs
