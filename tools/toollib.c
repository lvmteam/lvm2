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

#include "tools.h"
#include "lv_alloc.h"
#include "xlate.h"

#include <sys/stat.h>
#include <sys/wait.h>

const char *command_name(struct cmd_context *cmd)
{
	return cmd->command->name;
}

/*
 * Strip dev_dir if present
 */
const char *skip_dev_dir(struct cmd_context *cmd, const char *vg_name,
		   unsigned *dev_dir_found)
{
	const char *dmdir = dm_dir();
	size_t dmdir_len = strlen(dmdir), vglv_sz;
	char *vgname, *lvname, *layer, *vglv;

	/* FIXME Do this properly */
	if (*vg_name == '/') {
		while (*vg_name == '/')
			vg_name++;
		vg_name--;
	}

	/* Reformat string if /dev/mapper found */
	if (!strncmp(vg_name, dmdir, dmdir_len) && vg_name[dmdir_len] == '/') {
		if (dev_dir_found)
			*dev_dir_found = 1;
		vg_name += dmdir_len;
		while (*vg_name == '/')
			vg_name++;

		if (!dm_split_lvm_name(cmd->mem, vg_name, &vgname, &lvname, &layer) ||
		    *layer) {
			log_error("skip_dev_dir: Couldn't split up device name %s",
				  vg_name);
			return vg_name;
		}
		vglv_sz = strlen(vgname) + strlen(lvname) + 2;
		if (!(vglv = dm_pool_alloc(cmd->mem, vglv_sz)) ||
		    dm_snprintf(vglv, vglv_sz, "%s%s%s", vgname,
				 *lvname ? "/" : "",
				 lvname) < 0) {
			log_error("vg/lv string alloc failed");
			return vg_name;
		}
		return vglv;
	}

	if (!strncmp(vg_name, cmd->dev_dir, strlen(cmd->dev_dir))) {
		if (dev_dir_found)
			*dev_dir_found = 1;
		vg_name += strlen(cmd->dev_dir);
		while (*vg_name == '/')
			vg_name++;
	} else if (dev_dir_found)
		*dev_dir_found = 0;

	return vg_name;
}

/*
 * Metadata iteration functions
 */
