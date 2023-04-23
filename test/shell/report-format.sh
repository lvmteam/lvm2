#!/usr/bin/env bash

# Copyright (C) 2022 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_thin 1 0 0 || skip

aux prepare_vg 1

OUT_LOG_FILE="out"

lvcreate -l1 -T $vg/$lv1
lvcreate -l1 -n $lv2 --addtag lv_tag1 -an $vg
lvcreate -l1 -n $lv3 --addtag lv_tag3 --addtag lv_tag1 --addtag lv_tag2 $vg

aux lvmconf "report/output_format = basic"
lvs -o name,kernel_major,data_percent,tags | tee "$OUT_LOG_FILE"

#
#  LV    KMaj Data%  LV Tags
#  lvol1  253 0.00
#  lvol2   -1        lv_tag1
#  lvol3  253        lv_tag1,lv_tag2,lv_tag3
#
grep -E "^[[:space:]]*${lv1}[[:space:]]*[[:digit:]]+[[:space:]]*[[:digit:]]+.[[:digit:]]+[[:space:]]*\$" out
grep -E "^[[:space:]]*${lv2}[[:space:]]*-1[[:space:]]*lv_tag1[[:space:]]*\$" out
grep -E "^[[:space:]]*${lv3}[[:space:]]*[[:digit:]]+[[:space:]]*lv_tag1,lv_tag2,lv_tag3\$" out


aux lvmconf "report/output_format = json"
lvs -o name,kernel_major,data_percent,tags | tee "$OUT_LOG_FILE"
#  {
#      "report": [
#          {
#              "lv": [
#                  {"lv_name":"lvol1", "lv_kernel_major":"253", "data_percent":"0.00", "lv_tags":""},
#                  {"lv_name":"lvol2", "lv_kernel_major":"-1", "data_percent":"", "lv_tags":"lv_tag1"},
#                  {"lv_name":"lvol3", "lv_kernel_major":"253", "data_percent":"", "lv_tags":"lv_tag1,lv_tag2,lv_tag3"}
#              ]
#          }
#      ]
#  }
grep -E "^[[:space:]]*{\"lv_name\":\"$lv1\", \"lv_kernel_major\":\"[[:digit:]]+\", \"data_percent\":\"[[:digit:]]+.[[:digit:]]+\", \"lv_tags\":\"\"},\$" out
grep -E "^[[:space:]]*{\"lv_name\":\"$lv2\", \"lv_kernel_major\":\"-1\", \"data_percent\":\"\", \"lv_tags\":\"lv_tag1\"},\$" out
grep -E "^[[:space:]]*{\"lv_name\":\"$lv3\", \"lv_kernel_major\":\"[[:digit:]]+\", \"data_percent\":\"\", \"lv_tags\":\"lv_tag1,lv_tag2,lv_tag3\"}\$" out

aux lvmconf "report/output_format = json_std"
lvs -o name,kernel_major,data_percent,tags | tee "$OUT_LOG_FILE"
#  {
#      "report": [
#          {
#              "lv": [
#                  {"lv_name":"lvol1", "lv_kernel_major":253, "data_percent":0.00, "lv_tags":[]},
#                  {"lv_name":"lvol2", "lv_kernel_major":-1, "data_percent":null, "lv_tags":["lv_tag1"]},
#                  {"lv_name":"lvol3", "lv_kernel_major":253, "data_percent":null, "lv_tags":["lv_tag1","lv_tag2","lv_tag3"]}
#              ]
#          }
#      ]
#  }
grep -E "^[[:space:]]*{\"lv_name\":\"$lv1\", \"lv_kernel_major\":[[:digit:]]+, \"data_percent\":[[:digit:]]+.[[:digit:]]+, \"lv_tags\":\[\]},\$" out
grep -E "^[[:space:]]*{\"lv_name\":\"$lv2\", \"lv_kernel_major\":-1, \"data_percent\":null, \"lv_tags\":\[\"lv_tag1\"\]},\$" out
grep -E "^[[:space:]]*{\"lv_name\":\"$lv3\", \"lv_kernel_major\":[[:digit:]]+, \"data_percent\":null, \"lv_tags\":\[\"lv_tag1\",\"lv_tag2\",\"lv_tag3\"\]}\$" out

vgremove -ff $vg
