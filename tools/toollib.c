/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

#include <sys/stat.h>

int process_each_lv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  void *handle,
			  int (*process_single) (struct cmd_context * cmd,
						 struct logical_volume * lv,
						 void *handle))
{
	int ret_max = 0;
	int ret = 0;

	struct list *lvh;
	struct logical_volume *lv;

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg->name);
		return ECMD_FAILED;
	}

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		ret = process_single(cmd, lv, handle);
		if (ret > ret_max)
			ret_max = ret;
	}

	return ret_max;

}

struct volume_group *recover_vg(struct cmd_context *cmd, const char *vgname,
				int lock_type)
{
	int consistent = 1;

	lock_type &= ~LCK_TYPE_MASK;
	lock_type |= LCK_WRITE;

	if (!lock_vol(cmd, vgname, lock_type)) {
		log_error("Can't lock %s for metadata recovery: skipping",
			  vgname);
		return NULL;
	}

	return vg_read(cmd, vgname, &consistent);
}

int process_each_lv(struct cmd_context *cmd, int argc, char **argv,
		    int lock_type, void *handle,
		    int (*process_single) (struct cmd_context * cmd,
					   struct logical_volume * lv,
					   void *handle))
{
	int opt = 0;
	int ret_max = 0;
	int ret = 0;
	int vg_count = 0;
	int consistent;

	struct list *slh, *vgnames;
	struct volume_group *vg;
	struct logical_volume *lv;
	struct lv_list *lvl;

	const char *vgname;

	if (argc) {
		log_verbose("Using logical volume(s) on command line");
		for (; opt < argc; opt++) {
			char *lv_name = argv[opt];
			int vgname_provided = 1;

			/* Do we have a vgname or lvname? */
			vgname = lv_name;
			if (!strncmp(vgname, cmd->dev_dir,
				     strlen(cmd->dev_dir)))
				vgname += strlen(cmd->dev_dir);
			if (strchr(vgname, '/')) {
				/* Must be an LV */
				vgname_provided = 0;
				if (!(vgname = extract_vgname(cmd, lv_name))) {
					if (ret_max < ECMD_FAILED)
						ret_max = ECMD_FAILED;
					continue;
				}
			}

			log_verbose("Finding volume group \"%s\"", vgname);
			if (!lock_vol(cmd, vgname, lock_type)) {
				log_error("Can't lock %s: skipping", vgname);
				continue;
			}
			if (lock_type & LCK_WRITE)
				consistent = 1;
			else
				consistent = 0;
			if (!(vg = vg_read(cmd, vgname, &consistent)) ||
			    !consistent) {
				unlock_vg(cmd, vgname);
				if (!vg)
					log_error("Volume group \"%s\" "
						  "not found", vgname);
				else
					log_error("Volume group \"%s\" "
						  "inconsistent", vgname);
				if (!vg || !(vg =
					     recover_vg(cmd, vgname,
							lock_type))) {
					unlock_vg(cmd, vgname);
					if (ret_max < ECMD_FAILED)
						ret_max = ECMD_FAILED;
					continue;
				}
			}

			if (vg->status & EXPORTED_VG) {
				log_error("Volume group \"%s\" is exported",
					  vg->name);
				unlock_vg(cmd, vgname);
				return ECMD_FAILED;
			}

			if (vgname_provided) {
				if ((ret =
				     process_each_lv_in_vg(cmd, vg, handle,
							   process_single)) >
				    ret_max)
					ret_max = ret;
				unlock_vg(cmd, vgname);
				continue;
			}

			if (!(lvl = find_lv_in_vg(vg, lv_name))) {
				log_error("Can't find logical volume \"%s\" "
					  "in volume group \"%s\"",
					  lv_name, vgname);
				if (ret_max < ECMD_FAILED)
					ret_max = ECMD_FAILED;
				unlock_vg(cmd, vgname);
				continue;
			}

			lv = lvl->lv;

			if ((ret = process_single(cmd, lv, handle)) > ret_max)
				ret_max = ret;
			unlock_vg(cmd, vgname);
		}
	} else {
		log_verbose("Finding all logical volumes");
		if (!(vgnames = get_vgs(cmd, 0))) {
			log_error("No volume groups found");
			return ECMD_FAILED;
		}
		list_iterate(slh, vgnames) {
			vgname = list_item(slh, struct str_list)->str;
			if (!vgname || !*vgname)
				continue;	/* FIXME Unnecessary? */
			if (!lock_vol(cmd, vgname, lock_type)) {
				log_error("Can't lock %s: skipping", vgname);
				continue;
			}
			if (lock_type & LCK_WRITE)
				consistent = 1;
			else
				consistent = 0;
			if (!(vg = vg_read(cmd, vgname, &consistent)) ||
			    !consistent) {
				unlock_vg(cmd, vgname);
				if (!vg)
					log_error("Volume group \"%s\" "
						  "not found", vgname);
				else
					log_error("Volume group \"%s\" "
						  "inconsistent", vgname);
				if (!vg || !(vg =
					     recover_vg(cmd, vgname,
							lock_type))) {
					unlock_vg(cmd, vgname);
					if (ret_max < ECMD_FAILED)
						ret_max = ECMD_FAILED;
					continue;
				}
			}
			ret = process_each_lv_in_vg(cmd, vg, handle,
						    process_single);
			unlock_vg(cmd, vgname);
			if (ret > ret_max)
				ret_max = ret;
			vg_count++;
		}
	}

	return ret_max;
}

