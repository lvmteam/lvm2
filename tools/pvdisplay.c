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

void pvdisplay_device(const char *pv_dev);

int pvdisplay(int argc, char **argv)
{
	int opt;

	if (argc == 0) {
		log_error("please enter a physical volume path");
		return LVM_EINVALID_CMD_LINE;
	}

	if (arg_count(colon_ARG) && arg_count(verbose_ARG)) {
		log_error("option v not allowed with option c");
		return LVM_EINVALID_CMD_LINE;
	}

	for (opt = 0; opt < argc; opt++)
		pvdisplay_device(argv[opt]);

	putchar('\n');
	return 0;
}

void pvdisplay_device(const char *pv_name)
{
	struct dev_mgr *dm;
	struct device *pv_dev;

	pv_t *pv = NULL;
	lv_disk_t *lvs = NULL;

	dm = active_dev_mgr();

	if (!(pv_dev = dev_by_name(dm, pv_name))) {
		log_error("device \"%s\" not found", pv_name);
		return;
	}

	if (arg_count(short_ARG)) {
		int size;
		char *sz;

		/* Returns size in 512-byte units */
		if ((size = device_get_size(pv_name)) < 0) {
			log_error("%s: getting size of physical volume \"%s\"", 
			  	strerror(size), pv_name);
			return;
		}

		sz = display_size(size / 2, SIZE_SHORT);
		log_print("Device \"%s\" has a capacity of %s",
			  pv_name, sz);

		dbg_free(sz);
	}

	if (!(pv = pv_read(dm, pv_name))) {
		return;
	}

/* FIXME: Check attributes
	MD_DEVICE, 
        log_error("\"%s\" no VALID physical volume \"%s\"", lvm_error ( ret), pv_name);

	EXPORTED
        pv->vg_name[strlen(pv->vg_name)-strlen(EXPORTED)] = 0;
        log_print("physical volume \"%s\" of volume group \"%s\" is exported" , pv_name, pv->vg_name);

	Valid ID
        log_error("no physical volume identifier on \"%s\"" , pv_name);

	NEW
        pv_check_new (pv)
        log_print ( "\"%s\" is a new physical volume of %s",
                  pv_name, ( dummy = lvm_show_size ( size / 2, SHORT)));
*/

/* FIXME: Check active - no point?
      log_very_verbose("checking physical volume activity" );
         pv_check_active ( pv->vg_name, pv->pv_name)
         pv_status  ( pv->vg_name, pv->pv_name, &pv)
*/

/* FIXME: Check consistency - or do this when reading metadata? 
      log_very_verbose("checking physical volume consistency" );
      ret = pv_check_consistency (pv)
      log_error("\"%s\" checking consistency of physical volume \"%s\"", lvm_error ( ret), pv_name);
*/

	if (arg_count(colon_ARG)) {
		pv_display_colons(pv);
		goto pvdisplay_device_out;
	}

        pv_display_full(pv);

	if (!arg_count(verbose_ARG)) 
		goto pvdisplay_device_out;

	if (pv->pe_allocated) {
		if (!(pv->pe = pv_read_pe (pv_name, pv)))
                  	goto pvdisplay_device_out;
		if (!(lvs = pv_read_lvs(pv))) {
	                log_error("Failed to read LVs on %s", pv->pv_name);
			goto pvdisplay_device_out;
		}
               	pv_display_pe_text(pv, pv->pe, lvs);
	} else
               	log_print ("no logical volume on physical volume %s", pv_name);

      pvdisplay_device_out:
	if (pv)
		dbg_free(pv->pe);
	dbg_free(pv);
	dbg_free(lvs);

	return;
}



