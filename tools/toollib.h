/*
 * Copyright (C) 2001  Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 *
 */

#ifndef _LVM_TOOLLIB_H
#define _LVM_TOOLLIB_H

#include "metadata.h"
#include "pool.h"

int autobackup_set(void);
int autobackup_init(const char *backup_dir, int keep_days, int keep_number,
		    int autobackup);
int autobackup(struct volume_group *vg);

int process_each_vg(struct cmd_context *cmd, int argc, char **argv, int lock_type,
		    int (*process_single) (struct cmd_context *cmd, const char *vg_name));

int process_each_pv(struct cmd_context *cmd, int argc, char **argv, struct volume_group *vg,
		    int (*process_single) (struct cmd_context *cmd, struct volume_group * vg,
					   struct physical_volume * pv));
int process_each_lv(struct cmd_context *cmd, int argc, char **argv,
		    int (*process_single) (struct cmd_context *cmd, struct logical_volume * lv));


int process_each_pv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
		    int (*process_single) (struct cmd_context *cmd, struct volume_group * vg,
					   struct physical_volume * pv));
int process_each_lv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  int (*process_single) (struct cmd_context *cmd, struct logical_volume * lv));

int is_valid_chars(char *n);

char *default_vgname(struct format_instance *fi);
char *extract_vgname(struct format_instance *fi, char *lv_name);

/*
 * Builds a list of pv's from the names in argv.  Used in
 * lvcreate/extend.
 */
struct list *create_pv_list(struct pool *mem,
			    struct volume_group *vg,
			    int argc, char **argv);

#endif
