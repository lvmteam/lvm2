/*
 * Copyright (C) 2001 Sistina Software
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

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "dbg_malloc.h"
#include "dev-manager.h"
#include "log.h"
#include "metadata.h"

/*
 * FIXME: these should not allocate memory
 */

pv_t *pv_copy_from_disk(pv_disk_t * pv_disk)
{
	pv_t *pv;

	if (!pv_disk || !(pv = (pv_t *) dbg_malloc(sizeof (*pv))))
		return 0;

#define xx16(v) pv->v = LVM_TO_CORE16(pv_disk->v)
#define xx32(v) pv->v = LVM_TO_CORE32(pv_disk->v)

	memset(pv, 0, sizeof (*pv));
	strncpy(pv->id, pv_disk->id, sizeof (pv->id));

	xx16(version);
	xx32(pv_on_disk.base);
	xx32(pv_on_disk.size);
	xx32(vg_on_disk.base);
	xx32(vg_on_disk.size);
	xx32(pv_uuidlist_on_disk.base);
	xx32(pv_uuidlist_on_disk.size);
	xx32(lv_on_disk.base);
	xx32(lv_on_disk.size);
	xx32(pe_on_disk.base);
	xx32(pe_on_disk.size);

	memset(pv->pv_name, 0, sizeof (pv->pv_name));
	memset(pv->pv_uuid, 0, sizeof (pv->pv_uuid));
	memcpy(pv->pv_uuid, pv_disk->pv_uuid, UUID_LEN);
	strncpy(pv->vg_name, pv_disk->vg_name, sizeof (pv->vg_name));
	strncpy(pv->system_id, pv_disk->system_id, sizeof (pv->system_id));

	pv->pv_dev = LVM_TO_CORE32(pv_disk->pv_major);

	xx32(pv_number);
	xx32(pv_status);
	xx32(pv_allocatable);
	xx32(pv_size);
	xx32(lv_cur);
	xx32(pe_size);
	xx32(pe_total);
	xx32(pe_allocated);
	pv->pe_stale = 0;
	xx32(pe_start);

#undef xx16
#undef xx32

	return pv;
}

pv_disk_t *pv_copy_to_disk(pv_t * pv_core)
{
	pv_disk_t *pv;

	if (!pv_core || !(pv = dbg_malloc(sizeof (*pv))))
		return 0;

#define xx16(v) pv->v = LVM_TO_DISK16(pv_core->v)
#define xx32(v) pv->v = LVM_TO_DISK32(pv_core->v)

	memset(pv, 0, sizeof (*pv));
	strncpy(pv->id, pv_core->id, sizeof (pv->id));

	xx16(version);
	xx32(pv_on_disk.base);
	xx32(pv_on_disk.size);
	xx32(vg_on_disk.base);
	xx32(vg_on_disk.size);
	xx32(pv_uuidlist_on_disk.base);
	xx32(pv_uuidlist_on_disk.size);
	xx32(lv_on_disk.base);
	xx32(lv_on_disk.size);
	xx32(pe_on_disk.base);
	xx32(pe_on_disk.size);

	memcpy(pv->pv_uuid, pv_core->pv_uuid, UUID_LEN);
	strncpy(pv->vg_name, pv_core->vg_name, sizeof (pv->vg_name));
	strncpy(pv->system_id, pv_core->system_id, sizeof (pv->system_id));

	/* core type is kdev_t; but no matter what it is,
	   only store major for check in pv_read() */
	pv->pv_major = LVM_TO_DISK32(MAJOR(pv_core->pv_dev));
	xx32(pv_number);
	xx32(pv_status);
	xx32(pv_allocatable);
	xx32(pv_size);
	xx32(lv_cur);
	xx32(pe_size);
	xx32(pe_total);
	xx32(pe_allocated);
	xx32(pe_start);

#undef xx16
#undef xx32

	return pv;
}

lv_t *lv_copy_from_disk(lv_disk_t * lv_disk)
{
	lv_t *lv;

	if (!lv_disk || !(lv = dbg_malloc(sizeof (*lv))))
		return 0;

	memset(lv, 0, sizeof (*lv));

#define xx16(v) lv->v = LVM_TO_CORE16(lv_disk->v)
#define xx32(v) lv->v = LVM_TO_CORE32(lv_disk->v)

	strncpy(lv->lv_name, lv_disk->lv_name, sizeof (lv->lv_name));
	strncpy(lv->vg_name, lv_disk->vg_name, sizeof (lv->vg_name));

	xx32(lv_access);
	xx32(lv_status);

	lv->lv_open = 0;

	xx32(lv_dev);
	xx32(lv_number);
	xx32(lv_mirror_copies);
	xx32(lv_recovery);
	xx32(lv_schedule);
	xx32(lv_size);

	lv->lv_current_pe = NULL;

	xx32(lv_allocated_le);

	lv->lv_current_le = lv->lv_allocated_le;

	xx32(lv_stripes);
	xx32(lv_stripesize);
	xx32(lv_badblock);
	xx32(lv_allocation);
	xx32(lv_io_timeout);
	xx32(lv_read_ahead);

	lv->lv_snapshot_org = NULL;
	lv->lv_snapshot_prev = NULL;
	lv->lv_snapshot_next = NULL;
	lv->lv_block_exception = NULL;
	lv->lv_remap_ptr = 0;
	lv->lv_remap_end = 0;

	xx32(lv_snapshot_minor);
	xx16(lv_chunk_size);

#undef xx32
#undef xx16

	return lv;
}

