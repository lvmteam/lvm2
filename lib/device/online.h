/*
 * Copyright (C) 2021 Red Hat, Inc. All rights reserved.
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

#ifndef _ONLINE_H
#define _ONLINE_H

struct pv_online {
	struct dm_list list;
	struct device *dev;
	dev_t devno;
	char pvid[ID_LEN + 1];
	char vgname[NAME_LEN];
	char devname[NAME_LEN];
};

/*
 * Avoid a duplicate pvscan[%d] prefix when logging to the journal.
 * FIXME: this should probably replace if (udevoutput) with
 * if (log_journal & LOG_JOURNAL_OUTPUT)
 */
#define log_print_pvscan(cmd, fmt, args...) \
do \
	if (cmd->udevoutput) \
		log_print_unless_silent(fmt, ##args); \
	else \
		log_print_unless_silent("pvscan[%d] " fmt, getpid(), ##args); \
while (0)

#define log_error_pvscan(cmd, fmt, args...) \
do \
	if (cmd->udevoutput) \
		log_error(fmt, ##args); \
	else \
		log_error("pvscan[%d] " fmt, getpid(), ##args); \
while (0)

int online_pvid_file_read(char *path, int *major, int *minor, char *vgname, char *devname);
int online_vg_file_create(struct cmd_context *cmd, const char *vgname);
void online_vg_file_remove(const char *vgname);
int online_pvid_file_create(struct cmd_context *cmd, struct device *dev, const char *vgname);
int online_pvid_file_exists(const char *pvid);
void online_dir_setup(struct cmd_context *cmd);
int get_pvs_online(struct dm_list *pvs_online, const char *vgname);
int get_pvs_lookup(struct dm_list *pvs_online, const char *vgname);
void free_po_list(struct dm_list *list);
void online_lookup_file_remove(const char *vgname);
void online_vgremove(struct volume_group *vg);

#endif
