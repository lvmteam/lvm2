/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

#include <sys/stat.h>

int dir_exists(const char *dir)
{
	struct stat info;

	if (!*dir)
		return 1;

	if (stat(dir, &info) != -1) {
		log_error("%s exists", dir);
		return 0;
	}

	return 1;
}

int create_dir(const char *dir)
{
	struct stat info;

	if (!*dir)
		return 1;

	if (stat(dir, &info) < 0) {
		log_verbose("Creating directory %s", dir);
		if (!mkdir(dir, 0777))
			return 1;
		log_sys_error("mkdir", dir);
		return 0;
	}

	if (S_ISDIR(info.st_mode))
		return 1;

	log_error("Directory %s not found", dir);
	return 0;
}

int process_each_lv_in_vg(struct volume_group *vg,
			  int (*process_single) (struct logical_volume *lv))
{
	int ret_max = 0;
	int ret = 0;

	struct list *lvh;
	struct logical_volume *lv;

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group %s is exported", vg->name);
		return ECMD_FAILED;
	}

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		ret = process_single(lv);
		if (ret > ret_max)
			ret_max = ret;
	}

	return ret_max;

}

int process_each_lv(int argc, char **argv,
		    int (*process_single) (struct logical_volume *lv))
{
	int opt = 0;
	int ret_max = 0;
	int ret = 0;
	int vg_count = 0;

	struct list *vgh, *vgs;
	struct volume_group *vg;
	struct logical_volume *lv;
	struct lv_list *lvl;

	char *vg_name;

	if (argc) {
		log_verbose("Using logical volume(s) on command line");
		for (; opt < argc; opt++) {
			char *lv_name = argv[opt];

			/* does VG exist? */
			if (!(vg_name = extract_vgname(fid, lv_name))) {
				if (ret_max < ECMD_FAILED)
					ret_max = ECMD_FAILED;
				continue;
			}

			log_verbose("Finding volume group %s", vg_name);
			if (!(vg = fid->ops->vg_read(fid, vg_name))) {
				log_error("Volume group %s doesn't exist",
					  vg_name);
				if (ret_max < ECMD_FAILED)
					ret_max = ECMD_FAILED;
				continue;
			}

			if (vg->status & EXPORTED_VG) {
				log_error("Volume group %s is exported",
					  vg->name);
				return ECMD_FAILED;
			}

			if (!(lvl = find_lv_in_vg(vg, lv_name))) {
				log_error("Can't find logical volume %s "
					  "in volume group %s",
					  lv_name, vg_name);
				if (ret_max < ECMD_FAILED)
					ret_max = ECMD_FAILED;
				continue;
			}

			lv = lvl->lv;

			if ((ret = process_single(lv)) > ret_max)
				ret_max = ret;
		}
	} else {
		log_verbose("Finding all logical volumes");
		if (!(vgs = fid->ops->get_vgs(fid))) {
			log_error("No volume groups found");
			return ECMD_FAILED;
		}
		list_iterate(vgh, vgs) {
			vg_name = list_item(vgh, struct name_list)->name;
			if (!(vg = fid->ops->vg_read(fid, vg_name))) {
				log_error("Volume group %s not found", vg_name);
				if (ret_max < ECMD_FAILED)
					ret_max = ECMD_FAILED;
				continue;
			}
			ret = process_each_lv_in_vg(vg, process_single);
			if (ret > ret_max)
				ret_max = ret;
			vg_count++;
		}
	}

	return ret_max;
}

int process_each_vg(int argc, char **argv,
		    int (*process_single) (const char *vg_name))
{
	int opt = 0;
	int ret_max = 0;
	int ret = 0;

	struct list *vgh;
	struct list *vgs;

	if (argc) {
		log_verbose("Using volume group(s) on command line");
		for (; opt < argc; opt++)
			if ((ret = process_single(argv[opt])) > ret_max)
				ret_max = ret;
	} else {
		log_verbose("Finding all volume groups");
		if (!(vgs = fid->ops->get_vgs(fid))) {
			log_error("No volume groups found");
			return ECMD_FAILED;
		}
		list_iterate(vgh, vgs) {
			ret =
			    process_single(list_item
					   (vgh, struct name_list)->name);
			if (ret > ret_max)
				ret_max = ret;
		}
	}

	return ret_max;
}

