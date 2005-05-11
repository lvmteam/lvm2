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

#include "tools.h"

#include <sys/stat.h>
#include <sys/wait.h>

/*
 * Metadata iteration functions
 */
int process_each_lv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  struct list *arg_lvnames, struct list *tags,
			  void *handle,
			  int (*process_single) (struct cmd_context * cmd,
						 struct logical_volume * lv,
						 void *handle))
{
	int ret_max = 0;
	int ret = 0;
	int process_all = 0;
	int process_lv = 0;
	int tags_supplied = 0;
	int lvargs_supplied = 0;
	int lvargs_matched = 0;

	struct lv_list *lvl;

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg->name);
		return ECMD_FAILED;
	}

	if (tags && !list_empty(tags))
		tags_supplied = 1;

	if (arg_lvnames && !list_empty(arg_lvnames))
		lvargs_supplied = 1;

	/* Process all LVs in this VG if no restrictions given */
	if (!tags_supplied && !lvargs_supplied)
		process_all = 1;

	/* Or if VG tags match */
	if (!process_lv && tags_supplied &&
	    str_list_match_list(tags, &vg->tags)) {
		process_all = 1;
	}

	list_iterate_items(lvl, &vg->lvs) {
		if (lvl->lv->status & SNAPSHOT)
			continue;

		/* Should we process this LV? */
		if (process_all)
			process_lv = 1;
		else
			process_lv = 0;

		/* LV tag match? */
		if (!process_lv && tags_supplied &&
		    str_list_match_list(tags, &lvl->lv->tags)) {
			process_lv = 1;
		}

		/* LV name match? */
		if (lvargs_supplied &&
		    str_list_match_item(arg_lvnames, lvl->lv->name)) {
			process_lv = 1;
			lvargs_matched++;
		}

		if (!process_lv)
			continue;

		ret = process_single(cmd, lvl->lv, handle);
		if (ret > ret_max)
			ret_max = ret;
	}

	if (lvargs_supplied && lvargs_matched != list_size(arg_lvnames)) {
		log_error("One or more specified logical volume(s) not found.");
		if (ret_max < ECMD_FAILED)
			ret_max = ECMD_FAILED;
	}

	return ret_max;
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
	int consistent;

	struct list *slh, *tags_arg;
	struct list *vgnames;	/* VGs to process */
	struct str_list *sll;
	struct volume_group *vg;
	struct list tags, lvnames;
	struct list arg_lvnames;	/* Cmdline vgname or vgname/lvname */
	char *vglv;
	size_t vglv_sz;

	const char *vgname;

	list_init(&tags);
	list_init(&arg_lvnames);

	if (argc) {
		struct list arg_vgnames;

		log_verbose("Using logical volume(s) on command line");
		list_init(&arg_vgnames);

		for (; opt < argc; opt++) {
			const char *lv_name = argv[opt];
			char *vgname_def;
			int dev_dir_found = 0;

			/* Do we have a tag or vgname or lvname? */
			vgname = lv_name;

			if (*vgname == '@') {
				if (!validate_name(vgname + 1)) {
					log_error("Skipping invalid tag %s",
						  vgname);
					continue;
				}
				if (!str_list_add(cmd->mem, &tags,
						  pool_strdup(cmd->mem,
							      vgname + 1))) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
				continue;
			}

			/* FIXME Jumbled parsing */
			if (*vgname == '/') {
				while (*vgname == '/')
					vgname++;
				vgname--;
			}
			if (!strncmp(vgname, cmd->dev_dir,
				     strlen(cmd->dev_dir))) {
				vgname += strlen(cmd->dev_dir);
				dev_dir_found = 1;
				while (*vgname == '/')
					vgname++;
			}
			if (*vgname == '/') {
				log_error("\"%s\": Invalid path for Logical "
					  "Volume", argv[opt]);
				if (ret_max < ECMD_FAILED)
					ret_max = ECMD_FAILED;
				continue;
			}
			lv_name = vgname;
			if (strchr(vgname, '/')) {
				/* Must be an LV */
				lv_name = strchr(vgname, '/');
				while (*lv_name == '/')
					lv_name++;
				if (!(vgname = extract_vgname(cmd, vgname))) {
					if (ret_max < ECMD_FAILED)
						ret_max = ECMD_FAILED;
					continue;
				}
			} else if (!dev_dir_found &&
				   (vgname_def = default_vgname(cmd))) {
				vgname = vgname_def;
			} else
				lv_name = NULL;

			if (!str_list_add(cmd->mem, &arg_vgnames,
					  pool_strdup(cmd->mem, vgname))) {
				log_error("strlist allocation failed");
				return ECMD_FAILED;
			}

			if (!lv_name) {
				if (!str_list_add(cmd->mem, &arg_lvnames,
						  pool_strdup(cmd->mem,
							      vgname))) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
			} else {
				vglv_sz = strlen(vgname) + strlen(lv_name) + 2;
				if (!(vglv = pool_alloc(cmd->mem, vglv_sz)) ||
				    lvm_snprintf(vglv, vglv_sz, "%s/%s", vgname,
						 lv_name) < 0) {
					log_error("vg/lv string alloc failed");
					return ECMD_FAILED;
				}
				if (!str_list_add(cmd->mem, &arg_lvnames, vglv)) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
			}
		}
		vgnames = &arg_vgnames;
	}

	if (!argc || !list_empty(&tags)) {
		log_verbose("Finding all logical volumes");
		if (!(vgnames = get_vgs(cmd, 0)) || list_empty(vgnames)) {
			log_error("No volume groups found");
			return ret_max;
		}
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
		if (!(vg = vg_read(cmd, vgname, &consistent)) || !consistent) {
			unlock_vg(cmd, vgname);
			if (!vg)
				log_error("Volume group \"%s\" "
					  "not found", vgname);
			else
				log_error("Volume group \"%s\" "
					  "inconsistent", vgname);
			if (!vg || !(vg = recover_vg(cmd, vgname, lock_type))) {
				unlock_vg(cmd, vgname);
				if (ret_max < ECMD_FAILED)
					ret_max = ECMD_FAILED;
				continue;
			}
		}

		tags_arg = &tags;
		list_init(&lvnames);	/* LVs to be processed in this VG */
		list_iterate_items(sll, &arg_lvnames) {
			const char *vg_name = sll->str;
			const char *lv_name = strchr(vg_name, '/');

			if ((!lv_name && !strcmp(vg_name, vgname))) {
				/* Process all LVs in this VG */
				tags_arg = NULL;
				list_init(&lvnames);
				break;
			} else if (!strncmp(vg_name, vgname, strlen(vgname)) &&
				   strlen(vgname) == lv_name - vg_name) {
				if (!str_list_add(cmd->mem, &lvnames,
						  pool_strdup(cmd->mem,
							      lv_name + 1))) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
			}
		}

		ret = process_each_lv_in_vg(cmd, vg, &lvnames, tags_arg,
					    handle, process_single);
		unlock_vg(cmd, vgname);
		if (ret > ret_max)
			ret_max = ret;
	}

	return ret_max;
}