int process_each_segment_in_lv(struct cmd_context *cmd,
			       struct logical_volume *lv,
			       void *handle,
			       int (*process_single) (struct cmd_context * cmd,
						      struct lv_segment * seg,
						      void *handle))
{
	struct list *segh;
	struct lv_segment *seg;
	int ret_max = 0;
	int ret;

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct lv_segment);
		ret = process_single(cmd, seg, handle);
		if (ret > ret_max)
			ret_max = ret;
	}

	return ret_max;
}

int process_each_vg(struct cmd_context *cmd, int argc, char **argv,
		    int lock_type, int consistent, void *handle,
		    int (*process_single) (struct cmd_context * cmd,
					   const char *vg_name,
					   struct volume_group * vg,
					   int consistent, void *handle))
{
	int opt = 0;
	int ret_max = 0;
	int ret = 0;

	struct list *slh, *vgnames;
	struct volume_group *vg;

	char *vg_name;
	char *dev_dir = cmd->dev_dir;

	if (argc) {
		log_verbose("Using volume group(s) on command line");
		for (; opt < argc; opt++) {
			vg_name = argv[opt];
			if (!strncmp(vg_name, dev_dir, strlen(dev_dir)))
				vg_name += strlen(dev_dir);
			if (strchr(vg_name, '/')) {
				log_error("Invalid volume group name: %s",
					  vg_name);
				continue;
			}
			if (!lock_vol(cmd, vg_name, lock_type)) {
				log_error("Can't lock %s: skipping", vg_name);
				continue;
			}
			log_verbose("Finding volume group \"%s\"", vg_name);
			vg = vg_read(cmd, vg_name, &consistent);
			if ((ret = process_single(cmd, vg_name, vg, consistent,
						  handle))
			    > ret_max)
				ret_max = ret;
			unlock_vg(cmd, vg_name);
		}
	} else {
		log_verbose("Finding all volume groups");
		if (!(vgnames = get_vgs(cmd, 0)) || list_empty(vgnames)) {
			log_error("No volume groups found");
			return ECMD_FAILED;
		}
		list_iterate(slh, vgnames) {
			vg_name = list_item(slh, struct str_list)->str;
			if (!vg_name || !*vg_name)
				continue;	/* FIXME Unnecessary? */
			if (!lock_vol(cmd, vg_name, lock_type)) {
				log_error("Can't lock %s: skipping", vg_name);
				continue;
			}
			log_verbose("Finding volume group \"%s\"", vg_name);
			vg = vg_read(cmd, vg_name, &consistent);
			ret = process_single(cmd, vg_name, vg, consistent,
					     handle);
			if (ret > ret_max)
				ret_max = ret;
			unlock_vg(cmd, vg_name);
		}
	}

	return ret_max;
}

int process_each_pv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  void *handle,
			  int (*process_single) (struct cmd_context * cmd,
						 struct volume_group * vg,
						 struct physical_volume * pv,
						 void *handle))
{
	int ret_max = 0;
	int ret = 0;
	struct list *pvh;
	struct physical_volume *pv;

	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;

		if ((ret = process_single(cmd, vg, pv, handle)) > ret_max)
			ret_max = ret;
	}

	return ret_max;
}

