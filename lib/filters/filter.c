/*
 * Copyright (C) 2001 Sistina Software
 *
 * lvm is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * lvm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "dbg_malloc.h"
#include "log.h"
#include "dev-cache.h"
#include "filter.h"

static int passes_config_device_filter (struct dev_cache_filter *f, 
		                        struct device *dev)
{
	/* FIXME Check against config file scan/reject entries */
	return 1;
}

struct dev_filter *config_filter_create()
{
	struct dev_filter *f;

	if (!(f = dbg_malloc(sizeof(struct dev_filter)))) {
		log_error("lvm_v1_filter allocation failed");
		return NULL;
	}

	f->is_available = &passes_config_device_filter();

	return f;
}


void config_filter_destroy(struct dev_filter *f)
{
	dbg_free(f);
	return;
}

