/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * Translates between disk and in-core formats.
 *
 * This file is released under the LGPL.
 */

#include "disk-rep.h"
#include "dbg_malloc.h"
#include "pool.h"
#include "hash.h"
#include "list.h"
#include "log.h"
#include "lvm-string.h"

#include <time.h>
#include <sys/utsname.h>
#include <linux/kdev_t.h>

static int _check_vg_name(const char *name)
{
	return strlen(name) < NAME_LEN;
}

/*
 * Extracts the last part of a path.
 */
static char *_create_lv_name(struct pool *mem, const char *full_name)
{
	const char *ptr = strrchr(full_name, '/');

	if (!ptr)
		ptr = full_name;
	else
		ptr++;

	return pool_strdup(mem, ptr);
}

int import_pv(struct pool *mem, struct device *dev,
	      struct volume_group *vg,
	      struct physical_volume *pv, struct pv_disk *pvd)
{
	memset(pv, 0, sizeof(*pv));
	memcpy(&pv->id, pvd->pv_uuid, ID_LEN);

	pv->dev = dev;
	if (!(pv->vg_name = pool_strdup(mem, pvd->vg_name))) {
		stack;
		return 0;
	}

	/* Store system_id from first PV if PV belongs to a VG */
	if (vg && !*vg->system_id)
	    strncpy(vg->system_id, pvd->system_id, NAME_LEN);

	if (vg &&
	    strncmp(vg->system_id, pvd->system_id, sizeof(pvd->system_id)))
		log_very_verbose("System ID %s on %s differs from %s for "
				 "volume group", pvd->system_id,
				 dev_name(pv->dev), vg->system_id);

	/*
	 * If exported, we still need to flag in pv->status too because
	 * we don't always have a struct volume_group when we need this.
	 */
	if (pvd->pv_status & VG_EXPORTED)
		pv->status |= EXPORTED_VG;

	if (pvd->pv_allocatable)
		pv->status |= ALLOCATABLE_PV;

	pv->size = pvd->pv_size;
	pv->pe_size = pvd->pe_size;
	pv->pe_start = pvd->pe_start;
	pv->pe_count = pvd->pe_total;
	pv->pe_allocated = pvd->pe_allocated;

	return 1;
}

int _system_id(char *s, const char *prefix)
{
	struct utsname uts;

	if (uname(&uts) != 0) {
		log_sys_error("uname", "_system_id");
		return 0;
	}

	if (lvm_snprintf(s, NAME_LEN, "%s%s%lu",
			 prefix, uts.nodename, time(NULL)) < 0) {
		log_error("Generated system_id too long");
		return 0;
	}

	return 1;
}

int export_pv(struct pool *mem, struct volume_group *vg,
	      struct pv_disk *pvd, struct physical_volume *pv)
{
	memset(pvd, 0, sizeof(*pvd));

	pvd->id[0] = 'H';
	pvd->id[1] = 'M';
	pvd->version = 1;

	memcpy(pvd->pv_uuid, pv->id.uuid, ID_LEN);

	if (!_check_vg_name(pv->vg_name)) {
		stack;
		return 0;
	}

	memset(pvd->vg_name, 0, sizeof(pvd->vg_name));

	if (pv->vg_name)
		strncpy(pvd->vg_name, pv->vg_name, sizeof(pvd->vg_name));

	/* Preserve existing system_id if it exists */
	if (vg && *vg->system_id)
		strncpy(pvd->system_id, vg->system_id, sizeof(pvd->system_id));

	/* Is VG already exported or being exported? */
	if (vg && (vg->status & EXPORTED_VG)) {
		/* Does system_id need setting? */
		if (!*vg->system_id ||
		    strncmp(vg->system_id, EXPORTED_TAG,
			    sizeof(EXPORTED_TAG) - 1)) {
			if (!_system_id(pvd->system_id, EXPORTED_TAG)) {
				stack;
				return 0;
			}
		}
		if (strlen(pvd->vg_name) + sizeof(EXPORTED_TAG) >
		    sizeof(pvd->vg_name)) {
			log_error("Volume group name %s too long to export",
				  pvd->vg_name);
			return 0;
		}
		strcat(pvd->vg_name, EXPORTED_TAG);
	}

