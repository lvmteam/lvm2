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

int lvdisplay(int argc, char **argv)
{
	/* FIXME Allow VG args via process_each */

	if (arg_count(colon_ARG) && arg_count(verbose_ARG)) {
		log_error("Options -v and -c are incompatible");
		return EINVALID_CMD_LINE;
	}

	return process_each_lv(argc, argv, &lvdisplay_single);
}

int lvdisplay_single(struct logical_volume *lv)
{
	if (arg_count(colon_ARG))
		lvdisplay_colons(lv);
	else {
		lvdisplay_full(lv);
		if (arg_count(maps_ARG))
			lvdisplay_extents(lv);
	}

	return 0;
}
