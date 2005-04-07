/*
 * Copyright (C) 1997-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "pool.h"
#include "label.h"
#include "metadata.h"
#include "lvmcache.h"
#include "disk_rep.h"
#include "sptype_names.h"
#include "lv_alloc.h"
#include "str_list.h"
#include "display.h"
#include "segtype.h"

/* This file contains only imports at the moment... */

int import_pool_vg(struct volume_group *vg, struct pool *mem, struct list *pls)
{
	struct list *plhs;
	struct pool_list *pl;

	list_iterate(plhs, pls) {
		pl = list_item(plhs, struct pool_list);

		vg->extent_count +=
		    ((pl->pd.pl_blocks) / POOL_PE_SIZE);

		vg->pv_count++;

		if (vg->name)
			continue;

		vg->name = pool_strdup(mem, pl->pd.pl_pool_name);
		get_pool_vg_uuid(&vg->id, &pl->pd);
		vg->extent_size = POOL_PE_SIZE;
		vg->status |= LVM_READ | LVM_WRITE | CLUSTERED | SHARED;
		vg->free_count = 0;
		vg->max_lv = 1;
		vg->max_pv = POOL_MAX_DEVICES;
		vg->alloc = ALLOC_NORMAL;
		vg->lv_count = 0;
	}

	return 1;
}

int import_pool_lvs(struct volume_group *vg, struct pool *mem, struct list *pls)
{
	struct pool_list *pl;
	struct list *plhs;
	struct lv_list *lvl = pool_zalloc(mem, sizeof(*lvl));
	struct logical_volume *lv;

	if (!lvl) {
		log_error("Unable to allocate lv list structure");
		return 0;
	}

	if (!(lvl->lv = pool_zalloc(mem, sizeof(*lvl->lv)))) {
		log_error("Unable to allocate logical volume structure");
		return 0;
	}

	lv = lvl->lv;
	lv->status = 0;
	lv->vg = vg;
	lv->alloc = ALLOC_NORMAL;
	lv->size = 0;
	lv->name = NULL;
	lv->le_count = 0;
	lv->read_ahead = 0;
	lv->snapshot = NULL;
	list_init(&lv->snapshot_segs);
	list_init(&lv->segments);
	list_init(&lv->tags);

	list_iterate(plhs, pls) {
		pl = list_item(plhs, struct pool_list);

		lv->size += pl->pd.pl_blocks;

		if (lv->name)
			continue;

		if (!(lv->name = pool_strdup(mem, pl->pd.pl_pool_name))) {
			stack;
			return 0;
		}

		get_pool_lv_uuid(lv->lvid.id, &pl->pd);
		log_debug("Calculated lv uuid for lv %s: %s", lv->name,
			  lv->lvid.s);

		lv->status |= VISIBLE_LV | LVM_READ | LVM_WRITE;
		lv->major = POOL_MAJOR;

		/* for pool a minor of 0 is dynamic */
		if (pl->pd.pl_minor) {
			lv->status |= FIXED_MINOR;
			lv->minor = pl->pd.pl_minor + MINOR_OFFSET;
		} else {
			lv->minor = -1;
		}
		lv->snapshot = NULL;
		list_init(&lv->snapshot_segs);
		list_init(&lv->segments);
		list_init(&lv->tags);
	}

	lv->le_count = lv->size / POOL_PE_SIZE;
	lvl->lv = lv;
	list_add(&vg->lvs, &lvl->list);
	vg->lv_count++;

	return 1;
}

int import_pool_pvs(const struct format_type *fmt, struct volume_group *vg,
		    struct list *pvs, struct pool *mem, struct list *pls)
{
	struct pv_list *pvl;
	struct pool_list *pl;
	struct list *plhs;

	list_iterate(plhs, pls) {
		pl = list_item(plhs, struct pool_list);

		if (!(pvl = pool_zalloc(mem, sizeof(*pvl)))) {
			log_error("Unable to allocate pv list structure");
			return 0;
		}
		if (!(pvl->pv = pool_zalloc(mem, sizeof(*pvl->pv)))) {
			log_error("Unable to allocate pv structure");
			return 0;
		}
		if (!import_pool_pv(fmt, mem, vg, pvl->pv, pl)) {
			return 0;
		}
		pl->pv = pvl->pv;
		pvl->mdas = NULL;
		pvl->pe_ranges = NULL;
		list_add(pvs, &pvl->list);
	}

	return 1;
}

int import_pool_pv(const struct format_type *fmt, struct pool *mem,
		   struct volume_group *vg, struct physical_volume *pv,
		   struct pool_list *pl)
{
	struct pool_disk *pd = &pl->pd;