	/* Is VG being imported? */
	if (vg && !(vg->status & EXPORTED_VG) && *vg->system_id &&
	    !strncmp(vg->system_id, EXPORTED_TAG, sizeof(EXPORTED_TAG) - 1)) {
		if (!_system_id(pvd->system_id, IMPORTED_TAG)) {
			stack;
			return 0;
		}
	}

	/* Generate system_id if PV is in VG */
	if (!pvd->system_id || !*pvd->system_id)
		if (!_system_id(pvd->system_id, "")) {
			stack;
			return 0;
		}

	/* Update internal system_id if we changed it */
	if (vg &&
	    (!*vg->system_id ||
	     strncmp(vg->system_id, pvd->system_id, sizeof(pvd->system_id))))
		strncpy(vg->system_id, pvd->system_id, NAME_LEN);

	//pvd->pv_major = MAJOR(pv->dev);

	if (pv->status & ALLOCATABLE_PV)
		pvd->pv_allocatable = PV_ALLOCATABLE;

	pvd->pv_size = pv->size;
	pvd->lv_cur = 0;	/* this is set when exporting the lv list */
	pvd->pe_size = pv->pe_size;
	pvd->pe_total = pv->pe_count;
	pvd->pe_allocated = pv->pe_allocated;
	pvd->pe_start = pv->pe_start;

	return 1;
}

int import_vg(struct pool *mem,
	      struct volume_group *vg, struct disk_list *dl,
	      int partial)
{
	struct vg_disk *vgd = &dl->vgd;
	memcpy(vg->id.uuid, vgd->vg_uuid, ID_LEN);

	if (!_check_vg_name(dl->pvd.vg_name)) {
		stack;
		return 0;
	}

	if (!(vg->name = pool_strdup(mem, dl->pvd.vg_name))) {
		stack;
		return 0;
	}

	if (!(vg->system_id = pool_alloc(mem, NAME_LEN))) {
		stack;
		return 0;
	}

	*vg->system_id = '\0';

	if (vgd->vg_status & VG_EXPORTED)
		vg->status |= EXPORTED_VG;

	if (vgd->vg_status & VG_EXTENDABLE)
		vg->status |= RESIZEABLE_VG;

	if (partial || (vgd->vg_access & VG_READ))
		vg->status |= LVM_READ;

	if (!partial && (vgd->vg_access & VG_WRITE))
		vg->status |= LVM_WRITE;

	if (vgd->vg_access & VG_CLUSTERED)
		vg->status |= CLUSTERED;

	if (vgd->vg_access & VG_SHARED)
		vg->status |= SHARED;

	vg->extent_size = vgd->pe_size;
	vg->extent_count = vgd->pe_total;
	vg->free_count = vgd->pe_total - vgd->pe_allocated;
	vg->max_lv = vgd->lv_max;
	vg->max_pv = vgd->pv_max;

	if (partial)
		vg->status |= PARTIAL_VG;

	return 1;
}

int export_vg(struct vg_disk *vgd, struct volume_group *vg)
{
	memset(vgd, 0, sizeof(*vgd));
	memcpy(vgd->vg_uuid, vg->id.uuid, ID_LEN);

	if (vg->status & LVM_READ)
		vgd->vg_access |= VG_READ;

	if (vg->status & LVM_WRITE)
		vgd->vg_access |= VG_WRITE;

	if (vg->status & CLUSTERED)
		vgd->vg_access |= VG_CLUSTERED;

	if (vg->status & SHARED)
		vgd->vg_access |= VG_SHARED;

	if (vg->status & EXPORTED_VG)
		vgd->vg_status |= VG_EXPORTED;

	if (vg->status & RESIZEABLE_VG)
		vgd->vg_status |= VG_EXTENDABLE;

	vgd->lv_max = vg->max_lv;
	vgd->lv_cur = vg->lv_count;

	vgd->pv_max = vg->max_pv;
	vgd->pv_cur = vg->pv_count;

	vgd->pe_size = vg->extent_size;
	vgd->pe_total = vg->extent_count;
	vgd->pe_allocated = vg->extent_count - vg->free_count;

	return 1;
}

