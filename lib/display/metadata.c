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
 *
 */

#include <sys/types.h>
#include <string.h>
#include "display/display.h"
#include "display/metadata.h"
#include "mm/dbg_malloc.h"
#include "log/log.h"

void pv_display_colons(pv_t * pv)
{
	char *uuid;

	if (!pv)
		return;

	uuid = display_uuid(pv->pv_uuid);

	printf("%s:%s:%d:%d:%d:%d:%d:%d:%d:%d:%d:%s\n",
	       pv->pv_name,
	       pv->vg_name,
	       pv->pv_size,
	       pv->pv_number,
	       pv->pv_status,
	       pv->pv_allocatable,
	       pv->lv_cur,
	       pv->pe_size / 2,
	       pv->pe_total,
	       pv->pe_total - pv->pe_allocated,
	       pv->pe_allocated, uuid ? uuid : "none");

	dbg_free(uuid);

	return;
}

void pv_display_full(pv_t * pv)
{
	ulong pe_free;
	char *size = NULL;
	char *uuid;

	if (!pv)
		return;

	uuid = display_uuid(pv->pv_uuid);

	printf("--- %sPhysical volume ---\n", pv->pe_size ? "" : "NEW ");
	printf("PV Name               %s\n", pv->pv_name);
	printf("VG Name               %s\n", pv->vg_name);

	size = display_size(pv->pv_size / 2, SIZE_SHORT);
	printf("PV Size               %s", size);
	dbg_free(size);

	if (pv->pe_size && pv->pe_total) {
		size =
		    display_size((pv->pv_size - pv->pe_size * pv->pe_total) / 2,
				 SIZE_SHORT);
		printf(" / NOT usable %s ", size);
		dbg_free(size);

		size =
		    display_size(
				 (pv->pe_on_disk.base +
				  pv->pe_total * sizeof (pe_disk_t)) / 1024,
				 SIZE_SHORT);
		printf("[LVM: %s]", size);
		dbg_free(size);
	}

	printf("\n");

	printf("PV#                   %u\n", pv->pv_number);
	printf("PV Status             %savailable\n",
	       (pv->pv_status & PV_ACTIVE) ? "" : "NOT ");

	printf("Allocatable           ");
	pe_free = pv->pe_total - pv->pe_allocated;
	if (pv->pe_total > 0 && (pv->pv_allocatable & PV_ALLOCATABLE)) {
		printf("yes");
		if (pe_free == 0 && pv->pe_total > 0)
			printf(" (but full)");
		printf("\n");
	} else
		printf("NO\n");

	printf("Cur LV                %u\n", pv->lv_cur);
	printf("PE Size (KByte)       %u\n", pv->pe_size / 2);
	printf("Total PE              %u\n", pv->pe_total);
	printf("Free PE               %lu\n", pe_free);
	printf("Allocated PE          %u\n", pv->pe_allocated);

#ifdef LVM_FUTURE
	printf("Stale PE              %u\n", pv->pe_stale);
#endif

	printf("PV UUID               %s\n", uuid ? uuid : "none");
	printf("\n");

	dbg_free(uuid);

	return;
}

/*******
void pv_display_short(pv_t * pv)
{

	if (pv != NULL) {
		printf("PV Name (#)           %s (%u)\n", pv->pv_name,
		       pv->pv_number);
		printf("PV Status             ");
		if (!(pv->pv_status & PV_ACTIVE))
			printf("NOT ");
		printf("available / ");
		if (!(pv->pv_allocatable & PV_ALLOCATABLE))
			printf("NOT ");
		printf("allocatable\n");
		printf("Total PE / Free PE    %u / %u\n",
		       pv->pe_total, pv->pe_total - pv->pe_allocated);
	}

	return;
}

void pv_display_pe(pv_t * pv, pe_disk_t * pe)
{
	int p;

	for (p = 0; p < pv->pe_total; p++)
		printf("pe#: %4d  vg: %s  lv: %d  le: %d\n",
		       p, pv->vg_name, pe[p].lv_num, pe[p].le_num);

	return;
}
*******/