int process_each_segment_in_pv(struct cmd_context *cmd,
			       struct volume_group *vg,
			       struct physical_volume *pv,
			       void *handle,
			       int (*process_single) (struct cmd_context * cmd,
						      struct volume_group * vg,
						      struct pv_segment * pvseg,
						      void *handle))
{
	struct pv_segment *pvseg;
	int ret_max = 0;
	int ret;

	list_iterate_items(pvseg, &pv->segments) {
		ret = process_single(cmd, vg, pvseg, handle);
		if (ret > ret_max)
			ret_max = ret;
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
	struct lv_segment *seg;
	int ret_max = 0;
	int ret;

	list_iterate_items(seg, &lv->segments) {
		ret = process_single(cmd, seg, handle);
		if (ret > ret_max)
			ret_max = ret;
	}

	return ret_max;
}

static int _process_one_vg(struct cmd_context *cmd, const char *vg_name,
			   struct list *tags, struct list *arg_vgnames,
			   int lock_type, int consistent, void *handle,
			   int ret_max,
			   int (*process_single) (struct cmd_context * cmd,
						  const char *vg_name,
						  struct volume_group * vg,
						  int consistent, void *handle))
{
	struct volume_group *vg;
	int ret = 0;

	if (!lock_vol(cmd, vg_name, lock_type)) {
		log_error("Can't lock %s: skipping", vg_name);
		return ret_max;
	}

	log_verbose("Finding volume group \"%s\"", vg_name);
	vg = vg_read(cmd, vg_name, &consistent);

	if (!list_empty(tags)) {
		/* Only process if a tag matches or it's on arg_vgnames */
		if (!str_list_match_item(arg_vgnames, vg_name) &&
		    !str_list_match_list(tags, &vg->tags)) {
			unlock_vg(cmd, vg_name);
			return ret_max;
		}
	}

	if ((ret = process_single(cmd, vg_name, vg, consistent,
				  handle)) > ret_max) {
		ret_max = ret;
	}

	unlock_vg(cmd, vg_name);

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

	struct str_list *sl;
	struct list *vgnames;
	struct list arg_vgnames, tags;

	const char *vg_name;
	char *dev_dir = cmd->dev_dir;

	list_init(&tags);
	list_init(&arg_vgnames);

	if (argc) {
		log_verbose("Using volume group(s) on command line");

		for (; opt < argc; opt++) {
			vg_name = argv[opt];
			if (*vg_name == '@') {
				if (!validate_name(vg_name + 1)) {
					log_error("Skipping invalid tag %s",
						  vg_name);
					continue;
				}
				if (!str_list_add(cmd->mem, &tags,
						  pool_strdup(cmd->mem,
							      vg_name + 1))) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
				continue;
			}

			if (*vg_name == '/') {
				while (*vg_name == '/')
					vg_name++;
				vg_name--;
			}
			if (!strncmp(vg_name, dev_dir, strlen(dev_dir)))
				vg_name += strlen(dev_dir);
			if (strchr(vg_name, '/')) {
				log_error("Invalid volume group name: %s",
					  vg_name);
				continue;
			}
			if (!str_list_add(cmd->mem, &arg_vgnames,
					  pool_strdup(cmd->mem, vg_name))) {
				log_error("strlist allocation failed");
				return ECMD_FAILED;
			}
		}

		vgnames = &arg_vgnames;
	}

	if (!argc || !list_empty(&tags)) {
		log_verbose("Finding all volume groups");
		if (!(vgnames = get_vgs(cmd, 0)) || list_empty(vgnames)) {
			log_error("No volume groups found");
			return ret_max;
		}
	}

	list_iterate_items(sl, vgnames) {
		vg_name = sl->str;
		if (!vg_name || !*vg_name)
			continue;	/* FIXME Unnecessary? */
		ret_max = _process_one_vg(cmd, vg_name, &tags, &arg_vgnames,
					  lock_type, consistent, handle,
					  ret_max, process_single);
	}

	return ret_max;
}

int process_each_pv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  struct list *tags, void *handle,
			  int (*process_single) (struct cmd_context * cmd,
						 struct volume_group * vg,
						 struct physical_volume * pv,
						 void *handle))
{
	int ret_max = 0;
	int ret = 0;
	struct pv_list *pvl;

	list_iterate_items(pvl, &vg->pvs) {
		if (tags && !list_empty(tags) &&
		    !str_list_match_list(tags, &pvl->pv->tags)) {
			continue;
		}
		if ((ret = process_single(cmd, vg, pvl->pv, handle)) > ret_max)
			ret_max = ret;
	}

	return ret_max;
}

