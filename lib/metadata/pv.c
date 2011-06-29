/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2010 Red Hat, Inc. All rights reserved.
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
#include "metadata.h"
#include "lvmcache.h"

/*
 * FIXME: Check for valid handle before dereferencing field or log error?
 */
#define pv_field(handle, field)	((handle)->field)

char *pv_fmt_dup(const struct physical_volume *pv)
{
	if (!pv->fmt)
		return NULL;
	return dm_pool_strdup(pv->vg->vgmem, pv->fmt->name);
}

char *pv_name_dup(const struct physical_volume *pv)
{
	return dm_pool_strdup(pv->vg->vgmem, dev_name(pv->dev));
}

/*
 * Gets/Sets for external LVM library
 */
struct id pv_id(const struct physical_volume *pv)
{
	return pv_field(pv, id);
}

char *pv_uuid_dup(const struct physical_volume *pv)
{
	return id_format_and_copy(pv->vg->vgmem, &pv->id);
}

char *pv_tags_dup(const struct physical_volume *pv)
{
	return tags_format_and_copy(pv->vg->vgmem, &pv->tags);
}

const struct format_type *pv_format_type(const struct physical_volume *pv)
{
	return pv_field(pv, fmt);
}

struct id pv_vgid(const struct physical_volume *pv)
{
	return pv_field(pv, vgid);
}

struct device *pv_dev(const struct physical_volume *pv)
{
	return pv_field(pv, dev);
}

const char *pv_vg_name(const struct physical_volume *pv)
{
	return pv_field(pv, vg_name);
}

const char *pv_dev_name(const struct physical_volume *pv)
{
	return dev_name(pv_dev(pv));
}

uint64_t pv_size(const struct physical_volume *pv)
{
	return pv_field(pv, size);
}

uint64_t pv_dev_size(const struct physical_volume *pv)
{
	uint64_t size;

	if (!dev_get_size(pv->dev, &size))
		size = 0;
	return size;
}

uint64_t pv_size_field(const struct physical_volume *pv)
{
	uint64_t size;

	if (!pv->pe_count)
		size = pv->size;
	else
		size = (uint64_t) pv->pe_count * pv->pe_size;
	return size;
}

uint64_t pv_free(const struct physical_volume *pv)
{
	uint64_t freespace;

	if (!pv->pe_count)
		freespace = pv->size;
	else
		freespace = (uint64_t)
			(pv->pe_count - pv->pe_alloc_count) * pv->pe_size;
	return freespace;
}

uint64_t pv_status(const struct physical_volume *pv)
{
	return pv_field(pv, status);
}

uint32_t pv_pe_size(const struct physical_volume *pv)
{
	return pv_field(pv, pe_size);
}

uint64_t pv_pe_start(const struct physical_volume *pv)
{
	return pv_field(pv, pe_start);
}

uint32_t pv_pe_count(const struct physical_volume *pv)
{
	return pv_field(pv, pe_count);
}

uint32_t pv_pe_alloc_count(const struct physical_volume *pv)
{
	return pv_field(pv, pe_alloc_count);
}

uint32_t pv_mda_count(const struct physical_volume *pv)
{
	struct lvmcache_info *info;

	info = info_from_pvid((const char *)&pv->id.uuid, 0);
	return info ? dm_list_size(&info->mdas) : UINT64_C(0);
}

uint32_t pv_mda_used_count(const struct physical_volume *pv)
{
	struct lvmcache_info *info;
	struct metadata_area *mda;
	uint32_t used_count=0;

	info = info_from_pvid((const char *)&pv->id.uuid, 0);
	if (!info)
		return 0;
	dm_list_iterate_items(mda, &info->mdas) {
		if (!mda_is_ignored(mda))
			used_count++;
	}
	return used_count;
}

/**
 * is_orphan - Determine whether a pv is an orphan based on its vg_name
 * @pv: handle to the physical volume
 */
int is_orphan(const struct physical_volume *pv)
{
	return is_orphan_vg(pv_field(pv, vg_name));
}

/**
 * is_pv - Determine whether a pv is a real pv or dummy one
 * @pv: handle to device
 */
int is_pv(const struct physical_volume *pv)
{
	return (pv_field(pv, vg_name) ? 1 : 0);
}

int is_missing_pv(const struct physical_volume *pv)
{
	return pv_field(pv, status) & MISSING_PV ? 1 : 0;
}

