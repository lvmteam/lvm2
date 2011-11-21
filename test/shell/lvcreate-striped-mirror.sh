#!/bin/sh
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
aux prepare_vg 9

lvcreate -i2 -l2 -m1 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
not grep "Rounding" log
check mirror_images_redundant $vg $lv1
lvremove -ff $vg

lvcreate -i2 -l4 -m1 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
not grep "Rounding" log
check mirror_images_redundant $vg $lv1
lvremove -ff $vg

lvcreate -i3 -l3 -m1 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
not grep "Rounding" log
check mirror_images_redundant $vg $lv1
lvremove -ff $vg

lvcreate -i4 -l4 -m1 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
not grep "Rounding" log
check mirror_images_redundant $vg $lv1
lvremove -ff $vg


lvcreate -i2 -l2 -m2 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
not grep "Rounding" log
check mirror_images_redundant $vg $lv1
lvremove -ff $vg

lvcreate -i3 -l3 -m2 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
not grep "Rounding" log
check mirror_images_redundant $vg $lv1
lvremove -ff $vg

lvcreate -i2 -l2 -m3 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
not grep "Rounding" log
check mirror_images_redundant $vg $lv1
lvremove -ff $vg

lvcreate -i3 -l2 -m2 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
grep "Rounding size (2 extents) up to .* (3 extents)" log
lvremove -ff $vg

lvcreate -i3 -l4 -m2 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
grep "Rounding size (4 extents) up to .* (6 extents)" log
lvremove -ff $vg

lvcreate -i3 -l4 -m1 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
grep "Rounding size (4 extents) up to .* (6 extents)" log
lvremove -ff $vg

lvcreate -i4 -l4 -m1 --mirrorlog core -n $lv1 $vg 2>&1 | tee log
not grep "Rounding" log
lvremove -ff $vg