int import_lv(struct pool *mem, struct logical_volume *lv, struct lv_disk *lvd)
{
	memset(&lv->id, 0, sizeof(lv->id));

	id_from_lvnum(&lv->id, lvd->lv_number);

        if (!(lv->name = _create_lv_name(mem, lvd->lv_name))) {
		stack;
		return 0;
	}

	if (lvd->lv_status & LV_SPINDOWN)
		lv->status |= SPINDOWN_LV;

	if (lvd->lv_status & LV_PERSISTENT_MINOR) {
		lv->status |= FIXED_MINOR;
		lv->minor = MINOR(lvd->lv_dev);
	} else
		lv->minor = -1;

	if (lvd->lv_access & LV_READ)
		lv->status |= LVM_READ;

	if (lvd->lv_access & LV_WRITE)
		lv->status |= LVM_WRITE;

	if (lvd->lv_badblock)
		lv->status |= BADBLOCK_ON;

	if (lvd->lv_allocation & LV_STRICT)
		lv->status |= ALLOC_STRICT;

	if (lvd->lv_allocation & LV_CONTIGUOUS)
		lv->status |= ALLOC_CONTIGUOUS;
	else
		lv->status |= ALLOC_SIMPLE;

	lv->read_ahead = lvd->lv_read_ahead;
        lv->size = lvd->lv_size;
        lv->le_count = lvd->lv_allocated_le;

	list_init(&lv->segments);

	return 1;
}

void export_lv(struct lv_disk *lvd, struct volume_group *vg,
	       struct logical_volume *lv, const char *dev_dir)
{
	memset(lvd, 0, sizeof(*lvd));
	snprintf(lvd->lv_name, sizeof(lvd->lv_name), "%s%s/%s",
		 dev_dir, vg->name, lv->name);

	/* FIXME: Add 'if' test */
	_check_vg_name(vg->name);
	strcpy(lvd->vg_name, vg->name);

	if (lv->status & LVM_READ)
		lvd->lv_access |= LV_READ;

	if (lv->status & LVM_WRITE)
		lvd->lv_access |= LV_WRITE;

	if (lv->status & SPINDOWN_LV)
		lvd->lv_status |= LV_SPINDOWN;

	if (lv->status & FIXED_MINOR) {
		lvd->lv_status |= LV_PERSISTENT_MINOR;
		lvd->lv_dev = MKDEV(0, lv->minor);
	}

	lvd->lv_read_ahead = lv->read_ahead;
	lvd->lv_stripes = list_item(lv->segments.n,
				    struct stripe_segment)->stripes;
	lvd->lv_stripesize = list_item(lv->segments.n,
				    struct stripe_segment)->stripe_size;

        lvd->lv_size = lv->size;
        lvd->lv_allocated_le = lv->le_count;

	if (lv->status & BADBLOCK_ON)
		lvd->lv_badblock = LV_BADBLOCK_ON;

	if (lv->status & ALLOC_STRICT)
		lvd->lv_allocation |= LV_STRICT;

	if (lv->status & ALLOC_CONTIGUOUS)
		lvd->lv_allocation |= LV_CONTIGUOUS;
}

int export_extents(struct disk_list *dl, int lv_num,
		   struct logical_volume *lv,
		   struct physical_volume *pv)
{
	struct list *segh;
	struct pe_disk *ped;
	struct stripe_segment *seg;
	uint32_t pe, s;

	list_iterate (segh, &lv->segments) {
		seg = list_item(segh, struct stripe_segment);

		for (s = 0; s < seg->stripes; s++) {
			if (seg->area[s].pv != pv)
				continue; /* not our pv */

			for (pe = 0; pe < (seg->len / seg->stripes); pe++) {
				ped = &dl->extents[pe + seg->area[s].pe];
				ped->lv_num = lv_num;
				ped->le_num = (seg->le / seg->stripes) + pe +
					      s * (lv->le_count / seg->stripes);
			}
		}
	}

	return 1;
}

int import_pvs(struct pool *mem, struct volume_group *vg,
	       struct list *pvds, struct list *results, int *count)
{
	struct list *pvdh;
	struct disk_list *dl;
	struct pv_list *pvl;

