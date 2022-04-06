/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_DEVICE_ID_H
#define _LVM_DEVICE_ID_H

void free_du(struct dev_use *du);
void free_dus(struct dm_list *list);
void free_did(struct dev_id *did);
void free_dids(struct dm_list *list);
const char *idtype_to_str(uint16_t idtype);
uint16_t idtype_from_str(const char *str);
const char *dev_idtype_for_metadata(struct cmd_context *cmd, struct device *dev);
const char *dev_idname_for_metadata(struct cmd_context *cmd, struct device *dev);
int device_ids_use_devname(struct cmd_context *cmd);
int device_ids_read(struct cmd_context *cmd);
int device_ids_write(struct cmd_context *cmd);
int device_id_add(struct cmd_context *cmd, struct device *dev, const char *pvid,
                  const char *idtype_arg, const char *id_arg);
void device_id_pvremove(struct cmd_context *cmd, struct device *dev);
void device_ids_match(struct cmd_context *cmd);
int device_ids_match_dev(struct cmd_context *cmd, struct device *dev);
void device_ids_match_device_list(struct cmd_context *cmd);
void device_ids_validate(struct cmd_context *cmd, struct dm_list *scanned_devs, int *device_ids_invalid, int noupdate);
int device_ids_version_unchanged(struct cmd_context *cmd);
void device_ids_find_renamed_devs(struct cmd_context *cmd, struct dm_list *dev_list, int *search_count, int noupdate);
const char *device_id_system_read(struct cmd_context *cmd, struct device *dev, uint16_t idtype);
void device_id_update_vg_uuid(struct cmd_context *cmd, struct volume_group *vg, struct id *old_vg_id);

struct dev_use *get_du_for_devno(struct cmd_context *cmd, dev_t devno);
struct dev_use *get_du_for_dev(struct cmd_context *cmd, struct device *dev);
struct dev_use *get_du_for_pvid(struct cmd_context *cmd, const char *pvid);
struct dev_use *get_du_for_devname(struct cmd_context *cmd, const char *devname);
struct dev_use *get_du_for_device_id(struct cmd_context *cmd, uint16_t idtype, const char *idname);

char *devices_file_version(void);
int devices_file_exists(struct cmd_context *cmd);
int devices_file_touch(struct cmd_context *cmd);
int lock_devices_file(struct cmd_context *cmd, int mode);
int lock_devices_file_try(struct cmd_context *cmd, int mode, int *held);
void unlock_devices_file(struct cmd_context *cmd);

void devices_file_init(struct cmd_context *cmd);
void devices_file_exit(struct cmd_context *cmd);

void unlink_searched_devnames(struct cmd_context *cmd);

int read_sys_block(struct cmd_context *cmd, struct device *dev, const char *suffix, char *sysbuf, int sysbufsize);

int dev_has_mpath_uuid(struct cmd_context *cmd, struct device *dev, const char **idname_out);

#endif