lv_disk_t *lv_copy_to_disk(lv_t * lv_core)
{
	lv_disk_t *lv;

	if (!lv_core || !(lv = dbg_malloc(sizeof (*lv))))
		return 0;

	memset(lv, 0, sizeof (*lv));

#define xx16(v) lv->v = LVM_TO_DISK16(lv_core->v)
#define xx32(v) lv->v = LVM_TO_DISK32(lv_core->v)

	strncpy(lv->lv_name, lv_core->lv_name, sizeof (lv->lv_name));
	strncpy(lv->vg_name, lv_core->vg_name, sizeof (lv->vg_name));

	xx32(lv_access);
	xx32(lv_status);

	lv->lv_open = 0;

	xx32(lv_dev);
	xx32(lv_number);
	xx32(lv_mirror_copies);
	xx32(lv_recovery);
	xx32(lv_schedule);
	xx32(lv_size);
	xx32(lv_snapshot_minor);
	xx16(lv_chunk_size);

	lv->dummy = 0;

	xx32(lv_allocated_le);
	xx32(lv_stripes);
	xx32(lv_stripesize);
	xx32(lv_badblock);
	xx32(lv_allocation);
	xx32(lv_io_timeout);
	xx32(lv_read_ahead);

#undef xx32
#undef xx16

	return lv;
}

vg_t *vg_copy_from_disk(vg_disk_t * vg_disk)
{
	vg_t *vg;

	if (!vg_disk || !(vg = dbg_malloc(sizeof (*vg))))
		return 0;

#define xx32(v) vg->v = LVM_TO_CORE32(vg_disk->v)

	memset(vg, 0, sizeof (vg_t));

	xx32(vg_number);
	xx32(vg_access);
	xx32(vg_status);
	xx32(lv_max);
	xx32(lv_cur);

	vg->lv_open = 0;

	xx32(pv_max);
	xx32(pv_cur);
	xx32(pv_act);

	vg->dummy = 0;

	xx32(vgda);
	xx32(pe_size);
	xx32(pe_total);
	xx32(pe_allocated);
	xx32(pvg_total);

#undef xx32

	memset(&vg->pv, 0, sizeof (vg->pv));
	memset(&vg->lv, 0, sizeof (vg->lv));

	memset(vg->vg_uuid, 0, sizeof (vg->vg_uuid));
	memcpy(vg->vg_uuid, vg_disk->vg_uuid, UUID_LEN);

	return vg;
}

vg_disk_t *vg_copy_to_disk(vg_t * vg_core)
{
	vg_disk_t *vg;

	if (!vg_core ||
/* FIXME:	vg_check_consistency(vg_core) || */
	    !(vg = dbg_malloc(sizeof (*vg))))
		return 0;

	memset(vg, 0, sizeof (*vg));

#define xx32(v) vg->v = LVM_TO_DISK32(vg_core->v)

	xx32(vg_number);
	xx32(vg_access);
	xx32(vg_status);
	xx32(lv_max);
	xx32(lv_cur);

	vg->lv_open = 0;

	xx32(pv_max);
	xx32(pv_cur);
	xx32(pv_act);

	vg->dummy = 0;

	xx32(vgda);
	xx32(pe_size);
	xx32(pe_total);
	xx32(pe_allocated);
	xx32(pvg_total);

#undef xx32

	memcpy(vg->vg_uuid, vg_core->vg_uuid, UUID_LEN);

	return vg;
}

pe_disk_t *pe_copy_from_disk(pe_disk_t * pe_file, int count)
{
	int i;
	pe_disk_t *pe;
	size_t s = sizeof (*pe) * count;

	if (!pe_file || count <= 0 || !(pe = dbg_malloc(s)))
		return 0;

	memset(pe, 0, s);
	for (i = 0; i < count; i++) {
		pe[i].lv_num = LVM_TO_CORE16(pe_file[i].lv_num);
		pe[i].le_num = LVM_TO_CORE16(pe_file[i].le_num);
	}

	return pe;
}

pe_disk_t *pe_copy_to_disk(pe_disk_t * pe_core, int count)
{
	int i;
	pe_disk_t *pe;
	size_t s = sizeof (*pe) * count;

	if (!pe_core || count <= 0 || !(pe = dbg_malloc(s)))
		return 0;

	memset(pe, 0, s);
	for (i = 0; i < count; i++) {
		pe[i].lv_num = LVM_TO_DISK16(pe_core[i].lv_num);
		pe[i].le_num = LVM_TO_DISK16(pe_core[i].le_num);
	}

	return pe;
}