int process_each_lv_in_vg(struct cmd_context *cmd,
			  struct volume_group *vg,
			  const struct dm_list *arg_lvnames,
			  const struct dm_list *tags,
			  struct dm_list *failed_lvnames,
			  void *handle,
			  process_single_lv_fn_t process_single_lv)
{
	int ret_max = ECMD_PROCESSED;
	int ret = 0;
	unsigned process_all = 0;
	unsigned process_lv = 0;
	unsigned tags_supplied = 0;
	unsigned lvargs_supplied = 0;
	unsigned lvargs_matched = 0;
	char *lv_name;
	struct lv_list *lvl;

	if (!vg_check_status(vg, EXPORTED_VG))
		return ECMD_FAILED;

	if (tags && !dm_list_empty(tags))
		tags_supplied = 1;

	if (arg_lvnames && !dm_list_empty(arg_lvnames))
		lvargs_supplied = 1;

	/* Process all LVs in this VG if no restrictions given */
	if (!tags_supplied && !lvargs_supplied)
		process_all = 1;

	/* Or if VG tags match */
	if (!process_lv && tags_supplied &&
	    str_list_match_list(tags, &vg->tags, NULL)) {
		process_all = 1;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (lvl->lv->status & SNAPSHOT)
			continue;

		if (lv_is_virtual_origin(lvl->lv) && !arg_count(cmd, all_ARG))
			continue;

		/*
		 * Only let hidden LVs through it --all was used or the LVs 
		 * were specifically named on the command line.
		 */
		if (!lvargs_supplied && !lv_is_visible(lvl->lv) && !arg_count(cmd, all_ARG))
			continue;

		/* Should we process this LV? */
		if (process_all)
			process_lv = 1;
		else
			process_lv = 0;

		/* LV tag match? */
		if (!process_lv && tags_supplied &&
		    str_list_match_list(tags, &lvl->lv->tags, NULL)) {
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

		lvl->lv->vg->cmd_missing_vgs = 0;
		ret = process_single_lv(cmd, lvl->lv, handle);
		if (ret != ECMD_PROCESSED && failed_lvnames) {
			lv_name = dm_pool_strdup(cmd->mem, lvl->lv->name);
			if (!lv_name ||
			    !str_list_add(cmd->mem, failed_lvnames, lv_name)) {
				log_error("Allocation failed for str_list.");
				return ECMD_FAILED;
			}
			if (lvl->lv->vg->cmd_missing_vgs)
				ret = ECMD_PROCESSED;
		}
		if (ret > ret_max)
			ret_max = ret;
		if (sigint_caught()) {
			stack;
			return ret_max;
		}
	}

	if (lvargs_supplied && lvargs_matched != dm_list_size(arg_lvnames)) {
		log_error("One or more specified logical volume(s) not found.");
		if (ret_max < ECMD_FAILED)
			ret_max = ECMD_FAILED;
	}

	return ret_max;
}

int process_each_lv(struct cmd_context *cmd, int argc, char **argv,
		    uint32_t flags, void *handle,
		    process_single_lv_fn_t process_single_lv)
{
	int opt = 0;
	int ret_max = ECMD_PROCESSED;
	int ret = 0;

	struct dm_list *tags_arg;
	struct dm_list *vgnames;	/* VGs to process */
	struct str_list *sll, *strl;
	struct cmd_vg *cvl_vg;
	struct dm_list cmd_vgs;
	struct dm_list failed_lvnames;
	struct dm_list tags, lvnames;
	struct dm_list arg_lvnames;	/* Cmdline vgname or vgname/lvname */
	struct dm_list arg_vgnames;
	char *vglv;
	size_t vglv_sz;

	const char *vgname;

	dm_list_init(&tags);
	dm_list_init(&arg_lvnames);
	dm_list_init(&failed_lvnames);

	if (argc) {
		log_verbose("Using logical volume(s) on command line");
		dm_list_init(&arg_vgnames);

		for (; opt < argc; opt++) {
			const char *lv_name = argv[opt];
			const char *tmp_lv_name;
			char *vgname_def;
			unsigned dev_dir_found = 0;

			/* Do we have a tag or vgname or lvname? */
			vgname = lv_name;

			if (*vgname == '@') {
				if (!validate_tag(vgname + 1)) {
					log_error("Skipping invalid tag %s",
						  vgname);
					continue;
				}
				if (!str_list_add(cmd->mem, &tags,
						  dm_pool_strdup(cmd->mem,
							      vgname + 1))) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
				continue;
			}

			/* FIXME Jumbled parsing */
			vgname = skip_dev_dir(cmd, vgname, &dev_dir_found);

			if (*vgname == '/') {
				log_error("\"%s\": Invalid path for Logical "
					  "Volume", argv[opt]);
				if (ret_max < ECMD_FAILED)
					ret_max = ECMD_FAILED;
				continue;
			}
			lv_name = vgname;
			if ((tmp_lv_name = strchr(vgname, '/'))) {
				/* Must be an LV */
				lv_name = tmp_lv_name;
				while (*lv_name == '/')
					lv_name++;
				if (!(vgname = extract_vgname(cmd, vgname))) {
					if (ret_max < ECMD_FAILED) {
						stack;
						ret_max = ECMD_FAILED;
					}
					continue;
				}
			} else if (!dev_dir_found &&
				   (vgname_def = default_vgname(cmd))) {
				vgname = vgname_def;
			} else
				lv_name = NULL;

			if (!str_list_add(cmd->mem, &arg_vgnames,
					  dm_pool_strdup(cmd->mem, vgname))) {
				log_error("strlist allocation failed");
				return ECMD_FAILED;
			}

			if (!lv_name) {
				if (!str_list_add(cmd->mem, &arg_lvnames,
						  dm_pool_strdup(cmd->mem,
							      vgname))) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
			} else {
				vglv_sz = strlen(vgname) + strlen(lv_name) + 2;
				if (!(vglv = dm_pool_alloc(cmd->mem, vglv_sz)) ||
				    dm_snprintf(vglv, vglv_sz, "%s/%s", vgname,
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

	if (!argc || !dm_list_empty(&tags)) {
		log_verbose("Finding all logical volumes");
		if (!(vgnames = get_vgnames(cmd, 0)) || dm_list_empty(vgnames)) {
			log_error("No volume groups found");
			return ret_max;
		}
	}

	dm_list_iterate_items(strl, vgnames) {
		vgname = strl->str;
		dm_list_init(&cmd_vgs);
		if (!(cvl_vg = cmd_vg_add(cmd->mem, &cmd_vgs,
					  vgname, NULL, flags))) {
			stack;
			return ECMD_FAILED;
		}

		if (!cmd_vg_read(cmd, &cmd_vgs)) {
			free_cmd_vgs(&cmd_vgs);
			if (ret_max < ECMD_FAILED) {
				log_error("Skipping volume group %s", vgname);
				ret_max = ECMD_FAILED;
			} else
				stack;
			continue;
		}

		tags_arg = &tags;
		dm_list_init(&lvnames);	/* LVs to be processed in this VG */
		dm_list_iterate_items(sll, &arg_lvnames) {
			const char *vg_name = sll->str;
			const char *lv_name = strchr(vg_name, '/');

			if ((!lv_name && !strcmp(vg_name, vgname))) {
				/* Process all LVs in this VG */
				tags_arg = NULL;
				dm_list_init(&lvnames);
				break;
			} else if (!strncmp(vg_name, vgname, strlen(vgname)) && lv_name &&
				   strlen(vgname) == (size_t) (lv_name - vg_name)) {
				if (!str_list_add(cmd->mem, &lvnames,
						  dm_pool_strdup(cmd->mem,
								 lv_name + 1))) {
					log_error("strlist allocation failed");
					free_cmd_vgs(&cmd_vgs);
					return ECMD_FAILED;
				}
			}
		}

		while (!sigint_caught()) {
			ret = process_each_lv_in_vg(cmd, cvl_vg->vg, &lvnames,
						    tags_arg, &failed_lvnames,
						    handle, process_single_lv);
			if (ret != ECMD_PROCESSED) {
				stack;
				break;
			}

			if (dm_list_empty(&failed_lvnames))
				break;

			/* Try again with failed LVs in this VG */
			dm_list_init(&lvnames);
			dm_list_splice(&lvnames, &failed_lvnames);

			free_cmd_vgs(&cmd_vgs);
			if (!cmd_vg_read(cmd, &cmd_vgs)) {
				stack;
				ret = ECMD_FAILED; /* break */
				break;
			}
		}
		if (ret > ret_max)
			ret_max = ret;

		free_cmd_vgs(&cmd_vgs);
		/* FIXME: logic for breaking command is not consistent */
		if (sigint_caught()) {
			stack;
			return ECMD_FAILED;
		}
	}

	return ret_max;
}

int process_each_segment_in_pv(struct cmd_context *cmd,
			       struct volume_group *vg,
			       struct physical_volume *pv,
			       void *handle,
			       process_single_pvseg_fn_t process_single_pvseg)
{
	struct pv_segment *pvseg;
	struct pv_list *pvl;
	const char *vg_name = NULL;
	int ret_max = ECMD_PROCESSED;
	int ret;
	struct volume_group *old_vg = vg;
	struct pv_segment _free_pv_segment = { .pv = pv };

	if (is_pv(pv) && !vg && !is_orphan(pv)) {
		vg_name = pv_vg_name(pv);

		vg = vg_read(cmd, vg_name, NULL, 0);
		if (vg_read_error(vg)) {
			release_vg(vg);
			log_error("Skipping volume group %s", vg_name);
			return ECMD_FAILED;
		}

		/*
		 * Replace possibly incomplete PV structure with new one
		 * allocated in vg_read_internal() path.
		 */
		if (!(pvl = find_pv_in_vg(vg, pv_dev_name(pv)))) {
			 log_error("Unable to find %s in volume group %s",
				   pv_dev_name(pv), vg_name);
			 unlock_and_release_vg(cmd, vg, vg_name);
			 return ECMD_FAILED;
		}

		pv = pvl->pv;
	}

	if (dm_list_empty(&pv->segments)) {
		ret = process_single_pvseg(cmd, NULL, &_free_pv_segment, handle);
		if (ret > ret_max)
			ret_max = ret;
	} else
		dm_list_iterate_items(pvseg, &pv->segments) {
			ret = process_single_pvseg(cmd, vg, pvseg, handle);
			if (ret > ret_max)
				ret_max = ret;
			if (sigint_caught())
				break;
		}

	if (vg_name)
		unlock_vg(cmd, vg_name);
	if (!old_vg)
		release_vg(vg);

	return ret_max;
}

int process_each_segment_in_lv(struct cmd_context *cmd,
			       struct logical_volume *lv,
			       void *handle,
			       process_single_seg_fn_t process_single_seg)
{
	struct lv_segment *seg;
	int ret_max = ECMD_PROCESSED;
	int ret;

	dm_list_iterate_items(seg, &lv->segments) {
		ret = process_single_seg(cmd, seg, handle);
		if (ret > ret_max)
			ret_max = ret;
		/* FIXME: logic for breaking command is not consistent */
		if (sigint_caught())
			return ECMD_FAILED;
	}

	return ret_max;
}

static int _process_one_vg(struct cmd_context *cmd, const char *vg_name,
			   const char *vgid,
			   struct dm_list *tags, struct dm_list *arg_vgnames,
			   uint32_t flags, void *handle, int ret_max,
			   process_single_vg_fn_t process_single_vg)
{
	struct dm_list cmd_vgs;
	struct cmd_vg *cvl_vg;
	int ret = 0;

	log_verbose("Finding volume group \"%s\"", vg_name);

	dm_list_init(&cmd_vgs);
	if (!(cvl_vg = cmd_vg_add(cmd->mem, &cmd_vgs, vg_name, vgid, flags)))
		return_0;

	for (;;) {
		/* FIXME: consistent handling of command break */
		if (sigint_caught()) {
			ret = ECMD_FAILED;
			break;
		}
		if (!cmd_vg_read(cmd, &cmd_vgs))
			/* Allow FAILED_INCONSISTENT through only for vgcfgrestore */
			if (vg_read_error(cvl_vg->vg) &&
			    (!((flags & READ_ALLOW_INCONSISTENT) &&
			       (vg_read_error(cvl_vg->vg) == FAILED_INCONSISTENT)))) {
				ret = ECMD_FAILED;
				break;
			}

		if (!dm_list_empty(tags) &&
		    /* Only process if a tag matches or it's on arg_vgnames */
		    !str_list_match_item(arg_vgnames, vg_name) &&
		    !str_list_match_list(tags, &cvl_vg->vg->tags, NULL))
			break;

		ret = process_single_vg(cmd, vg_name, cvl_vg->vg, handle);

		if (vg_read_error(cvl_vg->vg)) /* FAILED_INCONSISTENT */
			break;

		if (!cvl_vg->vg->cmd_missing_vgs)
			break;

		free_cmd_vgs(&cmd_vgs);
	}

	free_cmd_vgs(&cmd_vgs);

	return (ret > ret_max) ? ret : ret_max;
}

int process_each_vg(struct cmd_context *cmd, int argc, char **argv,
		    uint32_t flags, void *handle,
		    process_single_vg_fn_t process_single_vg)
{
	int opt = 0;
	int ret_max = ECMD_PROCESSED;

	struct str_list *sl;
	struct dm_list *vgnames, *vgids;
	struct dm_list arg_vgnames, tags;

	const char *vg_name, *vgid;

	dm_list_init(&tags);
	dm_list_init(&arg_vgnames);

	if (argc) {
		log_verbose("Using volume group(s) on command line");

		for (; opt < argc; opt++) {
			vg_name = argv[opt];
			if (*vg_name == '@') {
				if (!validate_tag(vg_name + 1)) {
					log_error("Skipping invalid tag %s",
						  vg_name);
					if (ret_max < EINVALID_CMD_LINE)
						ret_max = EINVALID_CMD_LINE;
					continue;
				}
				if (!str_list_add(cmd->mem, &tags,
						  dm_pool_strdup(cmd->mem,
							      vg_name + 1))) {
					log_error("strlist allocation failed");
					return ECMD_FAILED;
				}
				continue;
			}

			vg_name = skip_dev_dir(cmd, vg_name, NULL);
			if (strchr(vg_name, '/')) {
				log_error("Invalid volume group name: %s",
					  vg_name);
				if (ret_max < EINVALID_CMD_LINE)
					ret_max = EINVALID_CMD_LINE;
				continue;
			}
			if (!str_list_add(cmd->mem, &arg_vgnames,
					  dm_pool_strdup(cmd->mem, vg_name))) {
				log_error("strlist allocation failed");
				return ECMD_FAILED;
			}
		}

		vgnames = &arg_vgnames;
	}

	if (!argc || !dm_list_empty(&tags)) {
		log_verbose("Finding all volume groups");
		if (!(vgids = get_vgids(cmd, 0)) || dm_list_empty(vgids)) {
			log_error("No volume groups found");
			return ret_max;
		}
		dm_list_iterate_items(sl, vgids) {
			vgid = sl->str;
			if (!(vgid) || !(vg_name = vgname_from_vgid(cmd->mem, vgid)))
				continue;
			ret_max = _process_one_vg(cmd, vg_name, vgid, &tags,
						  &arg_vgnames,
						  flags, handle,
					  	  ret_max, process_single_vg);
			if (sigint_caught())
				return ret_max;
		}
	} else {
		dm_list_iterate_items(sl, vgnames) {
			vg_name = sl->str;
			if (is_orphan_vg(vg_name))
				continue;	/* FIXME Unnecessary? */
			ret_max = _process_one_vg(cmd, vg_name, NULL, &tags,
						  &arg_vgnames,
						  flags, handle,
					  	  ret_max, process_single_vg);
			if (sigint_caught())
				return ret_max;
		}
	}

	return ret_max;
}

int process_each_pv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  const struct dm_list *tags, void *handle,
			  process_single_pv_fn_t process_single_pv)
{
	int ret_max = ECMD_PROCESSED;
	int ret = 0;
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (tags && !dm_list_empty(tags) &&
		    !str_list_match_list(tags, &pvl->pv->tags, NULL)) {
			continue;
		}
		if ((ret = process_single_pv(cmd, vg, pvl->pv, handle)) > ret_max)
			ret_max = ret;
		if (sigint_caught())
			return ret_max;
	}

	return ret_max;
}

static int _process_all_devs(struct cmd_context *cmd, void *handle,
			     process_single_pv_fn_t process_single_pv)
{
	struct physical_volume *pv;
	struct physical_volume pv_dummy;
	struct dev_iter *iter;
	struct device *dev;

	int ret_max = ECMD_PROCESSED;
	int ret = 0;

	if (!scan_vgs_for_pvs(cmd, 1)) {
		stack;
		return ECMD_FAILED;
	}

	if (!(iter = dev_iter_create(cmd->filter, 1))) {
		log_error("dev_iter creation failed");
		return ECMD_FAILED;
	}

	while ((dev = dev_iter_get(iter))) {
		if (!(pv = pv_read(cmd, dev_name(dev), 0, 0))) {
			memset(&pv_dummy, 0, sizeof(pv_dummy));
			dm_list_init(&pv_dummy.tags);
			dm_list_init(&pv_dummy.segments);
			pv_dummy.dev = dev;
			pv_dummy.fmt = NULL;
			pv = &pv_dummy;
		}
		ret = process_single_pv(cmd, NULL, pv, handle);

		free_pv_fid(pv);

		if (ret > ret_max)
			ret_max = ret;
		if (sigint_caught())
			break;
	}

	dev_iter_destroy(iter);

	return ret_max;
}

/*
 * If the lock_type is LCK_VG_READ (used only in reporting commands),
 * we lock VG_GLOBAL to enable use of metadata cache.
 * This can pause alongide pvscan or vgscan process for a while.
 */
int process_each_pv(struct cmd_context *cmd, int argc, char **argv,
		    struct volume_group *vg, uint32_t flags,
		    int scan_label_only, void *handle,
		    process_single_pv_fn_t process_single_pv)
{
	int opt = 0;
	int ret_max = ECMD_PROCESSED;
	int ret = 0;
	int lock_global = !(flags & READ_WITHOUT_LOCK) && !(flags & READ_FOR_UPDATE);

	struct pv_list *pvl;
	struct physical_volume *pv;
	struct dm_list *pvslist, *vgnames;
	struct dm_list tags;
	struct str_list *sll;
	char *at_sign, *tagname;
	int scanned = 0;

	dm_list_init(&tags);

	if (lock_global && !lock_vol(cmd, VG_GLOBAL, LCK_VG_READ)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	if (argc) {
		log_verbose("Using physical volume(s) on command line");
		for (; opt < argc; opt++) {
			unescape_colons_and_at_signs(argv[opt], NULL, &at_sign);
			if (at_sign && (at_sign == argv[opt])) {
				tagname = at_sign + 1;

				if (!validate_tag(tagname)) {
					log_error("Skipping invalid tag %s",
						  tagname);
					if (ret_max < EINVALID_CMD_LINE)
						ret_max = EINVALID_CMD_LINE;
					continue;
				}
				if (!str_list_add(cmd->mem, &tags,
						  dm_pool_strdup(cmd->mem,
							      tagname))) {
					log_error("strlist allocation failed");
					goto bad;
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
				if (!(pv = pv_read(cmd, argv[opt],
						   1, scan_label_only))) {
					log_error("Failed to read physical "
						  "volume \"%s\"", argv[opt]);
					ret_max = ECMD_FAILED;
					continue;
				}

				/*
				 * If a PV has no MDAs it may appear to be an
				 * orphan until the metadata is read off
				 * another PV in the same VG.  Detecting this
				 * means checking every VG by scanning every
				 * PV on the system.
				 */
				if (!scanned && is_orphan(pv) &&
				    !dm_list_size(&pv->fid->metadata_areas_in_use) &&
				    !dm_list_size(&pv->fid->metadata_areas_ignored)) {
					if (!scan_label_only &&
					    !scan_vgs_for_pvs(cmd, 1)) {
						stack;
						ret_max = ECMD_FAILED;
						continue;
					}
					scanned = 1;
					free_pv_fid(pv);
					if (!(pv = pv_read(cmd, argv[opt],
							   1,
							   scan_label_only))) {
						log_error("Failed to read "
							  "physical volume "
							  "\"%s\"", argv[opt]);
						ret_max = ECMD_FAILED;
						continue;
					}
				}
			}

			ret = process_single_pv(cmd, vg, pv, handle);

			/*
			 * Free PV only if we called pv_read before,
			 * otherwise the PV structure is part of the VG.
			 */
			if (!vg)
				free_pv_fid(pv);

			if (ret > ret_max)
				ret_max = ret;
			if (sigint_caught())
				goto out;
		}
		if (!dm_list_empty(&tags) && (vgnames = get_vgnames(cmd, 1)) &&
			   !dm_list_empty(vgnames)) {
			dm_list_iterate_items(sll, vgnames) {
				vg = vg_read(cmd, sll->str, NULL, flags);
				if (vg_read_error(vg)) {
					ret_max = ECMD_FAILED;
					release_vg(vg);
					stack;
					continue;
				}

				ret = process_each_pv_in_vg(cmd, vg, &tags,
							    handle,
							    process_single_pv);

				unlock_and_release_vg(cmd, vg, sll->str);

				if (ret > ret_max)
					ret_max = ret;
				if (sigint_caught())
					goto out;
			}
		}
	} else {
		if (vg) {
			log_verbose("Using all physical volume(s) in "
				    "volume group");
			ret = process_each_pv_in_vg(cmd, vg, NULL, handle,
						    process_single_pv);
			if (ret > ret_max)
				ret_max = ret;
			if (sigint_caught())
				goto out;
		} else if (arg_count(cmd, all_ARG)) {
			ret = _process_all_devs(cmd, handle, process_single_pv);
			if (ret > ret_max)
				ret_max = ret;
			if (sigint_caught())
				goto out;
		} else {
			log_verbose("Scanning for physical volume names");

			if (!(pvslist = get_pvs(cmd)))
				goto bad;

			dm_list_iterate_items(pvl, pvslist) {
				ret = process_single_pv(cmd, NULL, pvl->pv,
						     handle);
				free_pv_fid(pvl->pv);
				if (ret > ret_max)
					ret_max = ret;
				if (sigint_caught())
					goto out;
			}
		}
	}
out:
	if (lock_global)
		unlock_vg(cmd, VG_GLOBAL);
	return ret_max;
bad:
	if (lock_global)
		unlock_vg(cmd, VG_GLOBAL);

	return ECMD_FAILED;
}

/*
 * Determine volume group name from a logical volume name
 */
const char *extract_vgname(struct cmd_context *cmd, const char *lv_name)
{
	const char *vg_name = lv_name;
	char *st;
	char *dev_dir = cmd->dev_dir;

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

		if (!st || strchr(st, '/')) {
			log_error("\"%s\": Invalid path for Logical Volume",
				  lv_name);
			return 0;
		}

		vg_name = dm_pool_strdup(cmd->mem, vg_name);
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
	const char *vg_path;

	/* Take default VG from environment? */
	vg_path = getenv("LVM_VG_NAME");
	if (!vg_path)
		return 0;

	vg_path = skip_dev_dir(cmd, vg_path, NULL);

	if (strchr(vg_path, '/')) {
		log_error("Environment Volume Group in LVM_VG_NAME invalid: "
			  "\"%s\"", vg_path);
		return 0;
	}

	return dm_pool_strdup(cmd->mem, vg_path);
}

/*
 * Process physical extent range specifiers
 */
static int _add_pe_range(struct dm_pool *mem, const char *pvname,
			 struct dm_list *pe_ranges, uint32_t start, uint32_t count)
{
	struct pe_range *per;

	log_debug("Adding PE range: start PE %" PRIu32 " length %" PRIu32
		  " on %s", start, count, pvname);

	/* Ensure no overlap with existing areas */
	dm_list_iterate_items(per, pe_ranges) {
		if (((start < per->start) && (start + count - 1 >= per->start))
		    || ((start >= per->start) &&
			(per->start + per->count - 1) >= start)) {
			log_error("Overlapping PE ranges specified (%" PRIu32
				  "-%" PRIu32 ", %" PRIu32 "-%" PRIu32 ")"
				  " on %s",
				  start, start + count - 1, per->start,
				  per->start + per->count - 1, pvname);
			return 0;
		}
	}

	if (!(per = dm_pool_alloc(mem, sizeof(*per)))) {
		log_error("Allocation of list failed");
		return 0;
	}

	per->start = start;
	per->count = count;
	dm_list_add(pe_ranges, &per->list);

	return 1;
}

static int xstrtouint32(const char *s, char **p, int base, uint32_t *result)
{
	unsigned long ul;

	errno = 0;
	ul = strtoul(s, p, base);
	if (errno || *p == s || (uint32_t) ul != ul)
		return -1;
	*result = ul;
	return 0;
}

static int _parse_pes(struct dm_pool *mem, char *c, struct dm_list *pe_ranges,
		      const char *pvname, uint32_t size)
{
	char *endptr;
	uint32_t start, end;

	/* Default to whole PV */
	if (!c) {
		if (!_add_pe_range(mem, pvname, pe_ranges, UINT32_C(0), size))
			return_0;
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
			if (xstrtouint32(c, &endptr, 10, &start))
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
				if (xstrtouint32(c, &endptr, 10, &end))
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

		if (!_add_pe_range(mem, pvname, pe_ranges, start, end - start + 1))
			return_0;

	}

	return 1;

      error:
	log_error("Physical extent parsing error at %s", c);
	return 0;
}

static int _create_pv_entry(struct dm_pool *mem, struct pv_list *pvl,
			     char *colon, int allocatable_only, struct dm_list *r)
{
	const char *pvname;
	struct pv_list *new_pvl = NULL, *pvl2;
	struct dm_list *pe_ranges;

	pvname = pv_dev_name(pvl->pv);
	if (allocatable_only && !(pvl->pv->status & ALLOCATABLE_PV)) {
		log_error("Physical volume %s not allocatable", pvname);
		return 1;
	}

	if (allocatable_only && is_missing_pv(pvl->pv)) {
		log_error("Physical volume %s is missing", pvname);
		return 1;
	}

	if (allocatable_only &&
	    (pvl->pv->pe_count == pvl->pv->pe_alloc_count)) {
		log_error("No free extents on physical volume \"%s\"", pvname);
		return 1;
	}

	dm_list_iterate_items(pvl2, r)
		if (pvl->pv->dev == pvl2->pv->dev) {
			new_pvl = pvl2;
			break;
		}

	if (!new_pvl) {
		if (!(new_pvl = dm_pool_alloc(mem, sizeof(*new_pvl)))) {
			log_error("Unable to allocate physical volume list.");
			return 0;
		}

		memcpy(new_pvl, pvl, sizeof(*new_pvl));

		if (!(pe_ranges = dm_pool_alloc(mem, sizeof(*pe_ranges)))) {
			log_error("Allocation of pe_ranges list failed");
			return 0;
		}
		dm_list_init(pe_ranges);
		new_pvl->pe_ranges = pe_ranges;
		dm_list_add(r, &new_pvl->list);
	}

	/* Determine selected physical extents */
	if (!_parse_pes(mem, colon, new_pvl->pe_ranges, pv_dev_name(pvl->pv),
			pvl->pv->pe_count))
		return_0;

	return 1;
}

struct dm_list *create_pv_list(struct dm_pool *mem, struct volume_group *vg, int argc,
			    char **argv, int allocatable_only)
{
	struct dm_list *r;
	struct pv_list *pvl;
	struct dm_list tags, arg_pvnames;
	char *pvname = NULL;
	char *colon, *at_sign, *tagname;
	int i;

	/* Build up list of PVs */
	if (!(r = dm_pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed");
		return NULL;
	}
	dm_list_init(r);

	dm_list_init(&tags);
	dm_list_init(&arg_pvnames);

	for (i = 0; i < argc; i++) {
		unescape_colons_and_at_signs(argv[i], &colon, &at_sign);

		if (at_sign && (at_sign == argv[i])) {
			tagname = at_sign + 1;
			if (!validate_tag(tagname)) {
				log_error("Skipping invalid tag %s", tagname);
				continue;
			}
			dm_list_iterate_items(pvl, &vg->pvs) {
				if (str_list_match_item(&pvl->pv->tags,
							tagname)) {
					if (!_create_pv_entry(mem, pvl, NULL,
							      allocatable_only,
							      r))
						return_NULL;
				}
			}
			continue;
		}

		pvname = argv[i];

		if (colon && !(pvname = dm_pool_strndup(mem, pvname,
					(unsigned) (colon - pvname)))) {
			log_error("Failed to clone PV name");
			return NULL;
		}

		if (!(pvl = find_pv_in_vg(vg, pvname))) {
			log_error("Physical Volume \"%s\" not found in "
				  "Volume Group \"%s\"", pvname, vg->name);
			return NULL;
		}
		if (!_create_pv_entry(mem, pvl, colon, allocatable_only, r))
			return_NULL;
	}

	if (dm_list_empty(r))
		log_error("No specified PVs have space available");

	return dm_list_empty(r) ? NULL : r;
}

struct dm_list *clone_pv_list(struct dm_pool *mem, struct dm_list *pvsl)
{
	struct dm_list *r;
	struct pv_list *pvl, *new_pvl;

	/* Build up list of PVs */
	if (!(r = dm_pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed");
		return NULL;
	}
	dm_list_init(r);

	dm_list_iterate_items(pvl, pvsl) {
		if (!(new_pvl = dm_pool_zalloc(mem, sizeof(*new_pvl)))) {
			log_error("Unable to allocate physical volume list.");
			return NULL;
		}

		memcpy(new_pvl, pvl, sizeof(*new_pvl));
		dm_list_add(r, &new_pvl->list);
	}

	return r;
}

void vgcreate_params_set_defaults(struct vgcreate_params *vp_def,
				  struct volume_group *vg)
{
	if (vg) {
		vp_def->vg_name = NULL;
		vp_def->extent_size = vg->extent_size;
		vp_def->max_pv = vg->max_pv;
		vp_def->max_lv = vg->max_lv;
		vp_def->alloc = vg->alloc;
		vp_def->clustered = vg_is_clustered(vg);
		vp_def->vgmetadatacopies = vg->mda_copies;
	} else {
		vp_def->vg_name = NULL;
		vp_def->extent_size = DEFAULT_EXTENT_SIZE * 2;
		vp_def->max_pv = DEFAULT_MAX_PV;
		vp_def->max_lv = DEFAULT_MAX_LV;
		vp_def->alloc = DEFAULT_ALLOC_POLICY;
		vp_def->clustered = DEFAULT_CLUSTERED;
		vp_def->vgmetadatacopies = DEFAULT_VGMETADATACOPIES;
	}
}

/*
 * Set members of struct vgcreate_params from cmdline arguments.
 * Do preliminary validation with arg_*() interface.
 * Further, more generic validation is done in validate_vgcreate_params().
 * This function is to remain in tools directory.
 */
int vgcreate_params_set_from_args(struct cmd_context *cmd,
				  struct vgcreate_params *vp_new,
				  struct vgcreate_params *vp_def)
{
	vp_new->vg_name = skip_dev_dir(cmd, vp_def->vg_name, NULL);
	vp_new->max_lv = arg_uint_value(cmd, maxlogicalvolumes_ARG,
					vp_def->max_lv);
	vp_new->max_pv = arg_uint_value(cmd, maxphysicalvolumes_ARG,
					vp_def->max_pv);
	vp_new->alloc = arg_uint_value(cmd, alloc_ARG, vp_def->alloc);

	/* Units of 512-byte sectors */
	vp_new->extent_size =
	    arg_uint_value(cmd, physicalextentsize_ARG, vp_def->extent_size);

	if (arg_count(cmd, clustered_ARG))
		vp_new->clustered =
			!strcmp(arg_str_value(cmd, clustered_ARG,
					      vp_def->clustered ? "y":"n"), "y");
	else
		/* Default depends on current locking type */
		vp_new->clustered = locking_is_clustered();

	if (arg_sign_value(cmd, physicalextentsize_ARG, 0) == SIGN_MINUS) {
		log_error("Physical extent size may not be negative");
		return 1;
	}

	if (arg_uint64_value(cmd, physicalextentsize_ARG, 0) > MAX_EXTENT_SIZE) {
		log_error("Physical extent size cannot be larger than %s",
				  display_size(cmd, (uint64_t) MAX_EXTENT_SIZE));
		return 1;
	}

	if (arg_sign_value(cmd, maxlogicalvolumes_ARG, 0) == SIGN_MINUS) {
		log_error("Max Logical Volumes may not be negative");
		return 1;
	}

	if (arg_sign_value(cmd, maxphysicalvolumes_ARG, 0) == SIGN_MINUS) {
		log_error("Max Physical Volumes may not be negative");
		return 1;
	}

	if (arg_count(cmd, metadatacopies_ARG)) {
		vp_new->vgmetadatacopies = arg_int_value(cmd, metadatacopies_ARG,
							DEFAULT_VGMETADATACOPIES);
	} else if (arg_count(cmd, vgmetadatacopies_ARG)) {
		vp_new->vgmetadatacopies = arg_int_value(cmd, vgmetadatacopies_ARG,
							DEFAULT_VGMETADATACOPIES);
	} else {
		vp_new->vgmetadatacopies = find_config_tree_int(cmd,
						   "metadata/vgmetadatacopies",
						   DEFAULT_VGMETADATACOPIES);
	}

	return 0;
}

int lv_refresh(struct cmd_context *cmd, struct logical_volume *lv)
{
	int r = 0;

	if (!cmd->partial_activation && (lv->status & PARTIAL_LV)) {
		log_error("Refusing refresh of partial LV %s. Use --partial to override.",
			  lv->name);
		goto out;
	}

	r = suspend_lv(cmd, lv);
	if (!r)
		goto_out;

	r = resume_lv(cmd, lv);
	if (!r)
		goto_out;

	/*
	 * check if snapshot merge should be polled
	 * - unfortunately: even though the dev_manager will clear
	 *   the lv's merge attributes if a merge is not possible;
	 *   it is clearing a different instance of the lv (as
	 *   retrieved with lv_from_lvid)
	 * - fortunately: polldaemon will immediately shutdown if the
	 *   origin doesn't have a status with a snapshot percentage
	 */
	if (background_polling() && lv_is_origin(lv) && lv_is_merging_origin(lv))
		lv_spawn_background_polling(cmd, lv);

out:
	return r;
}

int vg_refresh_visible(struct cmd_context *cmd, struct volume_group *vg)
{
	struct lv_list *lvl;
	int r = 1;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_is_visible(lvl->lv))
			if (!lv_refresh(cmd, lvl->lv))
				r = 0;

	return r;
}

void lv_spawn_background_polling(struct cmd_context *cmd,
				 struct logical_volume *lv)
{
	const char *pvname;

	if ((lv->status & PVMOVE) &&
	    (pvname = get_pvmove_pvname_from_lv_mirr(lv))) {
		log_verbose("Spawning background pvmove process for %s",
			    pvname);
		pvmove_poll(cmd, pvname, 1);
	} else if ((lv->status & LOCKED) &&
	    (pvname = get_pvmove_pvname_from_lv(lv))) {
		log_verbose("Spawning background pvmove process for %s",
			    pvname);
		pvmove_poll(cmd, pvname, 1);
	}

	if (lv->status & (CONVERTING|MERGING)) {
		log_verbose("Spawning background lvconvert process for %s",
			lv->name);
		lvconvert_poll(cmd, lv, 1);
	}
}

/*
 * Intial sanity checking of non-recovery related command-line arguments.
 *
 * Output arguments:
 * pp: structure allocated by caller, fields written / validated here
 */
int pvcreate_params_validate(struct cmd_context *cmd,
			     int argc, char **argv,
			     struct pvcreate_params *pp)
{
	if (!argc) {
		log_error("Please enter a physical volume path");
		return 0;
	}

	pp->yes = arg_count(cmd, yes_ARG);
	pp->force = arg_count(cmd, force_ARG);

	if (arg_int_value(cmd, labelsector_ARG, 0) >= LABEL_SCAN_SECTORS) {
		log_error("labelsector must be less than %lu",
			  LABEL_SCAN_SECTORS);
		return 0;
	} else {
		pp->labelsector = arg_int64_value(cmd, labelsector_ARG,
						  DEFAULT_LABELSECTOR);
	}

	if (!(cmd->fmt->features & FMT_MDAS) &&
	    (arg_count(cmd, pvmetadatacopies_ARG) ||
	     arg_count(cmd, metadatasize_ARG)   ||
	     arg_count(cmd, dataalignment_ARG)  ||
	     arg_count(cmd, dataalignmentoffset_ARG))) {
		log_error("Metadata and data alignment parameters only "
			  "apply to text format.");
		return 0;
	}

	if (arg_count(cmd, pvmetadatacopies_ARG) &&
	    arg_int_value(cmd, pvmetadatacopies_ARG, -1) > 2) {
		log_error("Metadatacopies may only be 0, 1 or 2");
		return 0;
	}

	if (arg_count(cmd, metadataignore_ARG)) {
		pp->metadataignore = !strcmp(arg_str_value(cmd,
						metadataignore_ARG,
						DEFAULT_PVMETADATAIGNORE_STR),
					 "y");
	} else {
		pp->metadataignore = !strcmp(find_config_tree_str(cmd,
					"metadata/pvmetadataignore",
					DEFAULT_PVMETADATAIGNORE_STR),
					"y");
	}
	if (arg_count(cmd, pvmetadatacopies_ARG) &&
	    !arg_int_value(cmd, pvmetadatacopies_ARG, -1) &&
	    pp->metadataignore) {
		log_error("metadataignore only applies to metadatacopies > 0");
		return 0;
	}

	if (arg_count(cmd, zero_ARG))
		pp->zero = strcmp(arg_str_value(cmd, zero_ARG, "y"), "n");

	if (arg_sign_value(cmd, dataalignment_ARG, 0) == SIGN_MINUS) {
		log_error("Physical volume data alignment may not be negative");
		return 0;
	}
	pp->data_alignment = arg_uint64_value(cmd, dataalignment_ARG, UINT64_C(0));

	if (pp->data_alignment > ULONG_MAX) {
		log_error("Physical volume data alignment is too big.");
		return 0;
	}

	if (pp->data_alignment && pp->pe_start) {
		if (pp->pe_start % pp->data_alignment)
			log_warn("WARNING: Ignoring data alignment %" PRIu64
				 " incompatible with --restorefile value (%"
				 PRIu64").", pp->data_alignment, pp->pe_start);
		pp->data_alignment = 0;
	}

	if (arg_sign_value(cmd, dataalignmentoffset_ARG, 0) == SIGN_MINUS) {
		log_error("Physical volume data alignment offset may not be negative");
		return 0;
	}
	pp->data_alignment_offset = arg_uint64_value(cmd, dataalignmentoffset_ARG, UINT64_C(0));

	if (pp->data_alignment_offset > ULONG_MAX) {
		log_error("Physical volume data alignment offset is too big.");
		return 0;
	}

	if (pp->data_alignment_offset && pp->pe_start) {
		log_warn("WARNING: Ignoring data alignment offset %" PRIu64
			 " incompatible with --restorefile value (%"
			 PRIu64").", pp->data_alignment_offset, pp->pe_start);
		pp->data_alignment_offset = 0;
	}

	if (arg_sign_value(cmd, metadatasize_ARG, 0) == SIGN_MINUS) {
		log_error("Metadata size may not be negative");
		return 0;
	}

	pp->pvmetadatasize = arg_uint64_value(cmd, metadatasize_ARG, UINT64_C(0));
	if (!pp->pvmetadatasize)
		pp->pvmetadatasize = find_config_tree_int(cmd,
						 "metadata/pvmetadatasize",
						 DEFAULT_PVMETADATASIZE);

	pp->pvmetadatacopies = arg_int_value(cmd, pvmetadatacopies_ARG, -1);
	if (pp->pvmetadatacopies < 0)
		pp->pvmetadatacopies = find_config_tree_int(cmd,
						   "metadata/pvmetadatacopies",
						   DEFAULT_PVMETADATACOPIES);

	return 1;
}

int get_activation_monitoring_mode(struct cmd_context *cmd,
				   struct volume_group *vg,
				   int *monitoring_mode)
{
	*monitoring_mode = DEFAULT_DMEVENTD_MONITOR;

	if (arg_count(cmd, monitor_ARG) &&
	    (arg_count(cmd, ignoremonitoring_ARG) ||
	     arg_count(cmd, sysinit_ARG))) {
		log_error("--ignoremonitoring or --sysinit option not allowed with --monitor option");
		return 0;
	}

	if (arg_count(cmd, monitor_ARG))
		*monitoring_mode = arg_int_value(cmd, monitor_ARG,
						 DEFAULT_DMEVENTD_MONITOR);
	else if (is_static() || arg_count(cmd, ignoremonitoring_ARG) ||
		 arg_count(cmd, sysinit_ARG) ||
		 !find_config_tree_bool(cmd, "activation/monitoring",
					DEFAULT_DMEVENTD_MONITOR))
		*monitoring_mode = DMEVENTD_MONITOR_IGNORE;

	if (vg && vg_is_clustered(vg) &&
	    *monitoring_mode == DMEVENTD_MONITOR_IGNORE) {
		log_error("%s is incompatible with clustered Volume Group "
			  "\"%s\": Skipping.",
			  (arg_count(cmd, ignoremonitoring_ARG) ?
			   "--ignoremonitoring" : "activation/monitoring=0"),
			  vg->name);
		return 0;
	}
	
	return 1;
}

/*
 * Generic stripe parameter checks.
 */
static int _validate_stripe_params(struct cmd_context *cmd, uint32_t *stripes,
				   uint32_t *stripe_size)
{
	if (*stripes == 1 && *stripe_size) {
		log_print("Ignoring stripesize argument with single stripe");
		*stripe_size = 0;
	}

	if (*stripes > 1 && !*stripe_size) {
		*stripe_size = find_config_tree_int(cmd, "metadata/stripesize", DEFAULT_STRIPESIZE) * 2;
		log_print("Using default stripesize %s",
			  display_size(cmd, (uint64_t) *stripe_size));
	}

	if (*stripes < 1 || *stripes > MAX_STRIPES) {
		log_error("Number of stripes (%d) must be between %d and %d",
			  *stripes, 1, MAX_STRIPES);
		return 0;
	}

	if (*stripes > 1 && (*stripe_size < STRIPE_SIZE_MIN ||
			     *stripe_size & (*stripe_size - 1))) {
		log_error("Invalid stripe size %s",
			  display_size(cmd, (uint64_t) *stripe_size));
		return 0;
	}

	return 1;
}

/*
 * The stripe size is limited by the size of a uint32_t, but since the
 * value given by the user is doubled, and the final result must be a
 * power of 2, we must divide UINT_MAX by four and add 1 (to round it
 * up to the power of 2)
 */
int get_stripe_params(struct cmd_context *cmd, uint32_t *stripes, uint32_t *stripe_size)
{
	/* stripes_long_ARG takes precedence (for lvconvert) */
	*stripes = arg_uint_value(cmd, arg_count(cmd, stripes_long_ARG) ? stripes_long_ARG : stripes_ARG, 1);

	*stripe_size = arg_uint_value(cmd, stripesize_ARG, 0);
	if (*stripe_size) {
		if (arg_sign_value(cmd, stripesize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative stripesize is invalid");
			return 0;
		}

		if(arg_uint64_value(cmd, stripesize_ARG, 0) > STRIPE_SIZE_LIMIT * 2) {
			log_error("Stripe size cannot be larger than %s",
				  display_size(cmd, (uint64_t) STRIPE_SIZE_LIMIT));
			return 0;
		}
	}

	return _validate_stripe_params(cmd, stripes, stripe_size);
}

/* FIXME move to lib */
static int _pv_change_tag(struct physical_volume *pv, const char *tag, int addtag)
{
	if (addtag) {
		if (!str_list_add(pv->fmt->cmd->mem, &pv->tags, tag)) {
			log_error("Failed to add tag %s to physical volume %s",
				  tag, pv_dev_name(pv));
			return 0;
		}
	} else if (!str_list_del(&pv->tags, tag)) {
		log_error("Failed to remove tag %s from physical volume" "%s",
			  tag,  pv_dev_name(pv));
		return 0;
	}

	return 1;
}

/* Set exactly one of VG, LV or PV */
int change_tag(struct cmd_context *cmd, struct volume_group *vg,
	       struct logical_volume *lv, struct physical_volume *pv, int arg)
{
	const char *tag;
	struct arg_value_group_list *current_group;

	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(current_group->arg_values, arg))
			continue;

		if (!(tag = grouped_arg_str_value(current_group->arg_values, arg, NULL))) {
			log_error("Failed to get tag");
			return 0;
		}

		if (vg && !vg_change_tag(vg, tag, arg == addtag_ARG))
			return_0;
		else if (lv && !lv_change_tag(lv, tag, arg == addtag_ARG))
			return_0;
		else if (pv && !_pv_change_tag(pv, tag, arg == addtag_ARG))
			return_0;
	}

	return 1;
}