int process_each_pv(struct cmd_context *cmd, int argc, char **argv,
		    struct volume_group *vg, void *handle,
		    int (*process_single) (struct cmd_context * cmd,
					   struct volume_group * vg,
					   struct physical_volume * pv,
					   void *handle))
{
	int opt = 0;
	int ret_max = 0;
	int ret = 0;

	struct pv_list *pvl;
	struct physical_volume *pv;
	struct list *pvslist, *pvh;

	if (argc) {
		log_verbose("Using physical volume(s) on command line");
		for (; opt < argc; opt++) {
			if (vg) {
				if (!(pvl = find_pv_in_vg(vg, argv[opt]))) {
					log_error("Physical Volume \"%s\" not "
						  "found in Volume Group "
						  "\"%s\"", argv[opt],
						  vg->name);
					continue;
				}
				pv = pvl->pv;
			} else {
				if (!(pv = pv_read(cmd, argv[opt], NULL, NULL))) {
					log_error("Failed to read physical "
						  "volume \"%s\"", argv[opt]);
					continue;
				}
			}

			ret = process_single(cmd, vg, pv, handle);
			if (ret > ret_max)
				ret_max = ret;
		}
	} else {
		if (vg) {
			log_verbose("Using all physical volume(s) in "
				    "volume group");
			process_each_pv_in_vg(cmd, vg, handle, process_single);
		} else {
			log_verbose("Scanning for physical volume names");
			if (!(pvslist = get_pvs(cmd)))
				return ECMD_FAILED;

			list_iterate(pvh, pvslist) {
				pv = list_item(pvh, struct pv_list)->pv;
				ret = process_single(cmd, NULL, pv, handle);
				if (ret > ret_max)
					ret_max = ret;
			}
		}
	}

	return ret_max;
}

const char *extract_vgname(struct cmd_context *cmd, const char *lv_name)
{
	const char *vg_name = lv_name;
	char *st;
	char *dev_dir = cmd->dev_dir;

	/* Path supplied? */
	if (vg_name && strchr(vg_name, '/')) {
		/* Strip dev_dir (optional) */
		if (!strncmp(vg_name, dev_dir, strlen(dev_dir)))
			vg_name += strlen(dev_dir);

		/* Require exactly one slash */
		/* FIXME But allow for consecutive slashes */
		if (!(st = strchr(vg_name, '/')) || (strchr(st + 1, '/'))) {
			log_error("\"%s\": Invalid path for Logical Volume",
				  lv_name);
			return 0;
		}

		vg_name = pool_strdup(cmd->mem, vg_name);
		if (!vg_name) {
			log_error("Allocation of vg_name failed");
			return 0;
		}

		*strchr(vg_name, '/') = '\0';
		return vg_name;
	}

	if (!(vg_name = default_vgname(cmd))) {
		if (lv_name)
			log_error("Path required for Logical Volume \"%s\"",
				  lv_name);
		return 0;
	}

	return vg_name;
}

char *default_vgname(struct cmd_context *cmd)
{
	char *vg_path;
	char *dev_dir = cmd->dev_dir;

	/* Take default VG from environment? */
	vg_path = getenv("LVM_VG_NAME");
	if (!vg_path)
		return 0;

	/* Strip dev_dir (optional) */
	if (!strncmp(vg_path, dev_dir, strlen(dev_dir)))
		vg_path += strlen(dev_dir);

	if (strchr(vg_path, '/')) {
		log_error("Environment Volume Group in LVM_VG_NAME invalid: "
			  "\"%s\"", vg_path);
		return 0;
	}

	return pool_strdup(cmd->mem, vg_path);
}

static int _add_alloc_area(struct pool *mem, struct list *alloc_areas,
			   uint32_t start, uint32_t count)
{
	struct alloc_area *aa;
	struct list *aah;

	log_debug("Adding alloc area: start PE %" PRIu32 " length %" PRIu32,
		  start, count);

	/* Ensure no overlap with existing areas */
	list_iterate(aah, alloc_areas) {
		aa = list_item(aah, struct alloc_area);
		if (((start < aa->start) && (start + count - 1 >= aa->start)) ||
		    ((start >= aa->start) &&
		     (aa->start + aa->count - 1) >= start)) {
			log_error("Overlapping PE ranges detected (%" PRIu32
				  "-%" PRIu32 ", %" PRIu32 "-%" PRIu32 ")",
				  start, start + count - 1, aa->start,
				  aa->start + aa->count - 1);
			return 0;
		}
	}

	if (!(aa = pool_alloc(mem, sizeof(*aa)))) {
		log_error("Allocation of list failed");
		return 0;
	}

	aa->start = start;
	aa->count = count;
	list_add(alloc_areas, &aa->list);

	return 1;
}

