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

int dumpconfig(struct cmd_context *cmd, int argc, char **argv)
{
	const char *file = NULL;

	if (argc == 1)
		file = argv[0];

	if (argc > 1) {
		log_error("Please specify one file for output");
		return EINVALID_CMD_LINE;
	}

	if (!write_config_file(cmd->cft, file))
		return ECMD_FAILED;

	return ECMD_PROCESSED;
}
