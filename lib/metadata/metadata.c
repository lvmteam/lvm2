/*
 * Copyright (C)  2001 Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 */

#include <string.h>
#include "mm/dbg_malloc.h"
#include "dev-mgr/dev-manager.h"
#include "log/log.h"
#include "metadata/metadata.h"

/* FIXME: Use registered fn ptrs to avoid including this? */
/*        Split into external/internal hdr files? */
#include "metadata/lvm_v1.h"

pv_t *pv_read(struct dev_mgr *dm, const char *pv_name)
{
	/* FIXME: Use config to select lvm_v1 format?  Cache results? */
	/*        Pass structure around rather than pv_name? */

	log_very_verbose("Reading metadata from %s", pv_name);

	return pv_read_lvm_v1(dm, pv_name);

}

pe_disk_t *pv_read_pe(const char *pv_name, const pv_t * pv)
{
	log_very_verbose("Reading PE metadata from %s", pv_name);

	return pv_read_pe_lvm_v1(pv_name, pv);
}

lv_disk_t *pv_read_lvs(const pv_t *pv)
{
	log_very_verbose("Reading LV metadata from %s", pv->pv_name);

	return pv_read_lvs_lvm_v1(pv);
}
