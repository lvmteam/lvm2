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

void pvscan_display_single(struct physical_volume *pv);

int pv_max_name_len = 0;
int vg_max_name_len = 0;

int pvscan(int argc, char **argv)
{
	int new_pvs_found = 0;
	int pvs_found = 0;
	char *s1, *s2, *s3;

	struct pv_list *pvs_list, *pvl;
	struct list_head *pvh;
	struct physical_volume *pv;

	uint64_t size_total = 0;
	uint64_t size_new = 0;
	uint64_t size = 0;

	int len = 0;
	pv_max_name_len = 0;
	vg_max_name_len = 0;

	if (arg_count(novolumegroup_ARG) && arg_count(exported_ARG)) {
		log_error("options e and n incompatible");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(exported_ARG) || arg_count(novolumegroup_ARG))
		log_print("WARNING: only considering physical volumes %s",
			  arg_count(exported_ARG) ?
			  "of exported volume group(s)" : "in no volume group");

	log_verbose("Walking through all physical volumes");

	if (!(pvs_list = ios->get_pvs(ios)))
		return ECMD_FAILED;

	/* eliminate exported/new if required */
	list_for_each(pvh, &pvs_list->list) {
		pvl = list_entry(pvh, struct pv_list, list);
		pv = &pvl->pv;

		if ((arg_count(exported_ARG) && !(pv->status & STATUS_EXPORTED))
		    || (arg_count(novolumegroup_ARG) && (pv->vg_name[0]))) {
			list_del(&pvl->list);	/* Plus deallocation? */
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

		size = dev_get_size(pv->dev);
		size_total += size;

		if (pv->vg_name[0]) {
			new_pvs_found++;
			size_new += size;
		}
	}

	/* find maximum pv name length */
	pv_max_name_len = vg_max_name_len = 0;
	list_for_each(pvh, &pvs_list->list) {
		pvl = list_entry(pvh, struct pv_list, list);
		pv = &pvl->pv;
		len = strlen(pv->dev->name);
		if (pv_max_name_len < len)
			pv_max_name_len = len;
		len = strlen(pv->vg_name);
		if (vg_max_name_len < len)
			vg_max_name_len = len;
	}
	pv_max_name_len += 2;
	vg_max_name_len += 2;

	list_for_each(pvh, &pvs_list->list) {
		pvl = list_entry(pvh, struct pv_list, list);
		pv = &pvl->pv;

		pvscan_display_single(pv);
	}

	if (!pvs_found) {
		log_print("No matching physical volumes found");
		return 0;
	}

	log_print("total: %d [%s] / in use: %d [%s] / in no VG: %d [%s]",
		  pvs_found,
		  (s1 = display_size(size_total / 2, SIZE_SHORT)),
		  pvs_found - new_pvs_found,
		  (s2 =
		   display_size((size_total - size_new) / 2, SIZE_SHORT)),
		  new_pvs_found, (s3 = display_size(size_new / 2, SIZE_SHORT)));
	dbg_free(s1);
	dbg_free(s2);
	dbg_free(s3);

	return 0;
}

void pvscan_display_single(struct physical_volume *pv)
{

	int vg_name_len = 0;
	const char *active_str;

	char *s1, *s2;

	char pv_tmp_name[NAME_LEN] = { 0, };
	char vg_tmp_name[NAME_LEN] = { 0, };
	char vg_name_this[NAME_LEN] = { 0, };

	/* short listing? */
	if (arg_count(short_ARG) > 0) {
		log_print("%s", pv->dev->name);
		return;
	}

	if (arg_count(verbose_ARG) > 1) {
		pv_show(pv);
		log_print("System Id             %s", pv->exported);
		log_print("");
		return;
	}

	memset(pv_tmp_name, 0, sizeof (pv_tmp_name));

	active_str = (pv->status & ACTIVE) ? "ACTIVE   " : "inactive ";

	vg_name_len = strlen(pv->vg_name) - sizeof (EXPORTED) + 1;

	if (arg_count(uuid_ARG)) {
		sprintf(pv_tmp_name,
			"%-*s with UUID %s",
			pv_max_name_len - 2,
			pv->dev->name, display_uuid(pv->id->uuid));
	} else {
		sprintf(pv_tmp_name, "%s", pv->dev.name);
	}

	if (!pv->vg_name[0]) {
		log_print("%s PV %-*s is in no VG  [%s]", active_str,
			  pv_max_name_len, pv_tmp_name,
			  (s1 = display_size(pv->size / 2, SIZE_SHORT)));
		dbg_free(s1);
		return;
	}

	/* FIXME What is pe_total now? */
	if (strcmp(&pv->vg_name[vg_name_len], EXPORTED_TAG) == 0) {
		strncpy(vg_name_this, pv->vg_name, vg_name_len);
		log_print("%sPV %-*s  is in EXPORTED VG %s [%s / %s free]",
			  active_str, pv_max_name_len, pv_tmp_name,
			  vg_name_this, (s1 =
					 display_size(pv->pe_total *
						      pv->pe_size / 2,
						      SIZE_SHORT)),
			  (s2 = display_size((pv->pe_total - pv->pe_allocated)
					     * pv->pe_size / 2, SIZE_SHORT)));
		dbg_free(s1);
		dbg_free(s2);
		return;
	}

	if (!vg_check_name(pv->vg_name)) {
		log_print
		    ("%sPV %-*s  is associated to an unknown VG ",
		     active_str, pv_max_name_len, pv_tmp_name);
		return;
	}

	sprintf(vg_tmp_name, "%s", pv->vg_name);
	log_print
	    ("%sPV %-*s of VG %-*s [%s / %s free]", active_str, pv_max_name_len,
	     pv_tmp_name, vg_max_name_len, vg_tmp_name,
	     (s1 = display_size(pv->pe_total * pv->pe_size / 2, SIZE_SHORT)),
	     (s2 =
	      display_size((pv->pe_total - pv->pe_allocated) * pv->pe_size / 2,
			   SIZE_SHORT)));
	dbg_free(s1);
	dbg_free(s2);

	return;
}
