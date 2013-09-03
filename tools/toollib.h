/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved. 
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_TOOLLIB_H
#define _LVM_TOOLLIB_H

#include "metadata-exported.h"

int become_daemon(struct cmd_context *cmd, int skip_lvm);

int autobackup_set(void);
int autobackup_init(const char *backup_dir, int keep_days, int keep_number,
		    int autobackup);
int autobackup(struct volume_group *vg);

struct volume_group *recover_vg(struct cmd_context *cmd, const char *vgname,
				uint32_t lock_type);

typedef int (*process_single_vg_fn_t) (struct cmd_context * cmd,
				       const char *vg_name,
				       struct volume_group * vg,
				       void *handle);
typedef int (*process_single_pv_fn_t) (struct cmd_context *cmd,
				  struct volume_group *vg,
				  struct physical_volume *pv,
				  void *handle);
typedef int (*process_single_lv_fn_t) (struct cmd_context *cmd,
				  struct logical_volume *lv,
				  void *handle);
typedef int (*process_single_seg_fn_t) (struct cmd_context * cmd,
					struct lv_segment * seg,
					void *handle);
typedef int (*process_single_pvseg_fn_t) (struct cmd_context * cmd,
					  struct volume_group * vg,
					  struct pv_segment * pvseg,
					  void *handle);

int process_each_vg(struct cmd_context *cmd, int argc, char **argv,
		    uint32_t flags, void *handle,
		    process_single_vg_fn_t process_single_vg);

int process_each_pv(struct cmd_context *cmd, int argc, char **argv,
		    struct volume_group *vg, uint32_t lock_type,
		    int scan_label_only, void *handle,
		    process_single_pv_fn_t process_single_pv);

int process_each_segment_in_pv(struct cmd_context *cmd,
			       struct volume_group *vg,
			       struct physical_volume *pv,
			       void *handle,
			       process_single_pvseg_fn_t process_single_pvseg);

int process_each_lv(struct cmd_context *cmd, int argc, char **argv,
		    uint32_t flags, void *handle,
		    process_single_lv_fn_t process_single_lv);


int process_each_segment_in_lv(struct cmd_context *cmd,
			       struct logical_volume *lv, void *handle,
			       process_single_seg_fn_t process_single_seg);

int process_each_pv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  const struct dm_list *tags, void *handle,
			  process_single_pv_fn_t process_single_pv);


int process_each_lv_in_vg(struct cmd_context *cmd,
			  struct volume_group *vg,
			  const struct dm_list *arg_lvnames,
			  const struct dm_list *tags,
			  struct dm_list *failed_lvnames,
			  void *handle,
			  process_single_lv_fn_t process_single_lv);

char *default_vgname(struct cmd_context *cmd);
const char *extract_vgname(struct cmd_context *cmd, const char *lv_name);
const char *skip_dev_dir(struct cmd_context *cmd, const char *vg_name,
			 unsigned *dev_dir_found);

/*
 * Builds a list of pv's from the names in argv.  Used in
 * lvcreate/extend.
 */
struct dm_list *create_pv_list(struct dm_pool *mem, struct volume_group *vg, int argc,
			    char **argv, int allocatable_only);

struct dm_list *clone_pv_list(struct dm_pool *mem, struct dm_list *pvs);

void vgcreate_params_set_defaults(struct vgcreate_params *vp_def,
				 struct volume_group *vg);
int vgcreate_params_set_from_args(struct cmd_context *cmd,
				  struct vgcreate_params *vp_new,
				  struct vgcreate_params *vp_def);
int lv_change_activate(struct cmd_context *cmd, struct logical_volume *lv,
		       activation_change_t activate);
int lv_refresh(struct cmd_context *cmd, struct logical_volume *lv);
int vg_refresh_visible(struct cmd_context *cmd, struct volume_group *vg);
void lv_spawn_background_polling(struct cmd_context *cmd,
				 struct logical_volume *lv);
int pvcreate_params_validate(struct cmd_context *cmd,
			     int argc, char **argv,
			     struct pvcreate_params *pp);

int get_activation_monitoring_mode(struct cmd_context *cmd,
				   int *monitoring_mode);

int get_pool_params(struct cmd_context *cmd,
		    struct profile *profile,
		    int *passed_args,
		    uint32_t *chunk_size,
		    thin_discards_t *discards,
		    uint64_t *pool_metadata_size,
		    int *zero);

int get_stripe_params(struct cmd_context *cmd, uint32_t *stripes,
		      uint32_t *stripe_size);

int change_tag(struct cmd_context *cmd, struct volume_group *vg,
	       struct logical_volume *lv, struct physical_volume *pv, int arg);

#endif
