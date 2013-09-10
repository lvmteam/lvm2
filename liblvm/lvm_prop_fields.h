/*
 * Copyright (C) 2013 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

FIELD(LV_CREATE_PARAMS, lvcreate_params, NUM, "skip_zero", zero, 2, uint32, skip_zero, "Skip zeroing on lv creation", 1)

FIELD(PV_CREATE_PARAMS, pvcreate_params, NUM, "size",      size, 2, uint64_t, size, "PV size", 1)
FIELD(PV_CREATE_PARAMS, pvcreate_params, NUM, "pvmetadatacopies", pvmetadatacopies, 2, uint64_t, pvmetadatacopies, "PV Metadata copies", 1)
FIELD(PV_CREATE_PARAMS, pvcreate_params, NUM, "pvmetadatasize", pvmetadatasize, 2, uint64_t, pvmetadatasize, "PV Metadata size", 1)
FIELD(PV_CREATE_PARAMS, pvcreate_params, NUM, "data_alignment", data_alignment, 2, uint64_t, data_alignment, "Start data to a multiple of value", 1)
FIELD(PV_CREATE_PARAMS, pvcreate_params, NUM, "data_alignment_offset", data_alignment_offset, 2, uint64_t, data_alignment_offset, "Shift the start of the data area", 1)
FIELD(PV_CREATE_PARAMS, pvcreate_params, NUM, "zero", zero, 2, uint64_t, zero, "Zero first 2048 bytes of device", 1)