	*count = 0;
	list_iterate(pvdh, pvds) {

		dl = list_item(pvdh, struct disk_list);

		if (!(pvl = pool_alloc(mem, sizeof(*pvl))) ||
		    !(pvl->pv = pool_alloc(mem, sizeof(*pvl->pv)))) {
			stack;
			return 0;
		}

		if (!import_pv(mem, dl->dev, vg, pvl->pv, &dl->pvd)) {
			stack;
			return 0;
		}

		list_add(results, &pvl->list);
		(*count)++;
	}

	return 1;
}

static struct logical_volume *_add_lv(struct pool *mem,
				      struct volume_group *vg,
				      struct lv_disk *lvd)
{
	struct lv_list *ll;
	struct logical_volume *lv;

	if (!(ll = pool_zalloc(mem, sizeof(*ll))) ||
	    !(ll->lv = pool_zalloc(mem, sizeof(*ll->lv)))) {
		stack;
		return NULL;
	}
	lv = ll->lv;

	if (!import_lv(mem, lv, lvd)) {
		stack;
		return NULL;
	}

	list_add(&vg->lvs, &ll->list);
	lv->vg = vg;
	vg->lv_count++;

	return lv;
}

int import_lvs(struct pool *mem, struct volume_group *vg,
	       struct list *pvds)
{
	struct disk_list *dl;
	struct lvd_list *ll;
	struct lv_disk *lvd;
	struct list *pvdh, *lvdh;

	list_iterate(pvdh, pvds) {
		dl = list_item(pvdh, struct disk_list);
		list_iterate(lvdh, &dl->lvds) {
			ll = list_item(lvdh, struct lvd_list);
			lvd = &ll->lvd;

			if (!find_lv(vg, lvd->lv_name) &&
			    !_add_lv(mem, vg, lvd)) {
				stack;
				return 0;
			}
		}
	}

	return 1;
}

/* FIXME: tidy */
int export_lvs(struct disk_list *dl, struct volume_group *vg,
	       struct physical_volume *pv, const char *dev_dir)
{
	int r = 0;
	struct list *lvh, *sh;
	struct lv_list *ll;
	struct lvd_list *lvdl;
	int lv_num, len;
	struct hash_table *lvd_hash;

	if (!(lvd_hash = hash_create(32))) {
		stack;
		return 0;
	}

	/*
	 * setup the pv's extents array
	 */
	len = sizeof(struct pe_disk) * dl->pvd.pe_total;
	if (!(dl->extents = pool_alloc(dl->mem, len))) {
		stack;
		goto out;
	}
	memset(dl->extents, 0, len);

	list_iterate (lvh, &vg->lvs) {
		ll = list_item(lvh, struct lv_list);
		if (!(lvdl = pool_alloc(dl->mem, sizeof(*lvdl)))) {
			stack;
			goto out;
		}

		export_lv(&lvdl->lvd, vg, ll->lv, dev_dir);

		lv_num = lvnum_from_id(&ll->lv->id);

		lvdl->lvd.lv_number = lv_num;

		if (!hash_insert(lvd_hash, ll->lv->name, &lvdl->lvd)) {
			stack;
			goto out;
		}

		if (!export_extents(dl, lv_num + 1, ll->lv, pv)) {
			stack;
			goto out;
		}

		list_add(&dl->lvds, &lvdl->list);
		dl->pvd.lv_cur++;
	}

	/*
	 * Now we need to run through the snapshots, exporting
	 * the SNAPSHOT_ORG flags etc.
	 */
	list_iterate (sh, &vg->snapshots) {
		struct lv_disk *org, *cow;
		struct snapshot *s = list_item(sh,
					       struct snapshot_list)->snapshot;

		if (!(org = hash_lookup(lvd_hash, s->origin->name))) {
			log_err("Couldn't find snapshot origin '%s'.",
				s->origin->name);
			goto out;
		}

		if (!(cow = hash_lookup(lvd_hash, s->cow->name))) {
			log_err("Couldn't find snapshot cow store '%s'.",
				s->cow->name);
			goto out;
		}

		org->lv_access |= LV_SNAPSHOT_ORG;
		cow->lv_access |= LV_SNAPSHOT;
		cow->lv_snapshot_minor = org->lv_number;
		cow->lv_chunk_size = s->chunk_size;
	}

