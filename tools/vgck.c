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

static int vgck_single(const char *vg_name);

int vgck(int argc, char **argv)
{
	return process_each_vg(argc, argv, LCK_READ, &vgck_single);
}

static int vgck_single(const char *vg_name)
{
	struct volume_group *vg;

	log_verbose("Checking volume group \"%s\"", vg_name);

	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Volume group \"%s\" not found", vg_name);
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg_name);
		return ECMD_FAILED;
	}

/******* FIXME Must be caught and logged by vg_read
	log_error("not all physical volumes of volume group \"%s\" online",
	log_error("volume group \"%s\" has physical volumes with ",
		  "invalid version",
********/			     

	/* FIXME: free */
	return 0;
}

