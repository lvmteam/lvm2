/*
 * Copyright (C) 2003 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 - 2005 Red Hat, Inc. All rights reserved.
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
#include "metadata.h"
#include "pv_alloc.h"
#include "toolcontext.h"

static struct pv_segment *_alloc_pv_segment(struct pool *mem,
					    struct physical_volume *pv,
					    uint32_t pe, uint32_t len,
					    struct lv_segment *lvseg,
					    uint32_t lv_area)
{
	struct pv_segment *peg;

	if (!(peg = pool_zalloc(mem, sizeof(*peg)))) {
		log_error("pv_segment allocation failed");
		return NULL;
	}

	peg->pv = pv;
	peg->pe = pe;
	peg->len = len;
	peg->lvseg = lvseg;
	peg->lv_area = lv_area;

	list_init(&peg->list);
	list_init(&peg->freelist);

	return peg;
}

int alloc_pv_segment_whole_pv(struct pool *mem, struct physical_volume *pv)
{
	struct pv_segment *peg;

	if (!pv->pe_count)
		return 1;

	/* FIXME Cope with holes in PVs */
	if (!(peg = _alloc_pv_segment(mem, pv, 0, pv->pe_count, NULL, 0))) {
		stack;
		return 0;
	}

	list_add(&pv->segments, &peg->list);
	list_add(&pv->free_segments, &peg->freelist);

	return 1;
}

int peg_dup(struct pool *mem, struct list *peg_new, struct list *peg_free_new,
	    struct list *peg_old)
{
	struct pv_segment *peg, *pego;

	list_init(peg_new);
	list_init(peg_free_new);

	list_iterate_items(pego, peg_old) {
		if (!(peg = _alloc_pv_segment(mem, pego->pv, pego->pe,
					      pego->len, pego->lvseg,
					      pego->lv_area))) {
			stack;
			return 0;
		} 
		list_add(peg_new, &peg->list);
		if (!peg->lvseg)
			list_add(peg_free_new, &peg->freelist);
	}

	return 1;
}

/*
 * Split peg at given extent.
 * Second part is always deallocated.
 */
static int _pv_split_segment(struct physical_volume *pv, struct pv_segment *peg,
			     uint32_t pe)
{
	struct pv_segment *peg_new;

	if (!(peg_new = _alloc_pv_segment(pv->fmt->cmd->mem, peg->pv, pe,
					  peg->len + peg->pe - pe,
					  NULL, 0))) {
		stack;
		return 0;
	}

	peg->len = peg->len - peg_new->len;

	list_add_h(&peg->list, &peg_new->list);
	list_add_h(&pv->free_segments, &peg_new->freelist);

	return 1;
}

/*
 * Ensure there is a PV segment boundary at the given extent.
 */
int pv_split_segment(struct physical_volume *pv, uint32_t pe)
{
	struct pv_segment *peg;

	if (pe == pv->pe_count)
		return 1;

	if (!(peg = find_peg_by_pe(pv, pe))) {
		log_error("Segment with extent %" PRIu32 " in PV %s not found",
			  pe, dev_name(pv->dev));
		return 0;
	}

	/* This is a peg start already */
	if (pe == peg->pe)
		return 1;

	if (!_pv_split_segment(pv, peg, pe)) {
		stack;
		return 0;
	}

	return 1;
}

struct pv_segment *assign_peg_to_lvseg(struct physical_volume *pv,
				       uint32_t pe, uint32_t area_len,
				       struct lv_segment *seg,
				       uint32_t area_num)
{
	struct pv_segment *peg;

	if (!pv_split_segment(pv, pe) || 
	    !pv_split_segment(pv, pe + area_len)) {
		stack;
		return NULL;
	}

	if (!(peg = find_peg_by_pe(pv, pe))) {
		log_error("Missing PV segment on %s at %u.",
			  dev_name(pv->dev), pe);
		return NULL;
	}

	peg->lvseg = seg;
	peg->lv_area = area_num;

	list_del(&peg->freelist);
	list_init(&peg->freelist);

	return peg;
}

int release_pv_segment(struct pv_segment *peg, uint32_t new_area_len)
{
	if (new_area_len == 0) {
		peg->lvseg = NULL;
		peg->lv_area = 0;

		/* FIXME merge free space */
		list_add(&peg->pv->free_segments, &peg->freelist);

		return 1;
	}

	if (!pv_split_segment(peg->pv, peg->pe + new_area_len)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Only for use by lv_segment merging routines.
 */
void merge_pv_segments(struct pv_segment *peg1, struct pv_segment *peg2)
{
	peg1->len += peg2->len;

	list_del(&peg2->list);

	if (!list_empty(&peg2->freelist))
		list_del(&peg2->freelist);
}

/*
 * Check all pv_segments in VG for consistency
 */
int check_pv_segments(struct volume_group *vg)
{
	struct physical_volume *pv;
	struct pv_list *pvl;
	struct pv_segment *peg;
	unsigned s, segno;
	int free_count, free_total, free_size;
	uint32_t start_pe;
	int ret = 1;

	list_iterate_items(pvl, &vg->pvs) {
		pv = pvl->pv;
		segno = 0;
		start_pe = 0;
		free_total = 0;

		free_count = list_size(&pv->free_segments);

		list_iterate_items(peg, &pv->segments) {
			free_size = list_size(&peg->freelist);
			s = peg->lv_area;

			/* FIXME Remove this next line eventually */
			log_debug("%s %u: %6u %6u: %s(%u:%u)",
				  dev_name(pv->dev), segno++, peg->pe, peg->len,
				  peg->lvseg ? peg->lvseg->lv->name : "NULL",
				  peg->lvseg ? peg->lvseg->le : 0, s);
			/* FIXME Add details here on failure instead */
			if (start_pe != peg->pe) {
				log_debug("Gap in pvsegs: %u, %u",
					  start_pe, peg->pe);
				ret = 0;
			}
			if (peg->lvseg) {
				if (peg->lvseg->area[s].type != AREA_PV) {
					log_debug("Wrong lvseg area type");
					ret = 0;
				}
				if (peg->lvseg->area[s].u.pv.pvseg != peg) {
					log_debug("Inconsistent pvseg pointers");
					ret = 0;
				}
				if (peg->lvseg->area_len != peg->len) {
					log_debug("Inconsistent length: %u %u",
						  peg->len,
						  peg->lvseg->area_len);
					ret = 0;
				}
				if (free_size) {
					log_debug("Segment is on free list!");
					ret = 0;
				}
			} else {
				free_total++;
				if (!free_size) {
					log_debug("Seg missing from free list");
					ret = 0;
				}
				if (free_size != free_count) {
					log_debug("Seg free size inconsistent: "
						  "%u != %u", free_size,
						  free_count);
					ret = 0;
				}
			}
			start_pe += peg->len;
		}

		if (free_count != free_total) {
			log_debug("Free list inconsistent: %u != %u",
				  free_count, free_total);
			ret = 0;
		}
	}

	return ret;
}

