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
#include "metadata.h"

int pvremove(struct cmd_context *cmd, int argc, char **argv)
{
	int i;
	int ret = ECMD_PROCESSED;
	unsigned force_count;
	unsigned prompt;

	if (!argc) {
		log_error("Please enter a physical volume path");
		return EINVALID_CMD_LINE;
	}

	force_count = arg_count(cmd, force_ARG);
	prompt = arg_count(cmd, yes_ARG);

	for (i = 0; i < argc; i++) {
		dm_unescape_colons_and_at_signs(argv[i], NULL, NULL);
		if (!pvremove_single(cmd, argv[i], NULL, force_count, prompt)) {
			stack;
			ret = ECMD_FAILED;
		}
	}

	return ret;
}
