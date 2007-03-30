/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

static int _pvck_single(struct cmd_context * cmd,
			struct volume_group * vg,
			struct physical_volume * pv,
			void *handle)
{
	return ECMD_PROCESSED;
}

int pvck(struct cmd_context *cmd, int argc, char **argv)
{
	/* FIXME: Correlate findings of each PV */
	return process_each_pv(cmd, argc, argv, NULL, NULL, _pvck_single);
}