static int _process_all_devs(struct cmd_context *cmd, void *handle,
		    int (*process_single) (struct cmd_context * cmd,
					   struct volume_group * vg,
					   struct physical_volume * pv,
					   void *handle))
{
	struct physical_volume *pv;
	struct physical_volume pv_dummy;
	struct dev_iter *iter;
	struct device *dev;

	int ret_max = 0;
	int ret = 0;

	if (!(iter = dev_iter_create(cmd->filter, 1))) {
		log_error("dev_iter creation failed");
		return ECMD_FAILED;
	}

	while ((dev = dev_iter_get(iter))) {
		if (!(pv = pv_read(cmd, dev_name(dev), NULL, NULL, 0))) {
			memset(&pv_dummy, 0, sizeof(pv_dummy));
			list_init(&pv_dummy.tags);
			list_init(&pv_dummy.segments);
			pv_dummy.dev = dev;
			pv_dummy.fmt = NULL;
			pv = &pv_dummy;
		}
		ret = process_single(cmd, NULL, pv, handle);
		if (ret > ret_max)
			ret_max = ret;
	}

	dev_iter_destroy(iter);

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
	struct list *pvslist, *vgnames;
	struct list tags;
	struct str_list *sll;
	char *tagname;
	int consistent = 1;

	list_init(&tags);

	if (argc) {
		log_verbose("Using physical volume(s) on command line");
		for (; opt < argc; opt++) {
			if (*argv[opt] == '@') {
				tagname = argv[opt] + 1;

				if (!validate_name(tagname)) {
					log_error("Skipping invalid tag %s",
						  tagname);
					if (ret_max < EINVALID_CMD_LINE)
						ret_max = EINVALID_CMD_LINE;
					continue;
				}
				if (!str_list_add(cmd->mem, &tags,
						  pool_strdup(cmd->mem,
							      tagname))) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
				continue;
			}
			if (vg) {
				if (!(pvl = find_pv_in_vg(vg, argv[opt]))) {
					log_error("Physical Volume \"%s\" not "
						  "found in Volume Group "
						  "\"%s\"", argv[opt],
						  vg->name);
					ret_max = ECMD_FAILED;
					continue;
				}
				pv = pvl->pv;
			} else {
				if (!(pv = pv_read(cmd, argv[opt], NULL,
						   NULL, 1))) {
					log_error("Failed to read physical "
						  "volume \"%s\"", argv[opt]);
					ret_max = ECMD_FAILED;
					continue;
				}
			}

			ret = process_single(cmd, vg, pv, handle);
			if (ret > ret_max)
				ret_max = ret;
		}
		if (!list_empty(&tags) && (vgnames = get_vgs(cmd, 0)) &&
		    !list_empty(vgnames)) {
			list_iterate_items(sll, vgnames) {
				vg = vg_read(cmd, sll->str, &consistent);
				if (!consistent)
					continue;
				ret = process_each_pv_in_vg(cmd, vg, &tags,
							    handle,
							    process_single);
				if (ret > ret_max)
					ret_max = ret;
			}
		}
	} else {
		if (vg) {
			log_verbose("Using all physical volume(s) in "
				    "volume group");
			ret = process_each_pv_in_vg(cmd, vg, NULL, handle,
						    process_single);
			if (ret > ret_max)
				ret_max = ret;
		} else if (arg_count(cmd, all_ARG)) {
			ret = _process_all_devs(cmd, handle, process_single);
			if (ret > ret_max)
				ret_max = ret;
		} else {
			log_verbose("Scanning for physical volume names");
			if (!(pvslist = get_pvs(cmd)))
				return ECMD_FAILED;

			list_iterate_items(pvl, pvslist) {
				ret = process_single(cmd, NULL, pvl->pv,
						     handle);
				if (ret > ret_max)
					ret_max = ret;
			}
		}
	}

	return ret_max;
}

