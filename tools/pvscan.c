/*
 * Copyright (C) 2001 Sistina Software
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

int pv_max_name_len = 0;
int vg_max_name_len = 0;

static void _pvscan_display_single(struct cmd_context *cmd,
				   struct physical_volume *pv, void *handle)
{
	char uuid[64];
	unsigned int vg_name_len = 0;

	char pv_tmp_name[NAME_LEN] = { 0, };
	char vg_tmp_name[NAME_LEN] = { 0, };
	char vg_name_this[NAME_LEN] = { 0, };

	/* short listing? */
	if (arg_count(cmd, short_ARG) > 0) {
		log_print("%s", dev_name(pv->dev));
		return;
	}

	if (arg_count(cmd, verbose_ARG) > 1) {
		/* FIXME As per pv_display! Drop through for now. */
		/* pv_show(pv); */

		/* FIXME - Moved to Volume Group structure */
		/* log_print("System Id             %s", pv->vg->system_id); */

		/* log_print(" "); */
		/* return; */
	}

	memset(pv_tmp_name, 0, sizeof(pv_tmp_name));

	vg_name_len = strlen(pv->vg_name) + 1;

	if (arg_count(cmd, uuid_ARG)) {
		if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
			stack;
			return;
		}

		sprintf(pv_tmp_name, "%-*s with UUID %s",
			pv_max_name_len - 2, dev_name(pv->dev), uuid);
	} else {
		sprintf(pv_tmp_name, "%s", dev_name(pv->dev));
	}

	if (!*pv->vg_name) {
		log_print("PV %-*s    %-*s %s [%s]",
			  pv_max_name_len, pv_tmp_name,
			  vg_max_name_len, " ",
			  pv->fmt ? pv->fmt->name : "    ",
			  display_size(cmd, pv->size / 2, SIZE_SHORT));
		return;
	}

	if (pv->status & EXPORTED_VG) {
		strncpy(vg_name_this, pv->vg_name, vg_name_len);
		log_print("PV %-*s  is in exported VG %s "
			  "[%s / %s free]",
			  pv_max_name_len, pv_tmp_name,
			  vg_name_this,
			  display_size(cmd, (uint64_t) pv->pe_count *
				       pv->pe_size / 2, SIZE_SHORT),
			  display_size(cmd, (uint64_t) (pv->pe_count -
							pv->pe_alloc_count)
				       * pv->pe_size / 2, SIZE_SHORT));
		return;
	}

	sprintf(vg_tmp_name, "%s", pv->vg_name);
	log_print
	    ("PV %-*s VG %-*s %s [%s / %s free]", pv_max_name_len,
	     pv_tmp_name, vg_max_name_len, vg_tmp_name,
	     pv->fmt ? pv->fmt->name : "    ",
	     display_size(cmd, (uint64_t) pv->pe_count * pv->pe_size / 2,
			  SIZE_SHORT), display_size(cmd, (uint64_t)
						    (pv->pe_count -
						     pv->pe_alloc_count) *
						    pv->pe_size / 2,
						    SIZE_SHORT));
	return;
}

int pvscan(struct cmd_context *cmd, int argc, char **argv)
{
	int new_pvs_found = 0;
	int pvs_found = 0;

	struct list *pvslist;
	struct list *pvh;
	struct pv_list *pvl;
	struct physical_volume *pv;

	uint64_t size_total = 0;
	uint64_t size_new = 0;

	int len = 0;
	pv_max_name_len = 0;
	vg_max_name_len = 0;

	if (arg_count(cmd, novolumegroup_ARG) && arg_count(cmd, exported_ARG)) {
		log_error("Options -e and -n are incompatible");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, exported_ARG) || arg_count(cmd, novolumegroup_ARG))
		log_print("WARNING: only considering physical volumes %s",
			  arg_count(cmd, exported_ARG) ?
			  "of exported volume group(s)" : "in no volume group");

	log_verbose("Wiping cache of LVM-capable devices");
	persistent_filter_wipe(cmd->filter);

	log_verbose("Wiping internal cache");
	lvmcache_destroy();

	log_verbose("Walking through all physical volumes");
	if (!(pvslist = get_pvs(cmd)))
		return ECMD_FAILED;

	/* eliminate exported/new if required */
	list_iterate(pvh, pvslist) {
		pvl = list_item(pvh, struct pv_list);
		pv = pvl->pv;

		if ((arg_count(cmd, exported_ARG)
		     && !(pv->status & EXPORTED_VG))
		    || (arg_count(cmd, novolumegroup_ARG) && (*pv->vg_name))) {
			list_del(&pvl->list);
			continue;
		}

		/* Also check for MD use? */
/*******
		if (MAJOR(pv_create_kdev_t(pv[p]->pv_name)) != MD_MAJOR) {
			log_print
			    ("WARNING: physical volume \"%s\" belongs to a meta device",
			     pv[p]->pv_name);
		}
		if (MAJOR(pv[p]->pv_dev) != MD_MAJOR)
			continue;
********/
		pvs_found++;

		if (!*pv->vg_name) {
			new_pvs_found++;
			size_new += pv->size;
			size_total += pv->size;
		} else
			size_total += (pv->pe_count - pv->pe_alloc_count)
			    * pv->pe_size;
	}

	/* find maximum pv name length */
	pv_max_name_len = vg_max_name_len = 0;
	list_iterate(pvh, pvslist) {
		pv = list_item(pvh, struct pv_list)->pv;
		len = strlen(dev_name(pv->dev));
		if (pv_max_name_len < len)
			pv_max_name_len = len;
		len = strlen(pv->vg_name);
		if (vg_max_name_len < len)
			vg_max_name_len = len;
	}
	pv_max_name_len += 2;
	vg_max_name_len += 2;

	list_iterate(pvh, pvslist)
	    _pvscan_display_single(cmd, list_item(pvh, struct pv_list)->pv,
				   NULL);

	if (!pvs_found) {
		log_print("No matching physical volumes found");
		return 0;
	}

	log_print("Total: %d [%s] / in use: %d [%s] / in no VG: %d [%s]",
		  pvs_found,
		  display_size(cmd, size_total / 2, SIZE_SHORT),
		  pvs_found - new_pvs_found,
		  display_size(cmd, (size_total - size_new) / 2, SIZE_SHORT),
		  new_pvs_found, display_size(cmd, size_new / 2, SIZE_SHORT));

	return 0;
}
