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

int lvactivate(int argc, char **argv)
{
	int p;

	struct io_space *ios;
	struct device *pv_dev;

	char *lv_name;
	char *pv_name;

	struct physical_volume *pv = NULL;
	struct logical_volume *lv = NULL;

	if (argc < 2) {
		log_error("please enter logical volume & physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	lv_name = argv[0];
	argc--;
	argv++;

	ios = active_ios();

	while (argc--) {
		pv_name = argv[argc];
		if (!(pv_dev = dev_cache_get(pv_name))) {
			log_error("device \"%s\" not found", pv_name);
			return -1;
		}

		if (!(pv = pv_read(ios, pv_dev))) {
			return -1;
		}

		if (pv->status & ALLOCATED_PV) {
			if (!(pv->pe = pv_read_pe(pv_name, pv)))
				goto pvdisplay_device_out;
			if (!(lvs = pv_read_lvs(pv))) {
				log_error("Failed to read LVs on %s",
					  pv->pv_name);
				goto pvdisplay_device_out;
			}
		} else
			log_print("no logical volume on physical volume %s",
				  pv_name);

		for (p = 0; p < pv->pe_total; p++) {
			int l = pv->pe[p].lv_num;
			int le = pv->pe[p].le_num;
			long pe_size_guess = lvs[l - 1].lv_size /
			    lvs[l - 1].lv_allocated_le;

			if (l && !strcmp(lv, lvs[l - 1].lv_name))
				printf("%012ld %ld linear %s %012ld\n",
				       pe_size_guess * le,
				       pe_size_guess,
				       pv_name, get_pe_offset(p, pv));
		}

		if (pv)
			dbg_free(pv->pe);
		dbg_free(pv);
		dbg_free(lvs);
	}

	return 0;

      pvdisplay_device_out:
	if (pv)
		dbg_free(pv->pe);
	dbg_free(pv);
	dbg_free(lvs);

	return -1;
}
