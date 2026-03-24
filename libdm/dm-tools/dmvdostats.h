/*
 * Copyright (C) 2022-2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef DM_TOOLS_DMVDOSTATS_H
#define DM_TOOLS_DMVDOSTATS_H

#include "libdm/libdevmapper.h"

uint64_t get_disp_factor(void);
char get_disp_units(void);
int show_units(void);

char *vdo_get_stats(const char *name);

int vdostats_print_verbose(const char *name, const char *stats_str);

struct dm_report *vdostats_report_init(const char *output_fields,
				       const char *output_separator,
				       uint32_t output_flags,
				       const char *sort_keys,
				       const char *selection);

int vdostats_report_device(struct dm_report *rh,
			   const char *name,
			   const char *stats_str);

#endif
