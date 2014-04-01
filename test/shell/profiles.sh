#!/bin/sh

# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
# test basic profile functionality
#

. lib/test

MSG_FAILED_TO_APPLY_PROFILE="Failed to apply configuration profile"
MSG_IGNORING_INVALID_PROFILE="Ignoring invalid configuration profile"
MSG_NOT_PROFILABLE="not customizable by a profile"

# fail if the profile requested by --profile cmdline option is not present
not pvs --profile nonexistent 2>&1 | grep "$MSG_FAILED_TO_APPLY_PROFILE"

# config/checks=1: warning message about setting not being profilable +
#                  summary error message about invalid profile
# config/checks=0: just summary error message about invalid profile
aux profileconf invalid 'log/prefix=" "'

aux lvmconf 'config/checks = 0'
pvs --profile invalid 2>msg
not grep "$MSG_NOT_PROFILABLE" msg
grep "$MSG_IGNORING_INVALID_PROFILE" msg

aux lvmconf 'config/checks = 1'
pvs --profile invalid 2>msg
grep "$MSG_NOT_PROFILABLE" msg
grep "$MSG_IGNORING_INVALID_PROFILE" msg

aux lvmconf 'allocation/thin_pool_zero = 1'

# all profilable items listed here - should pass
aux profileconf valid 'allocation/thin_pool_zero = 0' \
                      'allocation/thin_pool_discards = "passdown"' \
                      'allocation/thin_pool_chunk_size = 64'\
                      'activation/thin_pool_autoextend_threshold = 100' \
                      'activation/thin_pool_autoextend_percent = 20' \
                      'global/units = "h"' \
                      'global/si_unit_consistency = 1' \
                      'global/suffix = 1' \
                      'global/lvdisplay_shows_full_device_path = 0' \
                      'report/aligned = 1' \
                      'report/buffered = 1' \
                      'report/headings = 1' \
                      'report/separator = " "' \
                      'report/prefixes = 0' \
                      'report/quoted = 1' \
                      'report/colums_as_rows = 0' \
                      'report/devtypes_sort = "devtype_name"' \
                      'report/devtypes_cols = "devtype_name,devtype_max_partitions,devtype_description"' \
                      'report/devtypes_cols_verbose = "devtype_name,devtype_max_partitions,devtype_description"' \
                      'report/lvs_sort = "vg_name,lv_name"' \
                      'report/lvs_cols = "lv_name,vg_name,lv_attr,lv_size,pool_lv,origin,data_percent,move_pv,mirror_log,copy_percent,convert_lv"' \
                      'report/lvs_cols_verbose = "lv_name,vg_name,seg_count,lv_attr,lv_size,lv_major,lv_minor,lv_kernel_major,lv_kernel_minor,pool_lv,origin,data_percent,metadata_percent,move_pv,copy_percent,mirror_log,convert_lv,lv_uuid,lv_profile"' \
                      'report/vgs_sort = "vg_name"' \
                      'report/vgs_cols = "vg_name,pv_count,lv_count,snap_count,vg_attr,vg_size,vg_free"' \
                      'report/vgs_cols_verbose = "vg_name,vg_attr,vg_extent_size,pv_count,lv_count,snap_count,vg_size,vg_free,vg_uuid,vg_profile"' \
                      'report/pvs_sort = "pv_name"' \
                      'report/pvs_cols = "pv_name,vg_name,pv_fmt,pv_attr,pv_size,pv_free"' \
                      'report/pvs_cols_verbose = "pv_name,vg_name,pv_fmt,pv_attr,pv_size,pv_free,dev_size,pv_uuid"' \
                      'report/segs_sort = "vg_name,lv_name,seg_start"' \
                      'report/segs_cols = "lv_name,vg_name,lv_attr,stripes,segtype,seg_size"' \
                      'report/segs_cols_verbose = "lv_name,vg_name,lv_attr,seg_start,seg_size,stripes,segtype,stripesize,chunksize"' \
                      'report/pvsegs_sort = "pv_name,pvseg_start"' \
                      'report/pvsegs_cols = "pv_name,vg_name,pv_fmt,pv_attr,pv_size,pv_free,pvseg_start,pvseg_size"' \
                      'report/pvsegs_cols_verbose = "pv_name,vg_name,pv_fmt,pv_attr,pv_size,pv_free,pvseg_start,pvseg_size,lv_name,seg_start_pe,segtype,seg_pe_ranges"'

aux profileconf extra 'allocation/thin_pool_chunk_size = 128'

pvs --profile valid 2>msg
not grep "$MSG_NOT_PROFILABLE" msg
not grep "$MSG_IGNORING_INVALID_PROFILE" msg

# attaching/detaching profiles to VG/LV
aux prepare_pvs 1 8
pvcreate "$dev1"
vgcreate $vg1 "$dev1"
check vg_field $vg1 vg_profile ""
lvcreate -l 1 -n $lv1 $vg1
check lv_field $vg1/$lv1 lv_profile ""
vgchange --profile valid $vg1
check vg_field $vg1 vg_profile valid
check lv_field $vg1/$lv1 lv_profile ""
lvchange --profile extra $vg1/$lv1
check vg_field $vg1 vg_profile valid
check lv_field $vg1/$lv1 lv_profile extra
vgchange --detachprofile $vg1
check vg_field $vg1 vg_profile ""
check lv_field $vg1/$lv1 lv_profile extra
lvchange --detachprofile $vg1/$lv1
check vg_field $vg1 vg_profile ""
check lv_field $vg1/$lv1 lv_profile ""

# dumpconfig and merged lvm.conf + profile
lvm dumpconfig &>out
grep 'thin_pool_zero=1' out
lvm dumpconfig --profile valid --mergedconfig >out
grep 'thin_pool_zero=0' out

# dumpconfig --profilable output must be usable as a profile
lvm dumpconfig --type profilable --file etc/profile/generated.profile
pvs --profile generated &> msg
not grep "$MSG_NOT_PROFILABLE" msg
not grep "$MSG_IGNORING_INVALID_PROFILE" msg