	r = 1;

 out:
	hash_destroy(lvd_hash);
	return r;
}

/*
 * FIXME: More inefficient code.
 */
int import_snapshots(struct pool *mem, struct volume_group *vg,
		     struct list *pvds)
{
	struct logical_volume *lvs[MAX_LV];
	struct list *pvdh, *lvdh;
	struct disk_list *dl;
	struct lv_disk *lvd;
	int lvnum;
	struct logical_volume *org, *cow;

	/* build an index of lv numbers */
	memset(lvs, 0, sizeof(lvs));
	list_iterate (pvdh, pvds) {
		dl = list_item(pvdh, struct disk_list);

		list_iterate (lvdh, &dl->lvds) {
			lvd = &(list_item(lvdh, struct lvd_list)->lvd);

			lvnum = lvd->lv_number;

			if (lvnum > MAX_LV) {
				log_err("Logical volume number "
					"out of bounds.");
				return 0;
			}

			if (!lvs[lvnum] &&
			    !(lvs[lvnum] = find_lv(vg, lvd->lv_name))) {
				log_err("Couldn't find logical volume '%s'.",
					lvd->lv_name);
				return 0;
			}
		}
	}

	/*
	 * Now iterate through yet again adding the snapshots.
	 */
	list_iterate (pvdh, pvds) {
		dl = list_item(pvdh, struct disk_list);

		list_iterate (lvdh, &dl->lvds) {
			lvd = &(list_item(lvdh, struct lvd_list)->lvd);

			if (!(lvd->lv_access & LV_SNAPSHOT))
				continue;

			lvnum = lvd->lv_number;
			cow = lvs[lvnum];
			if (!(org = lvs[lvd->lv_snapshot_minor])) {
				log_err("Couldn't find origin logical volume "
					"for snapshot '%s'.", lvd->lv_name);
				return 0;
			}

			/* we may have already added this snapshot */
			if (lv_is_cow(cow))
				continue;

			/* insert the snapshot */
			if (!vg_add_snapshot(org, cow, 1,
					     lvd->lv_chunk_size)) {
				log_err("Couldn't add snapshot.");
				return 0;
			}
		}
	}

	return 1;
}



int export_uuids(struct disk_list *dl, struct volume_group *vg)
{
	struct uuid_list *ul;
	struct pv_list *pvl;
	struct list *pvh;

	list_iterate(pvh, &vg->pvs) {
		pvl = list_item(pvh, struct pv_list);
		if (!(ul = pool_alloc(dl->mem, sizeof(*ul)))) {
			stack;
			return 0;
		}

		memset(ul->uuid, 0, sizeof(ul->uuid));
		memcpy(ul->uuid, pvl->pv->id.uuid, ID_LEN);

		list_add(&dl->uuids, &ul->list);
	}
	return 1;
}

/*
 * This calculates the nasty pv_number field
 * used by LVM1.
 */
void export_numbers(struct list *pvds, struct volume_group *vg)
{
	struct list *pvdh;
	struct disk_list *dl;
	int pv_num = 1;

	list_iterate(pvdh, pvds) {
		dl = list_item(pvdh, struct disk_list);
		dl->pvd.pv_number = pv_num++;
	}
}

/*
 * Calculate vg_disk->pv_act.
 */
void export_pv_act(struct list *pvds)
{
	struct list *pvdh;
	struct disk_list *dl;
	int act = 0;

	list_iterate(pvdh, pvds) {
		dl = list_item(pvdh, struct disk_list);
		if (dl->pvd.pv_status & PV_ACTIVE)
			act++;
	}

	list_iterate(pvdh, pvds) {
		dl = list_item(pvdh, struct disk_list);
		dl->vgd.pv_act = act;
	}
}

int export_vg_number(struct list *pvds, const char *vg_name,
		     struct dev_filter *filter)
{
	struct list *pvdh;
	struct disk_list *dl;
	int vg_num;

	if (!get_free_vg_number(filter, vg_name, &vg_num)) {
		stack;
		return 0;
	}

	list_iterate(pvdh, pvds) {
		dl = list_item(pvdh, struct disk_list);
		dl->vgd.vg_number = vg_num;
	}

	return 1;
}

