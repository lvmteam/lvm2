/*
 * Copyright (C) 2001 Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "tools.h"

void pvdisplay_single(struct physical_volume *pv);

int pvdisplay(int argc, char **argv)
{
	int opt=0;

	struct list_head *pvh, *pvs;
	struct physical_volume *pv;

	if (arg_count(colon_ARG) && arg_count(verbose_ARG)) {
		log_error("Option -v not allowed with option -c");
		return EINVALID_CMD_LINE;
	}

	if (argc)  {
                log_very_verbose("Using physical volume(s) on command line");

		for (; opt < argc; opt++) {
			if (!(pv = ios->pv_read(ios, argv[opt]))) {
                                log_error("Failed to read physical volume %s",
                                          argv[opt]);
				continue;
			}
			pvdisplay_single(pv);
		}
	} else {
                log_verbose("Scanning for physical volume names");
                if (!(pvs = ios->get_pvs(ios)))
                        return ECMD_FAILED;

                list_for_each(pvh, pvs)
                        pvdisplay_single(&list_entry(pvh, struct pv_list,                                                            list)->pv);
	}

	return 0;
}

void pvdisplay_single(struct physical_volume *pv)
{
	char *sz;
        uint64_t size;

	char *pv_name = pv->dev->name;

	if (!*pv->vg_name)
		size = pv->size;
	else 
		size = (pv->pe_count - pv->pe_allocated) * pv->pe_size;

	if (arg_count(short_ARG)) {
		sz = display_size(size / 2, SIZE_SHORT);
		log_print("Device '%s' has a capacity of %s", pv_name, sz);

		dbg_free(sz);
		return;
	}

	if (pv->status & EXPORTED_VG) 
        	log_print("Physical volume '%s' of volume group '%s' "
			  "is exported" , pv_name, pv->vg_name);

/********* FIXME
        log_error("no physical volume identifier on \"%s\"" , pv_name);
*********/

	if (!pv->vg_name) {
        	log_print ( "'%s' is a new physical volume of %s",
                  	     pv_name, ( sz = display_size ( size / 2, 
								SIZE_SHORT)));
		dbg_free(sz);
	}

/* FIXME: Check active - no point?
      log_very_verbose("checking physical volume activity" );
         pv_check_active ( pv->vg_name, pv->pv_name)
         pv_status  ( pv->vg_name, pv->pv_name, &pv)
*/

/* FIXME: Check consistency - do this when reading metadata BUT trigger mesgs
      log_very_verbose("checking physical volume consistency" );
      ret = pv_check_consistency (pv)
*/

	if (arg_count(colon_ARG)) {
		pvdisplay_colons(pv);
		return;
	}

	pvdisplay_full(pv);

	if (!arg_count(verbose_ARG))
		return;

/******* FIXME
	if (pv->pe_allocated) {
		if (!(pv->pe = pv_read_pe(pv_name, pv)))
			goto pvdisplay_device_out;
		if (!(lvs = pv_read_lvs(pv))) {
			log_error("Failed to read LVs on %s", pv->pv_name);
			goto pvdisplay_device_out;
		}
		pv_display_pe_text(pv, pv->pe, lvs);
	} else
		log_print("no logical volume on physical volume %s", pv_name);
**********/

	return;
}

