/*
 * Copyright (C) 2025 Red Hat, Inc. All rights reserved.
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

#ifndef _PERSIST_H
#define _PERSIST_H

#define PR_STR_WE   "WE"
#define PR_STR_EA   "EA"
#define PR_STR_WERO "WERO"
#define PR_STR_EARO "EARO"
#define PR_STR_WEAR "WEAR"
#define PR_STR_EAAR "EAAR"

#define PR_TYPE_WE   1
#define PR_TYPE_EA   2
#define PR_TYPE_WERO 3
#define PR_TYPE_EARO 4
#define PR_TYPE_WEAR 5
#define PR_TYPE_EAAR 6

int persist_check(struct cmd_context *cmd, struct volume_group *vg,
                  char *local_key, int local_host_id);

int persist_read(struct cmd_context *cmd, struct volume_group *vg);

int persist_start(struct cmd_context *cmd, struct volume_group *vg,
		  char *local_key, int local_host_id, const char *remkey);

int persist_stop(struct cmd_context *cmd, struct volume_group *vg);
int persist_stop_prepare(struct cmd_context *cmd, struct volume_group *vg, struct dm_list *devs, char **key);
int persist_stop_run(struct cmd_context *cmd, struct volume_group *vg, struct dm_list *devs, char *key);

int persist_remove(struct cmd_context *cmd, struct volume_group *vg,
                   char *local_key, int local_host_id, const char *remkey);

int persist_clear(struct cmd_context *cmd, struct volume_group *vg,
                  char *local_key, int local_host_id);

int persist_start_extend(struct cmd_context *cmd, struct volume_group *vg);

int persist_is_started(struct cmd_context *cmd, struct volume_group *vg, int may_fail);

int persist_key_update(struct cmd_context *cmd, struct volume_group *vg, uint32_t prev_gen);

void persist_key_file_remove(struct cmd_context *cmd, struct volume_group *vg);
void persist_key_file_rename(const char *old_name, const char *new_name);

int dev_read_reservation_nvme(struct cmd_context *cmd, struct device *dev, uint64_t *holder_ret, int *prtype_ret);

int dev_find_key(struct cmd_context *cmd, struct device *dev, int may_fail,
                 uint64_t find_key, int *found_key,
                 int find_host_id, uint64_t *found_host_id_key,
                 int find_all, int *found_count, uint64_t **found_all);

int dev_find_key_nvme(struct cmd_context *cmd, struct device *dev, int may_fail,
                      uint64_t find_key, int *found_key,
                      int find_host_id, uint64_t *found_host_id_key,
                      int find_all, int *found_count, uint64_t **found_all);

int vg_is_registered(struct cmd_context *cmd, struct volume_group *vg, uint64_t *our_key_ret, int *partial_ret);

#endif
