/*
 * Copyright (C) 2008,2009 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "lvm2app.h"
#include "metadata.h"
#include "lvm-string.h"

const char *lvm_pv_get_uuid(const pv_t pv)
{
	return pv_uuid_dup(pv);
}

const char *lvm_pv_get_name(const pv_t pv)
{
	return dm_pool_strndup(pv->vg->vgmem,
			       (const char *)pv_dev_name(pv), NAME_LEN + 1);
}

uint64_t lvm_pv_get_mda_count(const pv_t pv)
{
	return (uint64_t) pv_mda_count(pv);
}

uint64_t lvm_pv_get_dev_size(const pv_t pv)
{
	return (uint64_t) SECTOR_SIZE * pv_dev_size(pv);
}

uint64_t lvm_pv_get_size(const pv_t pv)
{
	return (uint64_t) SECTOR_SIZE * pv_size_field(pv);
}

uint64_t lvm_pv_get_free(const pv_t pv)
{
	return (uint64_t) SECTOR_SIZE * pv_free(pv);
}

int lvm_pv_resize(const pv_t pv, uint64_t new_size)
{
	/* FIXME: add pv resize code here */
	log_error("NOT IMPLEMENTED YET");
	return -1;
}
