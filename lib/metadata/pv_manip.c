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

static struct pv_segment *_alloc_pv_segment(struct pool *mem,
					    struct physical_volume *pv,
					    uint32_t pe, uint32_t len)
{
	struct pv_segment *peg;

	if (!(peg = pool_zalloc(mem, sizeof(*peg)))) {
		log_error("pv_segment allocation failed");
		return NULL;
	}

	peg->pv = pv;
	peg->pe = pe;
	peg->len = len;
        peg->lvseg = NULL;
	peg->lv_area = 0;

	list_init(&peg->list);
	list_init(&peg->freelist);

	return peg;
}

int alloc_pv_segment_whole_pv(struct pool *mem, struct physical_volume *pv)
{
        struct pv_segment *peg;

	/* FIXME Cope with holes in PVs */
        if (!(peg = _alloc_pv_segment(mem, pv, 0, pv->pe_count))) {
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
					      pego->len))) {
			stack;
			return 0;
		} 
		peg->lvseg = pego->lvseg;
		peg->lv_area = pego->lv_area;
		list_add(peg_new, &peg->list);
		if (!peg->lvseg)
			list_add(peg_free_new, &peg->freelist);
	}

	return 1;
}

