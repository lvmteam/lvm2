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

void pvdisplay_single(struct cmd_context *cmd, struct physical_volume *pv,
		      void *handle)
{
	char *sz;
	uint64_t size;

	const char *pv_name = dev_name(pv->dev);

	if (!*pv->vg_name)
		size = pv->size;
	else
		size = (pv->pe_count - pv->pe_alloc_count) * pv->pe_size;

	if (arg_count(cmd, short_ARG)) {
		sz = display_size(size / 2, SIZE_SHORT);
		log_print("Device \"%s\" has a capacity of %s", pv_name, sz);
		dbg_free(sz);
		return;
	}

	if (pv->status & EXPORTED_VG)
		log_print("Physical volume \"%s\" of volume group \"%s\" "
			  "is exported", pv_name, pv->vg_name);

	if (!pv->vg_name) {
		log_print("\"%s\" is a new physical volume of \"%s\"",
			  pv_name, (sz = display_size(size / 2, SIZE_SHORT)));
		dbg_free(sz);
	}

	if (arg_count(cmd, colon_ARG)) {
		pvdisplay_colons(pv);
		return;
	}

	pvdisplay_full(pv, handle);

	if (!arg_count(cmd, maps_ARG))
		return;

	return;
}

int pvdisplay(struct cmd_context *cmd, int argc, char **argv)
{
	int opt = 0;

	struct list *pvh, *pvs;
	struct physical_volume *pv;

	if (arg_count(cmd, colon_ARG) && arg_count(cmd, maps_ARG)) {
		log_error("Option -v not allowed with option -c");
		return EINVALID_CMD_LINE;
	}

	if (argc) {
		log_very_verbose("Using physical volume(s) on command line");

		for (; opt < argc; opt++) {
			if (!(pv = pv_read(cmd, argv[opt], NULL, NULL))) {
				log_error("Failed to read physical "
					  "volume \"%s\"", argv[opt]);
				continue;
			}
			pvdisplay_single(cmd, pv, NULL);
		}
	} else {
		log_verbose("Scanning for physical volume names");
		if (!(pvs = get_pvs(cmd)))
			return ECMD_FAILED;

		list_iterate(pvh, pvs)
		    pvdisplay_single(cmd, list_item(pvh, struct pv_list)->pv,
				     NULL);
	}

	return 0;
}
