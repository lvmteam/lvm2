/*
 * Copyright (C) 2001  Sistina Software
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
#include "metadata.h"
#include "dbg_malloc.h"
#include "log.h"
#include "display.h"

#define SIZE_BUF 128

char *display_size(unsigned long long size, size_len_t sl)
{
	int s;
	ulong byte = 1024 * 1024 * 1024;
	char *size_buf = NULL;
	char *size_str[][2] = {
		{"Terabyte", "TB"},
		{"Gigabyte", "GB"},
		{"Megabyte", "MB"},
		{"Kilobyte", "KB"},
		{"", ""}
	};

	if (!(size_buf = dbg_malloc(SIZE_BUF))) {
		log_error("no memory for size display buffer");
		return NULL;
	}

	if (size == 0LL)
		sprintf(size_buf, "0");
	else {
		s = 0;
		while (size_str[s] && size < byte)
			s++, byte /= 1024;
		snprintf(size_buf, SIZE_BUF - 1,
			 "%.2f %s", (float) size / byte, size_str[s][sl]);
	}

	/* Caller to deallocate */
	return size_buf;
}

/*
 * FIXME: this function is badly named, it doesn't display the data it
 * creates a new uuid string with -'s in it.  It would be better if
 * the destination was passed in as well. EJT
 */
char *display_uuid(char *uuidstr) {
	int i, j;
	char *uuid;

	if ((!uuidstr) || !(uuid = dbg_malloc(NAME_LEN))) {
		log_error("no memory for uuid display buffer");
		return NULL;
	}

	memset(uuid, 0, NAME_LEN);

	i = 6;
	memcpy(uuid, uuidstr, i);
	uuidstr += i;

	for (j = 0; j < 6; j++) {
		uuid[i++] = '-';
		memcpy(&uuid[i], uuidstr, 4);
		uuidstr += 4;
		i += 4;
	}

	memcpy(&uuid[i], uuidstr, 2);

	/* Caller must free */
	return uuid;
}

void pvdisplay_colons(struct physical_volume *pv)
{
	char *uuid;

	if (!pv)
		return;

	uuid = display_uuid(pv->id.uuid);

	log_print("%s:%s:%llu:-1:%u:%u:-1:%llu:%u:%u:%u:%s",
	       pv->dev->name,
	       pv->vg_name,
	       pv->size,
	       /* FIXME pv->pv_number, Derive or remove? */
	       pv->status, /* FIXME Support old or new format here? */
	       pv->status & ALLOCATED_PV,  /* FIXME Remove? */
	       /* FIXME pv->lv_cur, Remove? */
	       pv->pe_size / 2,
	       pv->pe_count,
	       pv->pe_count - pv->pe_allocated,
	       pv->pe_allocated, 
	       *uuid ? uuid : "none");

	dbg_free(uuid);

	return;
}

void pvdisplay_full(struct physical_volume * pv)
{
        char *uuid;
	char *size, *size1; /*, *size2; */

	uint64_t pe_free;

        if (!pv)
                return;

	uuid = display_uuid(pv->id.uuid);

	log_print("--- %sPhysical volume ---", pv->pe_size ? "" : "NEW ");
	log_print("PV Name               %s", pv->dev->name);
	log_print("VG Name               %s", pv->vg_name);

	size = display_size(pv->size / 2, SIZE_SHORT);
	if (pv->pe_size && pv->pe_count) {
		size1 = display_size((pv->size - pv->pe_count * pv->pe_size) 
					/ 2, SIZE_SHORT);

/******** FIXME display LVM on-disk data size 
		size2 = display_size(pv->size / 2, SIZE_SHORT);
********/

		log_print("PV Size               %s"
			  " / not usable %s",   /*  [LVM: %s]", */
			  size, size1);         /* , size2);    */

		dbg_free(size1);
		/* dbg_free(size2); */
	} else
		log_print("PV Size               %s", size);
	dbg_free(size);

/*********FIXME Anything use this?
	log_print("PV#                   %u", pv->pv_number);
**********/

	log_print("PV Status             %savailable",
	       (pv->status & ACTIVE) ? "" : "NOT ");

	pe_free = pv->pe_count - pv->pe_allocated;
	if (pv->pe_count && (pv->status & ALLOCATED_PV))
		log_print("Allocatable           yes %s",
			  (!pe_free && pv->pe_count) ? "(but full)" : "");
	else
		log_print("Allocatable           NO");

/*********FIXME
	log_print("Cur LV                %u", pv->lv_cur);
*********/
	log_print("PE Size (KByte)       %llu", pv->pe_size / 2);
	log_print("Total PE              %u", pv->pe_count);
	log_print("Free PE               %llu", pe_free);
	log_print("Allocated PE          %u", pv->pe_allocated);

#ifdef LVM_FUTURE
	printf("Stale PE              %u", pv->pe_stale);
#endif

	log_print("PV UUID               %s", *uuid ? uuid : "none");
	log_print(" ");

	dbg_free(uuid);

	return;
}

void pv_display_short(struct physical_volume *pv)
{
	if (!pv) 
		return;

	log_print("PV Name               %s     ", pv->dev->name);
	/* FIXME  pv->pv_number); */
	log_print("PV Status             %savailable / %sallocatable",
		(pv->status & ACTIVE) ? "" : "NOT ",
		(pv->status & ALLOCATED_PV) ? "" : "NOT ");
	log_print("Total PE / Free PE    %u / %u",
	       pv->pe_count, pv->pe_count - pv->pe_allocated);

	return;
}

#if 0
/******** FIXME
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
#endif