pv_t *pv_read_lvm_v1(struct dev_mgr * dm, const char *pv_name)
{
	int pv_handle = -1;
	ssize_t read_ret;
	ssize_t bytes_read = 0;
	static pv_disk_t pv_this;
	struct stat stat_b;
	pv_t *pv = NULL;
	struct device *pv_dev;

	if ((pv_handle = open(pv_name, O_RDONLY)) == -1) {
		log_error("%s: open failed: %s", pv_name, strerror(errno));
		return NULL;
	}

	if ((fstat(pv_handle, &stat_b))) {
		log_error("%s: fstat failed: %s", pv_name, strerror(errno));
		goto pv_read_lvm_v1_out;
	}

	while ((bytes_read < sizeof (pv_this) &&
		(read_ret = read(pv_handle, &pv_this + bytes_read,
				 sizeof (pv_this) - bytes_read)) != -1))
		bytes_read += read_ret;

	if (read_ret == -1) {
		log_error("%s: read failed: %s", pv_name, strerror(errno));
		goto pv_read_lvm_v1_out;
	}

	pv = pv_copy_from_disk(&pv_this);

	/* correct for imported/moved volumes */
	if (!(pv_dev = dev_by_dev(dm, stat_b.st_rdev))) {
		log_error("Device missing from cache");
		goto pv_read_lvm_v1_out;
	}

	memset(pv->pv_name, 0, sizeof (pv->pv_name));
	strncpy(pv->pv_name, pv_dev->name, sizeof (pv->pv_name) - 1);

	/* FIXME: Deleted version / consistency / export checks! */

	pv->pv_dev = stat_b.st_rdev;

      pv_read_lvm_v1_out:
	if (pv_handle != -1)
		close(pv_handle);

	return pv;
}

pe_disk_t *pv_read_pe_lvm_v1(const char *pv_name, const pv_t * pv)
{
	int pv_handle = -1;
	uint size = 0;
	ssize_t read_ret;
	ssize_t bytes_read = 0;
	pe_disk_t *pe = NULL;
	pe_disk_t *pe_this;

	size = pv->pe_total * sizeof (pe_disk_t);
	if (size > pv->pe_on_disk.size) {
		log_error("PEs extend beyond end of volume group!");
		return pe; /*NULL*/
	}

	if ((pv_handle = open(pv_name, O_RDONLY)) == -1) {
		log_error("%s: open failed: %s", pv_name, strerror(errno));
		goto pv_read_pe_out;
	}

	if (lseek(pv_handle, pv->pe_on_disk.base, SEEK_SET) !=
		 pv->pe_on_disk.base) {
		log_error("%s: lseek to PE failed: %s", pv_name, strerror(errno));
		goto pv_read_pe_out;
	}

	if (!(pe_this = dbg_malloc(size))) {
		log_error("PE malloc failed");
		goto pv_read_pe_out;
	}

	while ((bytes_read < size &&
		(read_ret = read(pv_handle, (void *)pe_this + bytes_read,
				 size - bytes_read)) != -1))
		bytes_read += read_ret;

	if (read_ret == -1) {
		log_error("%s: read failed: %s", pv_name, strerror(errno));
		goto pv_read_pe_out;
	}

	pe = pe_copy_from_disk(pe_this, pv->pe_total);
	
      pv_read_pe_out:
	if (pv_handle != -1)
		close(pv_handle);

	dbg_free(pe_this);

	return pe;
}

lv_disk_t *pv_read_lvs_lvm_v1(const pv_t *pv)
{
	int pv_handle = -1;
	uint size = 0;
	ssize_t read_ret;
	ssize_t bytes_read = 0;
	lv_disk_t *lvs;

	/* FIXME: replace lv_cur? */
	size = pv->lv_cur * sizeof (lv_disk_t);

	if ((pv_handle = open(pv->pv_name, O_RDONLY)) == -1) {
		log_error("%s: open failed: %s", pv->pv_name, strerror(errno));
		goto pv_read_lvs_out;
	}

	if (lseek(pv_handle, pv->lv_on_disk.base, SEEK_SET) !=
		 pv->lv_on_disk.base) {
		log_error("%s: lseek to LV failed: %s", pv->pv_name, strerror(errno));
		goto pv_read_lvs_out;
	}

	if (!(lvs = dbg_malloc(size))) {
		log_error("PE malloc failed");
		goto pv_read_lvs_out;
	}

	while ((bytes_read < size &&
		(read_ret = read(pv_handle, (void *) lvs + bytes_read,
				 size - bytes_read)) != -1))
		bytes_read += read_ret;

	if (read_ret == -1) {
		log_error("%s: read failed: %s", pv->pv_name, strerror(errno));
		goto pv_read_lvs_out;
	}

      pv_read_lvs_out:
	if (pv_handle != -1)
		close(pv_handle);

	/* Caller frees */
	return lvs;
}