	memset(pv, 0, sizeof(*pv));

	get_pool_pv_uuid(&pv->id, pd);
	pv->fmt = fmt;

	pv->dev = pl->dev;
	if (!(pv->vg_name = pool_strdup(mem, pd->pl_pool_name))) {
		log_error("Unable to duplicate vg_name string");
		return 0;
	}
	pv->status = 0;
	pv->size = pd->pl_blocks;
	pv->pe_size = POOL_PE_SIZE;
	pv->pe_start = POOL_PE_START;
	pv->pe_count = pv->size / POOL_PE_SIZE;
	pv->pe_alloc_count = pv->pe_count;

	list_init(&pv->tags);

	return 1;
}

static const char *_cvt_sptype(uint32_t sptype)
{
	int i;
	for (i = 0; sptype_names[i].name[0]; i++) {
		if (sptype == sptype_names[i].label) {
			break;
		}
	}
	log_debug("Found sptype %X and converted it to %s",
		  sptype, sptype_names[i].name);
	return sptype_names[i].name;
}

static int _add_stripe_seg(struct pool *mem,
			   struct user_subpool *usp, struct logical_volume *lv,
			   uint32_t *le_cur)
{
	struct lv_segment *seg;
	int j;

	if (!(seg = alloc_lv_segment(mem, usp->num_devs))) {
		log_error("Unable to allocate striped lv_segment structure");
		return 0;
	}
	if(usp->striping & (usp->striping - 1)) {
		log_error("Stripe size must be a power of 2");
		return 0;
	}
	seg->stripe_size = usp->striping;
	seg->status |= 0;
	seg->le += *le_cur;

	/* add the subpool type to the segment tag list */
	str_list_add(mem, &seg->tags, _cvt_sptype(usp->type));

	for (j = 0; j < usp->num_devs; j++) {
		if (!(seg->segtype = get_segtype_from_string(lv->vg->cmd,
							     "striped"))) {
			stack;
			return 0;
		}

		seg->area_len = (usp->devs[j].blocks) / POOL_PE_SIZE;
		seg->len += seg->area_len;
		*le_cur += seg->area_len;
		seg->lv = lv;

		seg->area[j].type = AREA_PV;
		seg->area[j].u.pv.pv = usp->devs[j].pv;
		seg->area[j].u.pv.pe = 0;
	}
	list_add(&lv->segments, &seg->list);
	return 1;
}

static int _add_linear_seg(struct pool *mem,
			   struct user_subpool *usp, struct logical_volume *lv,
			   uint32_t *le_cur)
{
	struct lv_segment *seg;
	int j;

	for (j = 0; j < usp->num_devs; j++) {
		/* linear segments only have 1 data area */
		if (!(seg = alloc_lv_segment(mem, 1))) {
			log_error("Unable to allocate linear lv_segment "
				  "structure");
			return 0;
		}
		seg->stripe_size = usp->striping;
		seg->le += *le_cur;
		seg->chunk_size = POOL_PE_SIZE;
		seg->status |= 0;
		if (!(seg->segtype = get_segtype_from_string(lv->vg->cmd,
							     "striped"))) {
			stack;
			return 0;
		}
		/* add the subpool type to the segment tag list */
		str_list_add(mem, &seg->tags, _cvt_sptype(usp->type));

		seg->lv = lv;

		seg->area_len = (usp->devs[j].blocks) / POOL_PE_SIZE;
		seg->len = seg->area_len;
		*le_cur += seg->len;
		seg->area[0].type = AREA_PV;
		seg->area[0].u.pv.pv = usp->devs[j].pv;
		seg->area[0].u.pv.pe = 0;
		list_add(&lv->segments, &seg->list);
	}
	return 1;
}

int import_pool_segments(struct list *lvs, struct pool *mem,
			 struct user_subpool *usp, int subpools)
{

	struct list *lvhs;
	struct lv_list *lvl;
	struct logical_volume *lv;
	uint32_t le_cur = 0;
	int i;

	list_iterate(lvhs, lvs) {
		lvl = list_item(lvhs, struct lv_list);
		lv = lvl->lv;

		if (lv->status & SNAPSHOT)
			continue;

		for (i = 0; i < subpools; i++) {
			if (usp[i].striping) {
				if (!_add_stripe_seg(mem, &usp[i], lv, &le_cur)) {
					stack;
					return 0;
				}
			} else {
				if (!_add_linear_seg(mem, &usp[i], lv, &le_cur)) {
					stack;
					return 0;
				}
			}
		}
	}

	return 1;

}