int process_each_pv_in_vg(struct volume_group *vg,
			  int (*process_single) (struct volume_group *vg,
						 struct physical_volume *pv))
{
	int ret_max = 0;
	int ret = 0;
	struct list *pvh;
	struct physical_volume *pv;

	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;

		if ((ret = process_single(vg, pv)) > ret_max)
			ret_max = ret;
	}

	return ret_max;
}

int process_each_pv(int argc, char **argv, struct volume_group *vg,
		    int (*process_single) (struct volume_group *vg,
					   struct physical_volume *pv))
{
	int opt = 0;
	int ret_max = 0;
	int ret = 0;

	struct pv_list *pvl;

	if (argc) {
		log_verbose("Using physical volume(s) on command line");
		for (; opt < argc; opt++) {
			if (!(pvl = find_pv_in_vg(vg, argv[opt]))) {
				log_error("Physical Volume %s not found in "
					  "Volume Group %s", argv[opt],
					  vg->name);
				continue;
			}
			ret = process_single(vg, pvl->pv);
			if (ret > ret_max)
				ret_max = ret;
		}
	} else {
		log_verbose("Using all physical volume(s) in volume group");
		process_each_pv_in_vg(vg, process_single);
	}

	return ret_max;
}

int is_valid_chars(char *n)
{
	register char c;
	while ((c = *n++))
		if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+')
			return 0;
	return 1;
}

char *extract_vgname(struct format_instance *fi, char *lv_name)
{
	char *vg_name = lv_name;
	char *st;
	char *dev_dir = fi->cmd->dev_dir;

	/* Path supplied? */
	if (vg_name && strchr(vg_name, '/')) {
		/* Strip dev_dir (optional) */
		if (!strncmp(vg_name, dev_dir, strlen(dev_dir)))
			vg_name += strlen(dev_dir);

		/* Require exactly one slash */
		/* FIXME But allow for consecutive slashes */
		if (!(st = strchr(vg_name, '/')) || (strchr(st + 1, '/'))) {
			log_error("%s: Invalid path for Logical Volume",
				  lv_name);
			return 0;
		}

		vg_name = pool_strdup(fid->cmd->mem, vg_name);
		if (!vg_name) {
			log_error("Allocation of vg_name failed");
			return 0;
		}

		*strchr(vg_name, '/') = '\0';
		return vg_name;
	}

	if (!(vg_name = default_vgname(fid))) {
		if (lv_name)
			log_error("Path required for Logical Volume %s",
				  lv_name);
		return 0;
	}

	return vg_name;
}

char *default_vgname(struct format_instance *fi)
{
	char *vg_path;
	char *dev_dir = fi->cmd->dev_dir;

	/* Take default VG from environment? */
	vg_path = getenv("LVM_VG_NAME");
	if (!vg_path)
		return 0;

	/* Strip dev_dir (optional) */
	if (!strncmp(vg_path, dev_dir, strlen(dev_dir)))
		vg_path += strlen(dev_dir);

	if (strchr(vg_path, '/')) {
		log_error("Environment Volume Group in LVM_VG_NAME invalid: %s",
			  vg_path);
		return 0;
	}

	return pool_strdup(fid->cmd->mem, vg_path);
}

struct list *create_pv_list(struct pool *mem,
			    struct volume_group *vg, int argc, char **argv)
{
	struct list *r;
	struct pv_list *pvl, *new_pvl;
	int i;

	/* Build up list of PVs */
	if (!(r = pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed");
		return NULL;
	}
	list_init(r);

	for (i = 0; i < argc; i++) {

		if (!(pvl = find_pv_in_vg(vg, argv[i]))) {
			log_err("Physical Volume %s not found in "
				"Volume Group %s", argv[i], vg->name);
			return NULL;
		}

		if (pvl->pv->pe_count == pvl->pv->pe_allocated) {
			log_err("No free extents on physical volume %s",
				argv[i]);
			continue;
		}

		if (!(new_pvl = pool_alloc(mem, sizeof(*new_pvl)))) {
			log_err("Unable to allocate physical volume list.");
			return NULL;
		}

		memcpy(new_pvl, pvl, sizeof(*new_pvl));
		list_add(r, &new_pvl->list);
	}

	return list_empty(r) ? NULL : r;
}
