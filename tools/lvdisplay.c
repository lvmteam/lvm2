/*
 * Copyright (C) 2001  Sistina Software
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

int lvdisplay_single(char *lv_name);
int lvdisplay(int argc, char **argv)
{
	int ret = 0;
	int ret_max = 0;
	int opt = 0;

	/* FIXME Allow VG / all arguments via a process_each? */
	if (!argc) {
		log_error("Please enter one or more logical volume paths");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(colon_ARG) && arg_count(verbose_ARG)) {
		log_error("Options -v and -c are incompatible");
		return EINVALID_CMD_LINE;
	}

	for (opt = 0; opt < argc; opt++) {
		ret = lvdisplay_single(argv[opt]);
		if (ret > ret_max)
			ret_max = ret;
	}

	return ret_max;
}

int lvdisplay_single(char *lv_name)
{
	char *vg_name = NULL;

	struct volume_group *vg;
	struct list *lvh;
	struct logical_volume *lv;

	/* does VG exist? */
	if (!(vg_name = extract_vgname(fid, lv_name))) {
		return ECMD_FAILED;
	}

	log_verbose("Finding volume group %s", vg_name);
	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Volume group %s doesn't exist", vg_name);
		return ECMD_FAILED;
	}

	if (!(lvh = find_lv_in_vg(vg, lv_name))) {
		log_error("Can't find logical volume %s in volume group %s",
			  lv_name, vg_name);
		return ECMD_FAILED;
	}

	lv = &list_item(lvh, struct lv_list)->lv;

	if (arg_count(colon_ARG))
		lvdisplay_colons(lv);
	else {
		lvdisplay_full(lv);
		if (arg_count(verbose_ARG))
			lvdisplay_extents(lv);
	}

	return 0;
}
