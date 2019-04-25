/*
 * Copyright (C) 2004-2018 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _LVM_HINTS_H
#define _LVM_HINTS_H

struct hint {
	struct dm_list list;
	char name[PATH_MAX];
	char pvid[ID_LEN + 1];
	char vgname[NAME_LEN];
	dev_t devt;
	unsigned chosen:1; /* this hint's dev was chosen for scanning */
};

int write_hint_file(struct cmd_context *cmd, int newhints);

void clear_hint_file(struct cmd_context *cmd);

void invalidate_hints(struct cmd_context *cmd);

int get_hints(struct cmd_context *cmd, struct dm_list *hints, int *newhints,
              struct dm_list *devs_in, struct dm_list *devs_out);

int validate_hints(struct cmd_context *cmd, struct dm_list *hints);

void hints_exit(struct cmd_context *cmd);

void pvscan_recreate_hints_begin(struct cmd_context *cmd);

#endif

