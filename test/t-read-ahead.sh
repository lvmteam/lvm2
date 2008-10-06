#!/bin/sh
# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#
# tests basic functionality of read-ahead and ra regressions
#

test_description='Test read-ahead functionality'

. ./test-utils.sh


get_lvs_() {
   case $(lvs --units s --nosuffix --noheadings -o $1_read_ahead "$vg"/"$lv") in
    *$2) true ;;
    *) false ;;
   esac
}

aux prepare_vg 5

#COMM "test various read ahead settings"
lvcreate -n "$lv" -l 100%FREE -i5 -I256 "$vg"     
lvdisplay "$vg"/"$lv"                             
lvchange -r auto "$vg"/"$lv" 2>&1 | grep auto     
get_lvs_ lv auto                                  
get_lvs_ lv_kernel 5120                           
lvchange -r 400 "$vg/$lv"                         
get_lvs_ lv 400                                   
vgremove -f "$vg"