void pv_display_pe_text(pv_t * pv, pe_disk_t * pe, lv_disk_t * lvs)
{
	int flag = 0;
	int lv_num_last = 0;
	int p = 0;
	int pe_free = 0;
	int *pe_this_count = NULL;
	int pt = 0;
	int pt_count = 0;
	lv_disk_t *lv;
	char *lv_name_this = NULL;
	char *lv_names = NULL;
	char *lv_names_sav = NULL;
	pe_disk_t *pe_this = NULL;

	if ((pe_this = dbg_malloc(pv->pe_total * sizeof (pe_disk_t))) == NULL) {
		log_error("pe_this allocation failed");
		goto pv_display_pe_text_out;
	}

	if ((pe_this_count = dbg_malloc(pv->pe_total * sizeof (int))) == NULL) {
		log_error("pe_this_count allocation failed");
		goto pv_display_pe_text_out;
	}

	memset(pe_this, 0, pv->pe_total * sizeof (pe_disk_t));
	memset(pe_this_count, 0, pv->pe_total * sizeof (int));

	/* get PE and LE summaries */
	pt_count = 0;
	for (p = pt_count; p < pv->pe_total; p++) {
		if (pe[p].lv_num != 0) {
			flag = 0;
			for (pt = 0; pt < pt_count; pt++) {
				if (pe_this[pt].lv_num == pe[p].lv_num) {
					flag = 1;
					break;
				}
			}
			if (flag == 0) {
				pe_this[pt_count].lv_num = pe[p].lv_num;
				for (pt = 0; pt < pv->pe_total; pt++)
					if (pe_this[pt_count].lv_num ==
					    pe[pt].lv_num)
						    pe_this_count[pt_count]++;
				pt_count++;
			}
		}
	}

	lv = lvs;
	printf("   --- Distribution of physical volume ---\n"
	       "   LV Name                   LE of LV  PE for LV\n");
	for (pt = 0; pt < pt_count; pt++) {
		printf("   %-25s ", lv->lv_name);
		if (strlen(lv->lv_name) > 25)
			printf("\n                             ");
		printf("%-8u  %-8d\n",
		       lv->lv_allocated_le,
		       pe_this_count[pt]);
		if (pe_this[pt].lv_num > lv_num_last) {
			lv_num_last = pe_this[pt].lv_num;
			lv_names_sav = lv_names;
			if ((lv_names = dbg_realloc(lv_names,
						    lv_num_last * NAME_LEN)) ==
			    NULL) {
				log_error("realloc error in %s [line %d]",
					  __FILE__, __LINE__);
				goto pv_display_pe_text_out;
			} else
				lv_names_sav = NULL;
		}
		strcpy(&lv_names[(pe_this[pt].lv_num - 1) * NAME_LEN],
		       lv->lv_name);
		lv++;
	}

	printf("\n   --- Physical extents ---\n"
	       "   PE    LV                        LE      Disk sector\n");
	pe_free = -1;
	for (p = 0; p < pv->pe_total; p++) {
		if (pe[p].lv_num != 0) {
			if (pe_free > -1) {
				pv_display_pe_free(pe_free, p);
				pe_free = -1;
			}
			lv_name_this = &lv_names[(pe[p].lv_num - 1) * NAME_LEN];
			printf("   %05d %-25s ", p, lv_name_this);
			if (strlen(lv_name_this) > 25)
				printf("\n                                  ");
			printf("%05d   %ld\n", pe[p].le_num,
			       get_pe_offset(p, pv));

		} else if (pe_free == -1)
			pe_free = p;
	}

	if (pe_free > 0)
		pv_display_pe_free(pe_free, p);

      pv_display_pe_text_out:
	if (lv_names != NULL)
		dbg_free(lv_names);
	else if (lv_names_sav != NULL)
		dbg_free(lv_names_sav);
	if (pe_this != NULL)
		dbg_free(pe_this);
	if (pe_this_count != NULL)
		dbg_free(pe_this_count);

	return;
}

void pv_display_pe_free(int pe_free, int p)
{
	printf("   %05d free\n", pe_free);

	if (p - pe_free > 1)
		printf("   .....\n   %05d free\n", p - 1);

	return;
}
