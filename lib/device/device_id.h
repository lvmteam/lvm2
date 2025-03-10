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

#include "lib/commands/toolcontext.h"
#include "lib/device/device.h"

void free_du(struct dev_use *du);
void free_dus(struct dm_list *dus);
void free_did(struct dev_id *did);
void free_dids(struct dm_list *ids);
const char *idtype_to_str(uint16_t idtype);
uint16_t idtype_from_str(const char *str);
const char *dev_idtype_for_metadata(struct cmd_context *cmd, struct device *dev);
const char *dev_idname_for_metadata(struct cmd_context *cmd, struct device *dev);
int device_ids_use_devname(struct cmd_context *cmd);
int device_ids_use_lvmlv(struct cmd_context *cmd);
int device_ids_read(struct cmd_context *cmd);
int device_ids_write(struct cmd_context *cmd);
int device_id_add(struct cmd_context *cmd, struct device *dev, const char *pvid,
                  const char *idtype_arg, const char *id_arg, int use_idtype_only);
void device_id_pvremove(struct cmd_context *cmd, struct device *dev);
void device_id_lvremove(struct cmd_context *cmd, struct dm_list *removed_uuids);
void device_ids_match(struct cmd_context *cmd);
int device_ids_match_dev(struct cmd_context *cmd, struct device *dev);
void device_ids_match_device_list(struct cmd_context *cmd);
void device_ids_validate(struct cmd_context *cmd, struct dm_list *scanned_devs, int using_hints,
			int noupdate, int *update_needed);
void device_ids_check_serial(struct cmd_context *cmd, struct dm_list *scan_devs,
			int noupdate, int *update_needed);
void device_ids_search(struct cmd_context *cmd, struct dm_list *new_devs,
		       int all_ids, int noupdate, int *update_needed);
char *device_id_system_read(struct cmd_context *cmd, struct device *dev, uint16_t idtype);
void device_id_update_vg_uuid(struct cmd_context *cmd, struct volume_group *vg, struct id *old_vg_id);
int device_ids_version_unchanged(struct cmd_context *cmd);

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
int read_sys_block_binary(struct cmd_context *cmd, struct device *dev,
			  const char *suffix, char *sysbuf, int sysbufsize, int *retlen);

int dev_has_mpath_uuid(struct cmd_context *cmd, struct device *dev, char **idname_out);

int scsi_type_to_idtype(int scsi_type);
int nvme_type_to_idtype(int nvme_type);
int idtype_to_scsi_type(int idtype);
int idtype_to_nvme_type(int idtype);
void free_wwids(struct dm_list *ids);
struct dev_wwid *dev_add_wwid(char *id, int dw_type, int is_nvme, struct dm_list *ids);
struct dev_wwid *dev_add_scsi_wwid(char *id, int dw_type, struct dm_list *ids);
struct dev_wwid *dev_add_nvme_wwid(char *id, int dw_type, struct dm_list *ids);
int dev_read_vpd_wwids(struct cmd_context *cmd, struct device *dev);
void dev_read_nvme_wwids(struct device *dev);
int dev_read_sys_wwid(struct cmd_context *cmd, struct device *dev,
		      char *buf, int bufsize, struct dev_wwid **dw_out);

int pv_device_id_is_stale(const struct physical_volume *pv);

#endif
