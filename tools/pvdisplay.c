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

int pvdisplay_single(struct cmd_context *cmd, struct volume_group *vg,
		     struct physical_volume *pv, void *handle)
{
	uint64_t size;

	const char *pv_name = dev_name(pv->dev);

	if (!*pv->vg_name)
		size = pv->size;
	else
		size = (pv->pe_count - pv->pe_alloc_count) * pv->pe_size;

	if (arg_count(cmd, short_ARG)) {
		log_print("Device \"%s\" has a capacity of %s", pv_name,
			  display_size(cmd, size / 2, SIZE_SHORT));
		return 0;
	}

	if (pv->status & EXPORTED_VG)
		log_print("Physical volume \"%s\" of volume group \"%s\" "
			  "is exported", pv_name, pv->vg_name);

	if (!pv->vg_name)
		log_print("\"%s\" is a new physical volume of \"%s\"",
			  pv_name, display_size(cmd, size / 2, SIZE_SHORT));

	if (arg_count(cmd, colon_ARG)) {
		pvdisplay_colons(pv);
		return 0;
	}

	pvdisplay_full(cmd, pv, handle);

	if (!arg_count(cmd, maps_ARG))
		return 0;

	return 0;
}

int pvdisplay(struct cmd_context *cmd, int argc, char **argv)
{
	if (arg_count(cmd, columns_ARG)) {
		if (arg_count(cmd, colon_ARG) || arg_count(cmd, maps_ARG) ||
		    arg_count(cmd, short_ARG)) {
			log_error("Incompatible options selected");
			return EINVALID_CMD_LINE;
		}
		return pvs(cmd, argc, argv);
	} else if (arg_count(cmd, aligned_ARG) ||
		   arg_count(cmd, noheadings_ARG) ||
		   arg_count(cmd, options_ARG) ||
		   arg_count(cmd, separator_ARG) ||
		   arg_count(cmd, sort_ARG) || arg_count(cmd, unbuffered_ARG)) {
		log_error("Incompatible options selected");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, colon_ARG) && arg_count(cmd, maps_ARG)) {
		log_error("Option -v not allowed with option -c");
		return EINVALID_CMD_LINE;
	}

	process_each_pv(cmd, argc, argv, NULL, NULL, pvdisplay_single);

	return 0;
}
