/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

int pvremove(struct cmd_context *cmd, int argc, char **argv)
{
	int i;
	unsigned force_count;
	unsigned prompt;
	struct dm_list pv_names;

	if (!argc) {
		log_error("Please enter a physical volume path");
		return EINVALID_CMD_LINE;
	}

	force_count = arg_count(cmd, force_ARG);
	prompt = arg_count(cmd, yes_ARG);

	dm_list_init(&pv_names);

	for (i = 0; i < argc; i++) {
		dm_unescape_colons_and_at_signs(argv[i], NULL, NULL);
		if (!str_list_add(cmd->mem, &pv_names, argv[i]))
			return_ECMD_FAILED;
	}

	if (!pvremove_many(cmd, &pv_names, force_count, prompt))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}
