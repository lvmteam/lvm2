/*
 * Copyright (C) 2003  Sistina Software
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

static int _vgmknodes_single(struct cmd_context *cmd, struct logical_volume *lv,
			     void *handle)
{
	if (!lv_mknodes(cmd, lv))
		return ECMD_FAILED;

	return ECMD_PROCESSED;
}

int vgmknodes(struct cmd_context *cmd, int argc, char **argv)
{
	int r;

	r = process_each_lv(cmd, argc, argv, LCK_VG_READ, NULL,
			    &_vgmknodes_single);

	if (!lv_mknodes(cmd, NULL) && (r < ECMD_FAILED))
		r = ECMD_FAILED;

	return r;
}