/*
 * Determine volume group name from a logical volume name
 */
const char *extract_vgname(struct cmd_context *cmd, const char *lv_name)
{
	const char *vg_name = lv_name;
	char *st;
	char *dev_dir = cmd->dev_dir;
	int dev_dir_provided = 0;

	/* Path supplied? */
	if (vg_name && strchr(vg_name, '/')) {
		/* Strip dev_dir (optional) */
		if (*vg_name == '/') {
			while (*vg_name == '/')
				vg_name++;
			vg_name--;
		}
		if (!strncmp(vg_name, dev_dir, strlen(dev_dir))) {
			vg_name += strlen(dev_dir);
			dev_dir_provided = 1;
			while (*vg_name == '/')
				vg_name++;
		}
		if (*vg_name == '/') {
			log_error("\"%s\": Invalid path for Logical "
				  "Volume", lv_name);
			return 0;
		}

		/* Require exactly one set of consecutive slashes */
		if ((st = strchr(vg_name, '/')))
			while (*st == '/')
				st++;

		if (!strchr(vg_name, '/') || strchr(st, '/')) {
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

/*
 * Extract default volume group name from environment
 */
char *default_vgname(struct cmd_context *cmd)
{
	char *vg_path;
	char *dev_dir = cmd->dev_dir;

	/* Take default VG from environment? */
	vg_path = getenv("LVM_VG_NAME");
	if (!vg_path)
		return 0;

	/* Strip dev_dir (optional) */
	if (*vg_path == '/') {
		while (*vg_path == '/')
			vg_path++;
		vg_path--;
	}
	if (!strncmp(vg_path, dev_dir, strlen(dev_dir)))
		vg_path += strlen(dev_dir);

	if (strchr(vg_path, '/')) {
		log_error("Environment Volume Group in LVM_VG_NAME invalid: "
			  "\"%s\"", vg_path);
		return 0;
	}

	return pool_strdup(cmd->mem, vg_path);
}

/*
 * Process physical extent range specifiers
 */
static int _add_pe_range(struct pool *mem, struct list *pe_ranges,
			 uint32_t start, uint32_t count)
{
	struct pe_range *per;

	log_debug("Adding PE range: start PE %" PRIu32 " length %" PRIu32,
		  start, count);

	/* Ensure no overlap with existing areas */
	list_iterate_items(per, pe_ranges) {
		if (((start < per->start) && (start + count - 1 >= per->start))
		    || ((start >= per->start) &&
			(per->start + per->count - 1) >= start)) {
			log_error("Overlapping PE ranges detected (%" PRIu32
				  "-%" PRIu32 ", %" PRIu32 "-%" PRIu32 ")",
				  start, start + count - 1, per->start,
				  per->start + per->count - 1);
			return 0;
		}
	}

	if (!(per = pool_alloc(mem, sizeof(*per)))) {
		log_error("Allocation of list failed");
		return 0;
	}

	per->start = start;
	per->count = count;
	list_add(pe_ranges, &per->list);

	return 1;
}

static int _parse_pes(struct pool *mem, char *c, struct list *pe_ranges,
		      uint32_t size)
{
	char *endptr;
	uint32_t start, end;

	/* Default to whole PV */
	if (!c) {
		if (!_add_pe_range(mem, pe_ranges, UINT32_C(0), size)) {
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

		if (!_add_pe_range(mem, pe_ranges, start, end - start + 1)) {
			stack;
			return 0;
		}

	}

	return 1;

      error:
	log_error("Physical extent parsing error at %s", c);
	return 0;
}

static void _create_pv_entry(struct pool *mem, struct pv_list *pvl,
			     char *colon, int allocatable_only, struct list *r)
{
	const char *pvname;
	struct pv_list *new_pvl;
	struct list *pe_ranges;

	pvname = dev_name(pvl->pv->dev);
	if (allocatable_only && !(pvl->pv->status & ALLOCATABLE_PV)) {
		log_error("Physical volume %s not allocatable", pvname);
		return;
	}

	if (allocatable_only &&
	    (pvl->pv->pe_count == pvl->pv->pe_alloc_count)) {
		log_err("No free extents on physical volume \"%s\"", pvname);
		return;
	}

	if (!(new_pvl = pool_alloc(mem, sizeof(*new_pvl)))) {
		log_err("Unable to allocate physical volume list.");
		return;
	}

	memcpy(new_pvl, pvl, sizeof(*new_pvl));

	if (!(pe_ranges = pool_alloc(mem, sizeof(*pe_ranges)))) {
		log_error("Allocation of pe_ranges list failed");
		return;
	}
	list_init(pe_ranges);

	/* Determine selected physical extents */
	if (!_parse_pes(mem, colon, pe_ranges, pvl->pv->pe_count)) {
		stack;
		return;
	}
	new_pvl->pe_ranges = pe_ranges;

	list_add(r, &new_pvl->list);
}

struct list *create_pv_list(struct pool *mem, struct volume_group *vg, int argc,
			    char **argv, int allocatable_only)
{
	struct list *r;
	struct pv_list *pvl;
	struct list tags, arg_pvnames;
	const char *pvname = NULL;
	char *colon, *tagname;
	int i;

	/* Build up list of PVs */
	if (!(r = pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed");
		return NULL;
	}
	list_init(r);

	list_init(&tags);
	list_init(&arg_pvnames);

	for (i = 0; i < argc; i++) {
		if (*argv[i] == '@') {
			tagname = argv[i] + 1;
			if (!validate_name(tagname)) {
				log_error("Skipping invalid tag %s", tagname);
				continue;
			}
			list_iterate_items(pvl, &vg->pvs) {
				if (str_list_match_item(&pvl->pv->tags,
							tagname)) {
					_create_pv_entry(mem, pvl, NULL,
							 allocatable_only, r);
				}
			}
			continue;
		}

		pvname = argv[i];

		if ((colon = strchr(pvname, ':'))) {
			if (!(pvname = pool_strndup(mem, pvname,
						    (unsigned) (colon -
								pvname)))) {
				log_error("Failed to clone PV name");
				return NULL;
			}
		}

		if (!(pvl = find_pv_in_vg(vg, pvname))) {
			log_err("Physical Volume \"%s\" not found in "
				"Volume Group \"%s\"", pvname, vg->name);
			return NULL;
		}
		_create_pv_entry(mem, pvl, colon, allocatable_only, r);
	}

	if (list_empty(r))
		log_error("No specified PVs have space available");

	return list_empty(r) ? NULL : r;
}

struct list *clone_pv_list(struct pool *mem, struct list *pvsl)
{
	struct list *r;
	struct pv_list *pvl, *new_pvl;

	/* Build up list of PVs */
	if (!(r = pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed");
		return NULL;
	}
	list_init(r);

	list_iterate_items(pvl, pvsl) {
		if (!(new_pvl = pool_zalloc(mem, sizeof(*new_pvl)))) {
			log_error("Unable to allocate physical volume list.");
			return NULL;
		}

		memcpy(new_pvl, pvl, sizeof(*new_pvl));
		list_add(r, &new_pvl->list);
	}

	return r;
}

/*
 * Attempt metadata recovery
 */
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

/*
 * Execute and wait for external command
 */
int exec_cmd(const char *command, const char *fscmd, const char *lv_path,
	     const char *size)
{
	pid_t pid;
	int status;

	log_verbose("Executing: %s %s %s %s", command, fscmd, lv_path, size);

	if ((pid = fork()) == -1) {
		log_error("fork failed: %s", strerror(errno));
		return 0;
	}

	if (!pid) {
		/* Child */
		/* FIXME Use execve directly */
		execlp(command, command, fscmd, lv_path, size, NULL);
		log_sys_error("execlp", command);
		exit(errno);
	}

	/* Parent */
	if (wait4(pid, &status, 0, NULL) != pid) {
		log_error("wait4 child process %u failed: %s", pid,
			  strerror(errno));
		return 0;
	}

	if (!WIFEXITED(status)) {
		log_error("Child %u exited abnormally", pid);
		return 0;
	}

	if (WEXITSTATUS(status)) {
		log_error("%s failed: %u", command, WEXITSTATUS(status));
		return 0;
	}

	return 1;
}
