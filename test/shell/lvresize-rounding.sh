# Copyright (C) 2007-2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux prepare_vg 2

lvcreate -l 10 -n lv -i2 $vg

lvextend -l +1 $vg/lv 2>&1 | tee log
grep 'down to stripe' log
lvresize -l +1 $vg/lv 2>&1 | tee log
grep 'down to stripe' log

lvreduce -f -l -1 $vg/lv 2>&1 | tee log
grep 'up to stripe' log
lvresize -f -l -1 $vg/lv 2>&1 | tee log
grep 'up to stripe' log
