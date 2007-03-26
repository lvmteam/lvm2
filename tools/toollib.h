/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved. 
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_TOOLLIB_H
#define _LVM_TOOLLIB_H

#include "metadata.h"

int autobackup_set(void);
int autobackup_init(const char *backup_dir, int keep_days, int keep_number,
		    int autobackup);
int autobackup(struct volume_group *vg);

struct volume_group *recover_vg(struct cmd_context *cmd, const char *vgname,
				int lock_type);

int process_each_vg(struct cmd_context *cmd, int argc, char **argv,
		    int lock_type, int consistent, void *handle,
		    int (*process_single) (struct cmd_context * cmd,
					   const char *vg_name,
					   struct volume_group * vg,
					   int consistent, void *handle));

int process_each_pv(struct cmd_context *cmd, int argc, char **argv,
		    struct volume_group *vg, void *handle,
		    int (*process_single) (struct cmd_context * cmd,
					   struct volume_group * vg,
					   struct physical_volume * pv,
					   void *handle));
int process_each_segment_in_pv(struct cmd_context *cmd,
			       struct volume_group *vg,
			       struct physical_volume *pv,
			       void *handle,
			       int (*process_single) (struct cmd_context * cmd,
						      struct volume_group * vg,
						      struct pv_segment * pvseg,
						      void *handle));

int process_each_lv(struct cmd_context *cmd, int argc, char **argv,
		    int lock_type, void *handle,
		    int (*process_single) (struct cmd_context * cmd,
					   struct logical_volume * lv,
					   void *handle));

int process_each_segment_in_lv(struct cmd_context *cmd,
			       struct logical_volume *lv, void *handle,
			       int (*process_single) (struct cmd_context * cmd,
						      struct lv_segment * seg,
						      void *handle));

int process_each_pv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  struct list *tags, void *handle,
			  int (*process_single) (struct cmd_context * cmd,
						 struct volume_group * vg,
						 struct physical_volume * pv,
						 void *handle));

int process_each_lv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  struct list *arg_lvnames, struct list *tags,
			  void *handle,
			  int (*process_single) (struct cmd_context * cmd,
						 struct logical_volume * lv,
						 void *handle));

char *default_vgname(struct cmd_context *cmd);
const char *extract_vgname(struct cmd_context *cmd, const char *lv_name);
char *skip_dev_dir(struct cmd_context *cmd, const char *vg_name,
		   unsigned *dev_dir_found);

/*
 * Builds a list of pv's from the names in argv.  Used in
 * lvcreate/extend.
 */
struct list *create_pv_list(struct dm_pool *mem, struct volume_group *vg, int argc,
			    char **argv, int allocatable_only);

struct list *clone_pv_list(struct dm_pool *mem, struct list *pvs);

int apply_lvname_restrictions(const char *name);

int validate_vg_name(struct cmd_context *cmd, const char *vg_name);

int generate_log_name_format(struct volume_group *vg, const char *lv_name,
                             char *buffer, size_t size);

struct logical_volume *create_mirror_log(struct cmd_context *cmd,
					 struct volume_group *vg,
					 struct alloc_handle *ah,
					 alloc_policy_t alloc,
					 const char *lv_name,
					 int in_sync,
					 struct list *tags);

int set_lv(struct cmd_context *cmd, struct logical_volume *lv,
	   uint64_t sectors, int value);

#endif