char *pv_attr_dup(struct dm_pool *mem, const struct physical_volume *pv)
{
	char *repstr;

	if (!(repstr = dm_pool_zalloc(mem, 3))) {
		log_error("dm_pool_alloc failed");
		return NULL;
	}

	repstr[0] = (pv->status & ALLOCATABLE_PV) ? 'a' : '-';
	repstr[1] = (pv->status & EXPORTED_VG) ? 'x' : '-';
	repstr[2] = (pv->status & MISSING_PV) ? 'm' : '-';
	return repstr;
}

uint64_t pv_mda_size(const struct physical_volume *pv)
{
	struct lvmcache_info *info;
	uint64_t min_mda_size = 0;
	const char *pvid = (const char *)(&pv->id.uuid);

	/* PVs could have 2 mdas of different sizes (rounding effect) */
	if ((info = info_from_pvid(pvid, 0)))
		min_mda_size = find_min_mda_size(&info->mdas);
	return min_mda_size;
}

uint64_t pv_mda_free(const struct physical_volume *pv)
{
	struct lvmcache_info *info;
	uint64_t freespace = UINT64_MAX, mda_free;
	const char *pvid = (const char *)&pv->id.uuid;
	struct metadata_area *mda;

	if ((info = info_from_pvid(pvid, 0)))
		dm_list_iterate_items(mda, &info->mdas) {
			if (!mda->ops->mda_free_sectors)
				continue;
			mda_free = mda->ops->mda_free_sectors(mda);
			if (mda_free < freespace)
				freespace = mda_free;
		}

	if (freespace == UINT64_MAX)
		freespace = UINT64_C(0);
	return freespace;
}

uint64_t pv_used(const struct physical_volume *pv)
{
	uint64_t used;

	if (!pv->pe_count)
		used = 0LL;
	else
		used = (uint64_t) pv->pe_alloc_count * pv->pe_size;
	return used;
}

unsigned pv_mda_set_ignored(const struct physical_volume *pv, unsigned mda_ignored)
{
	struct lvmcache_info *info;
	struct metadata_area *mda, *vg_mda, *tmda;
	struct dm_list *mdas_in_use, *mdas_ignored, *mdas_to_change;

	if (!(info = info_from_pvid((const char *)&pv->id.uuid, 0)))
		return_0;

	mdas_in_use = &pv->fid->metadata_areas_in_use;
	mdas_ignored = &pv->fid->metadata_areas_ignored;
	mdas_to_change = mda_ignored ? mdas_in_use : mdas_ignored;

	if (is_orphan(pv)) {
		dm_list_iterate_items(mda, mdas_to_change)
			mda_set_ignored(mda, mda_ignored);
		return 1;
	}

	/*
	 * Do not allow disabling of the the last PV in a VG.
	 */
	if (pv_mda_used_count(pv) == vg_mda_used_count(pv->vg)) {
		log_error("Cannot disable all metadata areas in volume group %s.",
			  pv->vg->name);
		return 0;
	}

	/*
	 * Non-orphan case is more complex.
	 * If the PV's mdas are ignored, and we wish to un-ignore,
	 * we clear the bit and move them from the ignored mda list to the
	 * in_use list, ensuring the new state will get written to disk
	 * in the vg_write() path.
	 * If the PV's mdas are not ignored, and we are setting
	 * them to ignored, we set the bit but leave them on the in_use
	 * list, ensuring the new state will get written to disk in the
	 * vg_write() path.
	 */
	/* FIXME: Try not to update the cache here! Also, try to iterate over
	 *	  PV mdas only using the format instance's index somehow
	 * 	  (i.e. try to avoid using mda_locn_match call). */
	dm_list_iterate_items(mda, &info->mdas) {
		if (mda_is_ignored(mda) && !mda_ignored)
			/* Changing an ignored mda to one in_use requires moving it */
			dm_list_iterate_items_safe(vg_mda, tmda, mdas_ignored)
				if (mda_locns_match(mda, vg_mda)) {
					mda_set_ignored(vg_mda, mda_ignored);
					dm_list_move(mdas_in_use, &vg_mda->list);
				}

		dm_list_iterate_items_safe(vg_mda, tmda, mdas_in_use)
			if (mda_locns_match(mda, vg_mda))
				/* Don't move mda: needs writing to disk. */
				mda_set_ignored(vg_mda, mda_ignored);

		mda_set_ignored(mda, mda_ignored);
	}

	return 1;
}