static int _parse_pes(struct pool *mem, char *c, struct list *alloc_areas,
		      uint32_t size)
{
	char *endptr;
	uint32_t start, end;

	/* Default to whole PV */
	if (!c) {
		if (!_add_alloc_area(mem, alloc_areas, UINT32_C(0), size)) {
			stack;
			return 0;
		}
		return 1;
	}

	while (*c) {
		if (*c != ':')
			goto error;

		c++;

		/* Disallow :: and :\0 */
		if (*c == ':' || !*c)
			goto error;

		/* Default to whole range */
		start = UINT32_C(0);
		end = size - 1;

		/* Start extent given? */
		if (isdigit(*c)) {
			start = (uint32_t) strtoul(c, &endptr, 10);
			if (endptr == c)
				goto error;
			c = endptr;
			/* Just one number given? */
			if (!*c || *c == ':')
				end = start;
		}
		/* Range? */
		if (*c == '-') {
			c++;
			if (isdigit(*c)) {
				end = (uint32_t) strtoul(c, &endptr, 10);
				if (endptr == c)
					goto error;
				c = endptr;
			}
		}
		if (*c && *c != ':')
			goto error;

		if ((start > end) || (end > size - 1)) {
			log_error("PE range error: start extent %" PRIu32 " to "
				  "end extent %" PRIu32, start, end);
			return 0;
		}

		if (!_add_alloc_area(mem, alloc_areas, start, end - start + 1)) {
			stack;
			return 0;
		}

	}

	return 1;

      error:
	log_error("Physical extent parsing error at %s", c);
	return 0;
}

struct list *create_pv_list(struct pool *mem,
			    struct volume_group *vg, int argc, char **argv)
{
	struct list *r;
	struct list *alloc_areas;
	struct pv_list *pvl, *new_pvl;
	char *pvname = NULL, *colon;
	int i;

	/* Build up list of PVs */
	if (!(r = pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed");
		return NULL;
	}
	list_init(r);

	for (i = 0; i < argc; i++) {
		if ((colon = strchr(argv[i], ':'))) {
			if (!(pvname = pool_strndup(mem, argv[i],
						    colon - argv[i]))) {
				log_error("Failed to clone PV name");
				return NULL;
			}
		} else
			pvname = argv[i];

		if (!(pvl = find_pv_in_vg(vg, pvname))) {
			log_err("Physical Volume \"%s\" not found in "
				"Volume Group \"%s\"", pvname, vg->name);
			return NULL;
		}

		if (!(pvl->pv->status & ALLOCATABLE_PV)) {
			log_error("Physical volume %s not allocatable", pvname);
			continue;
		}

		if (pvl->pv->pe_count == pvl->pv->pe_alloc_count) {
			log_err("No free extents on physical volume \"%s\"",
				pvname);
			continue;
		}

		if (!(new_pvl = pool_alloc(mem, sizeof(*new_pvl)))) {
			log_err("Unable to allocate physical volume list.");
			return NULL;
		}

		memcpy(new_pvl, pvl, sizeof(*new_pvl));
		list_add(r, &new_pvl->list);

		if (!(alloc_areas = pool_alloc(mem, sizeof(*alloc_areas)))) {
			log_error("Allocation of alloc_areas list failed");
			return NULL;
		}
		list_init(alloc_areas);

		/* Specify which physical extents may be used for allocation */
		if (!_parse_pes(mem, colon, alloc_areas, pvl->pv->pe_count)) {
			stack;
			return NULL;
		}
		new_pvl->alloc_areas = alloc_areas;
	}

	return list_empty(r) ? NULL : r;
}

struct list *clone_pv_list(struct pool *mem, struct list *pvsl)
{
	struct list *r, *pvh;
	struct pv_list *pvl, *new_pvl;

	/* Build up list of PVs */
	if (!(r = pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed");
		return NULL;
	}
	list_init(r);

	list_iterate(pvh, pvsl) {
		pvl = list_item(pvh, struct pv_list);

		if (!(new_pvl = pool_zalloc(mem, sizeof(*new_pvl)))) {
			log_error("Unable to allocate physical volume list.");
			return NULL;
		}

		memcpy(new_pvl, pvl, sizeof(*new_pvl));
		list_add(r, &new_pvl->list);
	}

	return r;
}
