/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
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
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/utsname.h>

struct device_id_list {
	struct dm_list list;
	struct device *dev;
	char pvid[ID_LEN + 1];
};

const char *command_name(struct cmd_context *cmd)
{
	return cmd->command->name;
}

static void _sigchld_handler(int sig __attribute__((unused)))
{
	while (wait4(-1, NULL, WNOHANG | WUNTRACED, NULL) > 0) ;
}

/*
 * returns:
 * -1 if the fork failed
 *  0 if the parent
 *  1 if the child
 */
int become_daemon(struct cmd_context *cmd, int skip_lvm)
{
	static const char devnull[] = "/dev/null";
	int null_fd;
	pid_t pid;
	struct sigaction act = {
		{_sigchld_handler},
		.sa_flags = SA_NOCLDSTOP,
	};

	log_verbose("Forking background process: %s", cmd->cmd_line);

	sigaction(SIGCHLD, &act, NULL);

	if (!skip_lvm)
		sync_local_dev_names(cmd); /* Flush ops and reset dm cookie */

	if ((pid = fork()) == -1) {
		log_error("fork failed: %s", strerror(errno));
		return -1;
	}

	/* Parent */
	if (pid > 0)
		return 0;

	/* Child */
	if (setsid() == -1)
		log_error("Background process failed to setsid: %s",
			  strerror(errno));

/* Set this to avoid discarding output from background process */
// #define DEBUG_CHILD

#ifndef DEBUG_CHILD
	if ((null_fd = open(devnull, O_RDWR)) == -1) {
		log_sys_error("open", devnull);
		_exit(ECMD_FAILED);
	}

	if ((dup2(null_fd, STDIN_FILENO) < 0)  || /* reopen stdin */
	    (dup2(null_fd, STDOUT_FILENO) < 0) || /* reopen stdout */
	    (dup2(null_fd, STDERR_FILENO) < 0)) { /* reopen stderr */
		log_sys_error("dup2", "redirect");
		(void) close(null_fd);
		_exit(ECMD_FAILED);
	}

	if (null_fd > STDERR_FILENO)
		(void) close(null_fd);

	init_verbose(VERBOSE_BASE_LEVEL);
#endif	/* DEBUG_CHILD */

	strncpy(*cmd->argv, "(lvm2)", strlen(*cmd->argv));

	lvmetad_disconnect();

	if (!skip_lvm) {
		reset_locking();
		lvmcache_destroy(cmd, 1, 1);
		if (!lvmcache_init())
			/* FIXME Clean up properly here */
			_exit(ECMD_FAILED);
	}
	dev_close_all();

	return 1;
}

/*
 * Strip dev_dir if present
 */
const char *skip_dev_dir(struct cmd_context *cmd, const char *vg_name,
			 unsigned *dev_dir_found)
{
	size_t devdir_len = strlen(cmd->dev_dir);
	const char *dmdir = dm_dir() + devdir_len;
	size_t dmdir_len = strlen(dmdir), vglv_sz;
	char *vgname, *lvname, *layer, *vglv;

	/* FIXME Do this properly */
	if (*vg_name == '/')
		while (vg_name[1] == '/')
			vg_name++;

	if (strncmp(vg_name, cmd->dev_dir, devdir_len)) {
		if (dev_dir_found)
			*dev_dir_found = 0;
	} else {
		if (dev_dir_found)
			*dev_dir_found = 1;

		vg_name += devdir_len;
		while (*vg_name == '/')
			vg_name++;

		/* Reformat string if /dev/mapper found */
		if (!strncmp(vg_name, dmdir, dmdir_len) && vg_name[dmdir_len] == '/') {
			vg_name += dmdir_len + 1;
			while (*vg_name == '/')
				vg_name++;

			if (!dm_split_lvm_name(cmd->mem, vg_name, &vgname, &lvname, &layer) ||
			    *layer) {
				log_error("skip_dev_dir: Couldn't split up device name %s.",
					  vg_name);
				return vg_name;
			}
			vglv_sz = strlen(vgname) + strlen(lvname) + 2;
			if (!(vglv = dm_pool_alloc(cmd->mem, vglv_sz)) ||
			    dm_snprintf(vglv, vglv_sz, "%s%s%s", vgname,
					*lvname ? "/" : "",
					lvname) < 0) {
				log_error("vg/lv string alloc failed.");
				return vg_name;
			}
			return vglv;
		}
	}

	return vg_name;
}

/*
 * Three possible results:
 * a) return 0, skip 0: take the VG, and cmd will end in success
 * b) return 0, skip 1: skip the VG, and cmd will end in success
 * c) return 1, skip *: skip the VG, and cmd will end in failure
 *
 * Case b is the special case, and includes the following:
 * . The VG is inconsistent, and the command allows for inconsistent VGs.
 * . The VG is clustered, the host cannot access clustered VG's,
 *   and the command option has been used to ignore clustered vgs.
 *
 * Case c covers the other errors returned when reading the VG.
 *   If *skip is 1, it's OK for the caller to read the list of PVs in the VG.
 */
static int _ignore_vg(struct volume_group *vg, const char *vg_name,
		      struct dm_list *arg_vgnames, int allow_inconsistent, int *skip)
{
	uint32_t read_error = vg_read_error(vg);
	*skip = 0;

	if ((read_error & FAILED_INCONSISTENT) && allow_inconsistent)
		read_error &= ~FAILED_INCONSISTENT; /* Check for other errors */

	if ((read_error & FAILED_CLUSTERED) && vg->cmd->ignore_clustered_vgs) {
		read_error &= ~FAILED_CLUSTERED; /* Check for other errors */
		log_verbose("Skipping volume group %s", vg_name);
		*skip = 1;
	}

	/*
	 * Commands that operate on "all vgs" shouldn't be bothered by
	 * skipping a foreign VG, and the command shouldn't fail when
	 * one is skipped.  But, if the command explicitly asked to
	 * operate on a foreign VG and it's skipped, then the command
	 * would expect to fail.
	 */
	if (read_error & FAILED_SYSTEMID) {
		if (arg_vgnames && str_list_match_item(arg_vgnames, vg->name)) {
			log_error("Cannot access VG %s with system ID %s with %slocal system ID%s%s.",
				  vg->name, vg->system_id, vg->cmd->system_id ? "" : "unknown ",
				  vg->cmd->system_id ? " " : "", vg->cmd->system_id ? vg->cmd->system_id : "");
			return 1;
		} else {
			read_error &= ~FAILED_SYSTEMID; /* Check for other errors */
			log_verbose("Skipping foreign volume group %s", vg_name);
			*skip = 1;
		}
	}

	if (read_error == FAILED_CLUSTERED) {
		*skip = 1;
		stack;	/* Error already logged */
		return 1;
	}

	if (read_error != SUCCESS) {
		*skip = 0;
		log_error("Cannot process volume group %s", vg_name);
		return 1;
	}

	return 0;
}

/*
 * This functiona updates the "selected" arg only if last item processed
 * is selected so this implements the "whole structure is selected if
 * at least one of its items is selected".
 */
static void _update_selection_result(struct processing_handle *handle, int *selected)
{
	if (!handle || !handle->selection_handle)
		return;

	if (handle->selection_handle->selected)
		*selected = 1;
}

static void _set_final_selection_result(struct processing_handle *handle, int selected)
{
	if (!handle || !handle->selection_handle)
		return;

	handle->selection_handle->selected = selected;
}

/*
 * Metadata iteration functions
 */
int process_each_segment_in_pv(struct cmd_context *cmd,
			       struct volume_group *vg,
			       struct physical_volume *pv,
			       struct processing_handle *handle,
			       process_single_pvseg_fn_t process_single_pvseg)
{
	struct pv_segment *pvseg;
	int whole_selected = 0;
	int ret_max = ECMD_PROCESSED;
	int ret;
	struct pv_segment _free_pv_segment = { .pv = pv };

	if (dm_list_empty(&pv->segments)) {
		ret = process_single_pvseg(cmd, NULL, &_free_pv_segment, handle);
		if (ret != ECMD_PROCESSED)
			stack;
		if (ret > ret_max)
			ret_max = ret;
	} else {
		dm_list_iterate_items(pvseg, &pv->segments) {
			if (sigint_caught())
				return_ECMD_FAILED;

			ret = process_single_pvseg(cmd, vg, pvseg, handle);
			_update_selection_result(handle, &whole_selected);
			if (ret != ECMD_PROCESSED)
				stack;
			if (ret > ret_max)
				ret_max = ret;
		}
	}

	/* the PV is selected if at least one PV segment is selected */
	_set_final_selection_result(handle, whole_selected);
	return ret_max;
}

int process_each_segment_in_lv(struct cmd_context *cmd,
			       struct logical_volume *lv,
			       struct processing_handle *handle,
			       process_single_seg_fn_t process_single_seg)
{
	struct lv_segment *seg;
	int whole_selected = 0;
	int ret_max = ECMD_PROCESSED;
	int ret;

	dm_list_iterate_items(seg, &lv->segments) {
		if (sigint_caught())
			return_ECMD_FAILED;

		ret = process_single_seg(cmd, seg, handle);
		_update_selection_result(handle, &whole_selected);
		if (ret != ECMD_PROCESSED)
			stack;
		if (ret > ret_max)
			ret_max = ret;
	}

	/* the LV is selected if at least one LV segment is selected */
	_set_final_selection_result(handle, whole_selected);
	return ret_max;
}

static const char *_extract_vgname(struct cmd_context *cmd, const char *lv_name,
				   const char **after)
{
	const char *vg_name = lv_name;
	char *st, *pos;

	/* Strip dev_dir (optional) */
	if (!(vg_name = skip_dev_dir(cmd, vg_name, NULL)))
		return_0;

	/* Require exactly one set of consecutive slashes */
	if ((st = pos = strchr(vg_name, '/')))
		while (*st == '/')
			st++;

	if (!st || strchr(st, '/')) {
		log_error("\"%s\": Invalid path for Logical Volume.",
			  lv_name);
		return 0;
	}

	if (!(vg_name = dm_pool_strndup(cmd->mem, vg_name, pos - vg_name))) {
		log_error("Allocation of vg_name failed.");
		return 0;
	}

	if (after)
		*after = st;

	return vg_name;
}

/*
 * Extract default volume group name from environment
 */
static const char *_default_vgname(struct cmd_context *cmd)
{
	const char *vg_path;

	/* Take default VG from environment? */
	vg_path = getenv("LVM_VG_NAME");
	if (!vg_path)
		return 0;

	vg_path = skip_dev_dir(cmd, vg_path, NULL);

	if (strchr(vg_path, '/')) {
		log_error("\"%s\": Invalid environment var LVM_VG_NAME set for Volume Group.",
			  vg_path);
		return 0;
	}

	return dm_pool_strdup(cmd->mem, vg_path);
}

/*
 * Determine volume group name from a logical volume name
 */
const char *extract_vgname(struct cmd_context *cmd, const char *lv_name)
{
	const char *vg_name = lv_name;

	/* Path supplied? */
	if (vg_name && strchr(vg_name, '/')) {
		if (!(vg_name = _extract_vgname(cmd, lv_name, NULL)))
			return_NULL;

		return vg_name;
	}

	if (!(vg_name = _default_vgname(cmd))) {
		if (lv_name)
			log_error("Path required for Logical Volume \"%s\".",
				  lv_name);
		return NULL;
	}

	return vg_name;
}

/*
 * Process physical extent range specifiers
 */
static int _add_pe_range(struct dm_pool *mem, const char *pvname,
			 struct dm_list *pe_ranges, uint32_t start, uint32_t count)
{
	struct pe_range *per;

	log_debug("Adding PE range: start PE %" PRIu32 " length %" PRIu32
		  " on %s.", start, count, pvname);

	/* Ensure no overlap with existing areas */
	dm_list_iterate_items(per, pe_ranges) {
		if (((start < per->start) && (start + count - 1 >= per->start)) ||
		    ((start >= per->start) &&
			(per->start + per->count - 1) >= start)) {
			log_error("Overlapping PE ranges specified (%" PRIu32
				  "-%" PRIu32 ", %" PRIu32 "-%" PRIu32 ")"
				  " on %s.",
				  start, start + count - 1, per->start,
				  per->start + per->count - 1, pvname);
			return 0;
		}
	}

	if (!(per = dm_pool_alloc(mem, sizeof(*per)))) {
		log_error("Allocation of list failed.");
		return 0;
	}

	per->start = start;
	per->count = count;
	dm_list_add(pe_ranges, &per->list);

	return 1;
}

static int _xstrtouint32(const char *s, char **p, int base, uint32_t *result)
{
	unsigned long ul;

	errno = 0;
	ul = strtoul(s, p, base);

	if (errno || *p == s || ul > UINT32_MAX)
		return 0;

	*result = ul;

	return 1;
}

static int _parse_pes(struct dm_pool *mem, char *c, struct dm_list *pe_ranges,
		      const char *pvname, uint32_t size)
{
	char *endptr;
	uint32_t start, end, len;

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
			if (!_xstrtouint32(c, &endptr, 10, &start))
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
				if (!_xstrtouint32(c, &endptr, 10, &end))
					goto error;
				c = endptr;
			}
		} else if (*c == '+') {	/* Length? */
			c++;
			if (isdigit(*c)) {
				if (!_xstrtouint32(c, &endptr, 10, &len))
					goto error;
				c = endptr;
				end = start + (len ? (len - 1) : 0);
			}
		}

		if (*c && *c != ':')
			goto error;

		if ((start > end) || (end > size - 1)) {
			log_error("PE range error: start extent %" PRIu32 " to "
				  "end extent %" PRIu32 ".", start, end);
			return 0;
		}

		if (!_add_pe_range(mem, pvname, pe_ranges, start, end - start + 1))
			return_0;

	}

	return 1;

      error:
	log_error("Physical extent parsing error at %s.", c);
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
		log_warn("Physical volume %s not allocatable.", pvname);
		return 1;
	}

	if (allocatable_only && is_missing_pv(pvl->pv)) {
		log_warn("Physical volume %s is missing.", pvname);
		return 1;
	}

	if (allocatable_only &&
	    (pvl->pv->pe_count == pvl->pv->pe_alloc_count)) {
		log_warn("No free extents on physical volume \"%s\".", pvname);
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
			log_error("Allocation of pe_ranges list failed.");
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
	struct dm_list tagsl, arg_pvnames;
	char *pvname = NULL;
	char *colon, *at_sign, *tagname;
	int i;

	/* Build up list of PVs */
	if (!(r = dm_pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed");
		return NULL;
	}
	dm_list_init(r);

	dm_list_init(&tagsl);
	dm_list_init(&arg_pvnames);

	for (i = 0; i < argc; i++) {
		dm_unescape_colons_and_at_signs(argv[i], &colon, &at_sign);

		if (at_sign && (at_sign == argv[i])) {
			tagname = at_sign + 1;
			if (!validate_tag(tagname)) {
				log_error("Skipping invalid tag %s.", tagname);
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
			log_error("Failed to clone PV name.");
			return NULL;
		}

		if (!(pvl = find_pv_in_vg(vg, pvname))) {
			log_error("Physical Volume \"%s\" not found in "
				  "Volume Group \"%s\".", pvname, vg->name);
			return NULL;
		}
		if (!_create_pv_entry(mem, pvl, colon, allocatable_only, r))
			return_NULL;
	}

	if (dm_list_empty(r))
		log_error("No specified PVs have space available.");

	return dm_list_empty(r) ? NULL : r;
}

struct dm_list *clone_pv_list(struct dm_pool *mem, struct dm_list *pvsl)
{
	struct dm_list *r;
	struct pv_list *pvl, *new_pvl;

	/* Build up list of PVs */
	if (!(r = dm_pool_alloc(mem, sizeof(*r)))) {
		log_error("Allocation of list failed.");
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

const char _pe_size_may_not_be_negative_msg[] = "Physical extent size may not be negative.";

int vgcreate_params_set_defaults(struct cmd_context *cmd,
				 struct vgcreate_params *vp_def,
				 struct volume_group *vg)
{
	int64_t extent_size;

	/* Only vgsplit sets vg */
	if (vg) {
		vp_def->vg_name = NULL;
		vp_def->extent_size = vg->extent_size;
		vp_def->max_pv = vg->max_pv;
		vp_def->max_lv = vg->max_lv;
		vp_def->alloc = vg->alloc;
		vp_def->clustered = vg_is_clustered(vg);
		vp_def->vgmetadatacopies = vg->mda_copies;
		vp_def->system_id = vg->system_id;	/* No need to clone this */
	} else {
		vp_def->vg_name = NULL;
		extent_size = find_config_tree_int64(cmd,
				allocation_physical_extent_size_CFG, NULL) * 2;
		if (extent_size < 0) {
			log_error(_pe_size_may_not_be_negative_msg);
			return 0;
		}
		vp_def->extent_size = (uint32_t) extent_size;
		vp_def->max_pv = DEFAULT_MAX_PV;
		vp_def->max_lv = DEFAULT_MAX_LV;
		vp_def->alloc = DEFAULT_ALLOC_POLICY;
		vp_def->clustered = DEFAULT_CLUSTERED;
		vp_def->vgmetadatacopies = DEFAULT_VGMETADATACOPIES;
		vp_def->system_id = cmd->system_id;
	}

	return 1;
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
	const char *system_id_arg_str;

	vp_new->vg_name = skip_dev_dir(cmd, vp_def->vg_name, NULL);
	vp_new->max_lv = arg_uint_value(cmd, maxlogicalvolumes_ARG,
					vp_def->max_lv);
	vp_new->max_pv = arg_uint_value(cmd, maxphysicalvolumes_ARG,
					vp_def->max_pv);
	vp_new->alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, vp_def->alloc);

	/* Units of 512-byte sectors */
	vp_new->extent_size =
	    arg_uint_value(cmd, physicalextentsize_ARG, vp_def->extent_size);

	if (arg_count(cmd, clustered_ARG))
		vp_new->clustered = arg_int_value(cmd, clustered_ARG, vp_def->clustered);
	else
		/* Default depends on current locking type */
		vp_new->clustered = locking_is_clustered();

	if (arg_sign_value(cmd, physicalextentsize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error(_pe_size_may_not_be_negative_msg);
		return 0;
	}

	if (arg_uint64_value(cmd, physicalextentsize_ARG, 0) > MAX_EXTENT_SIZE) {
		log_error("Physical extent size cannot be larger than %s.",
				  display_size(cmd, (uint64_t) MAX_EXTENT_SIZE));
		return 0;
	}

	if (arg_sign_value(cmd, maxlogicalvolumes_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Max Logical Volumes may not be negative.");
		return 0;
	}

	if (arg_sign_value(cmd, maxphysicalvolumes_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Max Physical Volumes may not be negative.");
		return 0;
	}

	if (arg_count(cmd, metadatacopies_ARG))
		vp_new->vgmetadatacopies = arg_int_value(cmd, metadatacopies_ARG,
							DEFAULT_VGMETADATACOPIES);
	else if (arg_count(cmd, vgmetadatacopies_ARG))
		vp_new->vgmetadatacopies = arg_int_value(cmd, vgmetadatacopies_ARG,
							DEFAULT_VGMETADATACOPIES);
	else
		vp_new->vgmetadatacopies = find_config_tree_int(cmd, metadata_vgmetadatacopies_CFG, NULL);

	/* A clustered VG has no system ID. */
	if (vp_new->clustered) {
		if (arg_is_set(cmd, systemid_ARG)) {
			log_error("system ID cannot be set on clustered Volume Groups.");
			return 0;
		}
		vp_new->system_id = NULL;
	} else if (!(system_id_arg_str = arg_str_value(cmd, systemid_ARG, NULL)))
		vp_new->system_id = vp_def->system_id;
	else {
		if (!(vp_new->system_id = system_id_from_string(cmd, system_id_arg_str)))
			return_0;

		/* FIXME Take local/extra_system_ids into account */
		if (vp_new->system_id && cmd->system_id &&
		    strcmp(vp_new->system_id, cmd->system_id)) {
			if (*vp_new->system_id)
				log_warn("VG with system ID %s might become inaccessible as local system ID is %s",
					 vp_new->system_id, cmd->system_id);
			else
				log_warn("WARNING: A VG without a system ID allows unsafe access from other hosts.");
		}
	}

	return 1;
}

/* Shared code for changing activation state for vgchange/lvchange */
int lv_change_activate(struct cmd_context *cmd, struct logical_volume *lv,
		       activation_change_t activate)
{
	int r = 1;

	if (lv_is_cache_pool(lv)) {
		if (is_change_activating(activate)) {
			log_verbose("Skipping activation of cache pool %s.",
				    display_lvname(lv));
			return 1;
		}
		if (!dm_list_empty(&lv->segs_using_this_lv)) {
			log_verbose("Skipping deactivation of used cache pool %s.",
				    display_lvname(lv));
			return 1;
		}
		/*
		 * Allow to pass only deactivation of unused cache pool.
		 * Useful only for recovery of failed zeroing of metadata LV.
		 */
	}

	if (lv_is_merging_origin(lv)) {
		/*
		 * For merging origin, its snapshot must be inactive.
		 * If it's still active and cannot be deactivated
		 * activation or deactivation of origin fails!
		 *
		 * When origin is deactivated and merging snapshot is thin
		 * it allows to deactivate origin, but still report error,
		 * since the thin snapshot remains active.
		 *
		 * User could retry to deactivate it with another
		 * deactivation of origin, which is the only visible LV
		 */
		if (!deactivate_lv(cmd, find_snapshot(lv)->lv)) {
			if (is_change_activating(activate)) {
				log_error("Refusing to activate merging \"%s\" while snapshot \"%s\" is still active.",
					  lv->name, find_snapshot(lv)->lv->name);
				return 0;
			}

			log_error("Cannot fully deactivate merging origin \"%s\" while snapshot \"%s\" is still active.",
				  lv->name, find_snapshot(lv)->lv->name);
			r = 0; /* and continue to deactivate origin... */
		}
	}

	if (!lv_active_change(cmd, lv, activate, 0))
		return_0;

	return r;
}

int lv_refresh(struct cmd_context *cmd, struct logical_volume *lv)
{
	if (!lv_refresh_suspend_resume(cmd, lv))
		return_0;

	/*
	 * check if snapshot merge should be polled
	 * - unfortunately: even though the dev_manager will clear
	 *   the lv's merge attributes if a merge is not possible;
	 *   it is clearing a different instance of the lv (as
	 *   retrieved with lv_from_lvid)
	 * - fortunately: polldaemon will immediately shutdown if the
	 *   origin doesn't have a status with a snapshot percentage
	 */
	if (background_polling() && lv_is_merging_origin(lv) && lv_is_active_locally(lv))
		lv_spawn_background_polling(cmd, lv);

	return 1;
}

int vg_refresh_visible(struct cmd_context *cmd, struct volume_group *vg)
{
	struct lv_list *lvl;
	int r = 1;

	sigint_allow();
	dm_list_iterate_items(lvl, &vg->lvs) {
		if (sigint_caught()) {
			r = 0;
			stack;
			break;
		}

		if (lv_is_visible(lvl->lv) && !lv_refresh(cmd, lvl->lv)) {
			r = 0;
			stack;
		}
	}

	sigint_restore();

	return r;
}

void lv_spawn_background_polling(struct cmd_context *cmd,
				 struct logical_volume *lv)
{
	const char *pvname;
	const struct logical_volume *lv_mirr = NULL;

	if (lv_is_pvmove(lv))
		lv_mirr = lv;
	else if (lv_is_locked(lv))
		lv_mirr = find_pvmove_lv_in_lv(lv);

	if (lv_mirr &&
	    (pvname = get_pvmove_pvname_from_lv_mirr(lv_mirr))) {
		log_verbose("Spawning background pvmove process for %s.",
			    pvname);
		pvmove_poll(cmd, pvname, lv_mirr->lvid.s, lv_mirr->vg->name, lv_mirr->name, 1);
	}

	if (lv_is_converting(lv) || lv_is_merging(lv)) {
		log_verbose("Spawning background lvconvert process for %s.",
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
int pvcreate_params_validate(struct cmd_context *cmd, int argc,
			     struct pvcreate_params *pp)
{
	if (!argc) {
		log_error("Please enter a physical volume path.");
		return 0;
	}

	pp->yes = arg_count(cmd, yes_ARG);
	pp->force = (force_t) arg_count(cmd, force_ARG);

	if (arg_int_value(cmd, labelsector_ARG, 0) >= LABEL_SCAN_SECTORS) {
		log_error("labelsector must be less than %lu.",
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

	if (!(cmd->fmt->features & FMT_BAS) &&
	    arg_count(cmd, bootloaderareasize_ARG)) {
		log_error("Bootloader area parameters only "
			  "apply to text format.");
		return 0;
	}

	if (arg_count(cmd, metadataignore_ARG))
		pp->metadataignore = arg_int_value(cmd, metadataignore_ARG,
						   DEFAULT_PVMETADATAIGNORE);
	else
		pp->metadataignore = find_config_tree_bool(cmd, metadata_pvmetadataignore_CFG, NULL);

	if (arg_count(cmd, pvmetadatacopies_ARG) &&
	    !arg_int_value(cmd, pvmetadatacopies_ARG, -1) &&
	    pp->metadataignore) {
		log_error("metadataignore only applies to metadatacopies > 0");
		return 0;
	}

	pp->zero = arg_int_value(cmd, zero_ARG, 1);

	if (arg_sign_value(cmd, dataalignment_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Physical volume data alignment may not be negative.");
		return 0;
	}
	pp->data_alignment = arg_uint64_value(cmd, dataalignment_ARG, UINT64_C(0));

	if (pp->data_alignment > UINT32_MAX) {
		log_error("Physical volume data alignment is too big.");
		return 0;
	}

	if (arg_sign_value(cmd, dataalignmentoffset_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Physical volume data alignment offset may not be negative");
		return 0;
	}
	pp->data_alignment_offset = arg_uint64_value(cmd, dataalignmentoffset_ARG, UINT64_C(0));

	if (pp->data_alignment_offset > UINT32_MAX) {
		log_error("Physical volume data alignment offset is too big.");
		return 0;
	}

	if ((pp->data_alignment + pp->data_alignment_offset) &&
	    (pp->rp.pe_start != PV_PE_START_CALC)) {
		if ((pp->data_alignment ? pp->rp.pe_start % pp->data_alignment : pp->rp.pe_start) != pp->data_alignment_offset) {
			log_warn("WARNING: Ignoring data alignment %s"
				 " incompatible with restored pe_start value %s)",
				 display_size(cmd, pp->data_alignment + pp->data_alignment_offset),
				 display_size(cmd, pp->rp.pe_start));
			pp->data_alignment = 0;
			pp->data_alignment_offset = 0;
		}
	}

	if (arg_sign_value(cmd, metadatasize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Metadata size may not be negative.");
		return 0;
	}

	if (arg_sign_value(cmd, bootloaderareasize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Bootloader area size may not be negative.");
		return 0;
	}

	pp->pvmetadatasize = arg_uint64_value(cmd, metadatasize_ARG, UINT64_C(0));
	if (!pp->pvmetadatasize)
		pp->pvmetadatasize = find_config_tree_int(cmd, metadata_pvmetadatasize_CFG, NULL);

	pp->pvmetadatacopies = arg_int_value(cmd, pvmetadatacopies_ARG, -1);
	if (pp->pvmetadatacopies < 0)
		pp->pvmetadatacopies = find_config_tree_int(cmd, metadata_pvmetadatacopies_CFG, NULL);

	pp->rp.ba_size = arg_uint64_value(cmd, bootloaderareasize_ARG, pp->rp.ba_size);

	return 1;
}

int get_activation_monitoring_mode(struct cmd_context *cmd,
				   int *monitoring_mode)
{
	*monitoring_mode = DEFAULT_DMEVENTD_MONITOR;

	if (arg_count(cmd, monitor_ARG) &&
	    (arg_count(cmd, ignoremonitoring_ARG) ||
	     arg_count(cmd, sysinit_ARG))) {
		log_error("--ignoremonitoring or --sysinit option not allowed with --monitor option.");
		return 0;
	}

	if (arg_count(cmd, monitor_ARG))
		*monitoring_mode = arg_int_value(cmd, monitor_ARG,
						 DEFAULT_DMEVENTD_MONITOR);
	else if (is_static() || arg_count(cmd, ignoremonitoring_ARG) ||
		 arg_count(cmd, sysinit_ARG) ||
		 !find_config_tree_bool(cmd, activation_monitoring_CFG, NULL))
		*monitoring_mode = DMEVENTD_MONITOR_IGNORE;

	return 1;
}

/*
 * Read pool options from cmdline
 */
int get_pool_params(struct cmd_context *cmd,
		    const struct segment_type *segtype,
		    int *passed_args,
		    uint64_t *pool_metadata_size,
		    int *pool_metadata_spare,
		    uint32_t *chunk_size,
		    thin_discards_t *discards,
		    int *zero)
{
	*passed_args = 0;

	if (segtype_is_thin_pool(segtype) || segtype_is_thin(segtype)) {
		if (arg_is_set(cmd, zero_ARG)) {
			*passed_args |= PASS_ARG_ZERO;
			*zero = arg_int_value(cmd, zero_ARG, 1);
			log_very_verbose("%s pool zeroing.", *zero ? "Enabling" : "Disabling");
		}
		if (arg_is_set(cmd, discards_ARG)) {
			*passed_args |= PASS_ARG_DISCARDS;
			*discards = (thin_discards_t) arg_uint_value(cmd, discards_ARG, 0);
			log_very_verbose("Setting pool discards to %s.",
					 get_pool_discards_name(*discards));
		}
	}

	if (arg_from_list_is_negative(cmd, "may not be negative",
				      chunksize_ARG,
				      pooldatasize_ARG,
				      poolmetadatasize_ARG,
				      -1))
		return_0;

	if (arg_from_list_is_zero(cmd, "may not be zero",
				  chunksize_ARG,
				  pooldatasize_ARG,
				  poolmetadatasize_ARG,
				  -1))
		return_0;

	if (arg_is_set(cmd, chunksize_ARG)) {
		*passed_args |= PASS_ARG_CHUNK_SIZE;
		*chunk_size = arg_uint_value(cmd, chunksize_ARG, 0);

		if (!validate_pool_chunk_size(cmd, segtype, *chunk_size))
			return_0;

		log_very_verbose("Setting pool chunk size to %s.",
				 display_size(cmd, *chunk_size));
	}

	if (arg_count(cmd, poolmetadatasize_ARG)) {
		if (arg_sign_value(cmd, poolmetadatasize_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Negative pool metadata size is invalid.");
			return 0;
		}

		if (arg_count(cmd, poolmetadata_ARG)) {
			log_error("Please specify either metadata logical volume or its size.");
			return 0;
		}

		*passed_args |= PASS_ARG_POOL_METADATA_SIZE;
		*pool_metadata_size = arg_uint64_value(cmd, poolmetadatasize_ARG,
						       UINT64_C(0));
	} else if (arg_count(cmd, poolmetadata_ARG))
		*passed_args |= PASS_ARG_POOL_METADATA_SIZE; /* fixed size */

	/* TODO: default in lvm.conf ? */
	*pool_metadata_spare = arg_int_value(cmd, poolmetadataspare_ARG,
					     DEFAULT_POOL_METADATA_SPARE);

	return 1;
}

/*
 * Generic stripe parameter checks.
 */
static int _validate_stripe_params(struct cmd_context *cmd, uint32_t *stripes,
				   uint32_t *stripe_size)
{
	if (*stripes == 1 && *stripe_size) {
		log_print_unless_silent("Ignoring stripesize argument with single stripe.");
		*stripe_size = 0;
	}

	if (*stripes > 1 && !*stripe_size) {
		*stripe_size = find_config_tree_int(cmd, metadata_stripesize_CFG, NULL) * 2;
		log_print_unless_silent("Using default stripesize %s.",
			  display_size(cmd, (uint64_t) *stripe_size));
	}

	if (*stripes < 1 || *stripes > MAX_STRIPES) {
		log_error("Number of stripes (%d) must be between %d and %d.",
			  *stripes, 1, MAX_STRIPES);
		return 0;
	}

	if (*stripes > 1 && (*stripe_size < STRIPE_SIZE_MIN ||
			     *stripe_size & (*stripe_size - 1))) {
		log_error("Invalid stripe size %s.",
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
		if (arg_sign_value(cmd, stripesize_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Negative stripesize is invalid.");
			return 0;
		}

		if (arg_uint64_value(cmd, stripesize_ARG, 0) > STRIPE_SIZE_LIMIT * 2) {
			log_error("Stripe size cannot be larger than %s.",
				  display_size(cmd, (uint64_t) STRIPE_SIZE_LIMIT));
			return 0;
		}
	}

	return _validate_stripe_params(cmd, stripes, stripe_size);
}

static int _validate_cachepool_params(struct dm_config_tree *tree)
{
	return 1;
}

struct dm_config_tree *get_cachepolicy_params(struct cmd_context *cmd)
{
	const char *str;
	struct arg_value_group_list *group;
	struct dm_config_tree *result = NULL, *prev = NULL, *current = NULL;
	struct dm_config_node *cn;
	int ok = 0;

	dm_list_iterate_items(group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(group->arg_values, cachesettings_ARG))
			continue;

		current = dm_config_create();
		if (!current)
			goto_out;
		if (prev)
			current->cascade = prev;
		prev = current;

		if (!(str = grouped_arg_str_value(group->arg_values,
						  cachesettings_ARG,
						  NULL)))
			goto_out;

		if (!dm_config_parse(current, str, str + strlen(str)))
			goto_out;
	}

	if (!(result = dm_config_flatten(current)))
		goto_out;

	if (result->root) {
		if (!(cn = dm_config_create_node(result, "policy_settings")))
			goto_out;

		cn->child = result->root;
		result->root = cn;
	}

	if (arg_count(cmd, cachepolicy_ARG)) {
		if (!(cn = dm_config_create_node(result, "policy")))
			goto_out;

		cn->sib = result->root;
		result->root = cn;
		if (!(cn->v = dm_config_create_value(result)))
			goto_out;

		cn->v->type = DM_CFG_STRING;
		cn->v->v.str = arg_str_value(cmd, cachepolicy_ARG, NULL);
	}

	if (!_validate_cachepool_params(result))
		goto_out;

	ok = 1;

out:
	if (!ok && result) {
		dm_config_destroy(result);
		result = NULL;
	}
	while (prev) {
		current = prev->cascade;
		dm_config_destroy(prev);
		prev = current;
	}
	return result;
}

/* FIXME move to lib */
static int _pv_change_tag(struct physical_volume *pv, const char *tag, int addtag)
{
	if (addtag) {
		if (!str_list_add(pv->fmt->cmd->mem, &pv->tags, tag)) {
			log_error("Failed to add tag %s to physical volume %s.",
				  tag, pv_dev_name(pv));
			return 0;
		}
	} else
		str_list_del(&pv->tags, tag);

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
			log_error("Failed to get tag.");
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

int process_each_label(struct cmd_context *cmd, int argc, char **argv,
		       struct processing_handle *handle,
		       process_single_label_fn_t process_single_label)
{
	struct label *label;
	struct dev_iter *iter;
	struct device *dev;

	int ret_max = ECMD_PROCESSED;
	int ret;
	int opt = 0;

	if (argc) {
		for (; opt < argc; opt++) {
			if (!(dev = dev_cache_get(argv[opt], cmd->full_filter))) {
				log_error("Failed to find device "
					  "\"%s\".", argv[opt]);
				ret_max = ECMD_FAILED;
				continue;
			}

			if (!label_read(dev, &label, 0)) {
				log_error("No physical volume label read from %s.",
					  argv[opt]);
				ret_max = ECMD_FAILED;
				continue;
			}

			ret = process_single_label(cmd, label, handle);

			if (ret > ret_max)
				ret_max = ret;

			if (sigint_caught())
				break;
		}

		return ret_max;
	}

	if (!(iter = dev_iter_create(cmd->full_filter, 1))) {
		log_error("dev_iter creation failed.");
		return ECMD_FAILED;
	}

	while ((dev = dev_iter_get(iter)))
	{
		if (!label_read(dev, &label, 0))
			continue;

		ret = process_single_label(cmd, label, handle);

		if (ret > ret_max)
			ret_max = ret;

		if (sigint_caught())
			break;
	}

	dev_iter_destroy(iter);

	return ret_max;
}

/*
 * Parse persistent major minor parameters.
 *
 * --persistent is unspecified => state is deduced
 * from presence of options --minor or --major.
 *
 * -Mn => --minor or --major not allowed.
 *
 * -My => --minor is required (and also --major on <=2.4)
 */
int get_and_validate_major_minor(const struct cmd_context *cmd,
				 const struct format_type *fmt,
				 int32_t *major, int32_t *minor)
{
	if (arg_count(cmd, minor_ARG) > 1) {
		log_error("Option --minor may not be repeated.");
		return 0;
	}

	if (arg_count(cmd, major_ARG) > 1) {
		log_error("Option -j|--major may not be repeated.");
		return 0;
	}

	/* Check with default 'y' */
	if (!arg_int_value(cmd, persistent_ARG, 1)) { /* -Mn */
		if (arg_is_set(cmd, minor_ARG) || arg_is_set(cmd, major_ARG)) {
			log_error("Options --major and --minor are incompatible with -Mn.");
			return 0;
		}
		*major = *minor = -1;
		return 1;
	}

	/* -1 cannot be entered as an argument for --major, --minor */
	*major = arg_int_value(cmd, major_ARG, -1);
	*minor = arg_int_value(cmd, minor_ARG, -1);

	if (arg_is_set(cmd, persistent_ARG)) { /* -My */
		if (*minor == -1) {
			log_error("Please specify minor number with --minor when using -My.");
			return 0;
		}
	}

	if (!strncmp(cmd->kernel_vsn, "2.4.", 4)) {
		/* Major is required for 2.4 */
		if (arg_is_set(cmd, persistent_ARG) && *major < 0) {
			log_error("Please specify major number with --major when using -My.");
			return 0;
		}
	} else {
		if (*major != -1) {
			log_warn("WARNING: Ignoring supplied major number %d - "
				 "kernel assigns major numbers dynamically. "
				 "Using major number %d instead.",
				 *major, cmd->dev_types->device_mapper_major);
		}
		/* Stay with dynamic major:minor if minor is not specified. */
		*major = (*minor == -1) ? -1 : cmd->dev_types->device_mapper_major;
	}

	if ((*minor != -1) && !validate_major_minor(cmd, fmt, *major, *minor))
		return_0;

	return 1;
}

/*
 * Validate lvname parameter
 *
 * If it contains vgname, it is extracted from lvname.
 * If there is passed vgname, it is compared whether its the same name.
 */
int validate_lvname_param(struct cmd_context *cmd, const char **vg_name,
			  const char **lv_name)
{
	const char *vgname;
	const char *lvname;

	if (!lv_name || !*lv_name)
		return 1;  /* NULL lvname is ok */

	/* If contains VG name, extract it. */
	if (strchr(*lv_name, (int) '/')) {
		if (!(vgname = _extract_vgname(cmd, *lv_name, &lvname)))
			return_0;

		if (!*vg_name)
			*vg_name = vgname;
		else if (strcmp(vgname, *vg_name)) {
			log_error("Please use a single volume group name "
				  "(\"%s\" or \"%s\").", vgname, *vg_name);
			return 0;
		}

		*lv_name = lvname;
	}

	if (!validate_name(*lv_name)) {
		log_error("Logical volume name \"%s\" is invalid.",
			  *lv_name);
		return 0;
	}

	return 1;
}

/*
 * Validate lvname parameter
 * This name must follow restriction rules on prefixes and suffixes.
 *
 * If it contains vgname, it is extracted from lvname.
 * If there is passed vgname, it is compared whether its the same name.
 */
int validate_restricted_lvname_param(struct cmd_context *cmd, const char **vg_name,
				     const char **lv_name)
{
	if (!validate_lvname_param(cmd, vg_name, lv_name))
		return_0;

	if (lv_name && *lv_name && !apply_lvname_restrictions(*lv_name))
		return_0;

	return -1;
}

/*
 * Extract list of VG names and list of tags from command line arguments.
 */
static int _get_arg_vgnames(struct cmd_context *cmd,
			    int argc, char **argv,
			    unsigned one_vgname_arg,
			    struct dm_list *arg_vgnames,
			    struct dm_list *arg_tags)
{
	int opt = 0;
	int ret_max = ECMD_PROCESSED;
	const char *vg_name;

	log_verbose("Using volume group(s) on command line.");

	for (; opt < argc; opt++) {
		vg_name = argv[opt];

		if (*vg_name == '@') {
			if (one_vgname_arg) {
				log_error("This command does not yet support a tag to identify a Volume Group.");
				return EINVALID_CMD_LINE;
			}

			if (!validate_tag(vg_name + 1)) {
				log_error("Skipping invalid tag: %s", vg_name);
				if (ret_max < EINVALID_CMD_LINE)
					ret_max = EINVALID_CMD_LINE;
				continue;
			}

			if (!str_list_add(cmd->mem, arg_tags,
					  dm_pool_strdup(cmd->mem, vg_name + 1))) {
				log_error("strlist allocation failed.");
				return ECMD_FAILED;
			}

			continue;
		}

		vg_name = skip_dev_dir(cmd, vg_name, NULL);
		if (strchr(vg_name, '/')) {
			log_error("Invalid volume group name %s.", vg_name);
			if (ret_max < EINVALID_CMD_LINE)
				ret_max = EINVALID_CMD_LINE;
			if (one_vgname_arg)
				break;
			continue;
		}

		if (!str_list_add(cmd->mem, arg_vgnames,
				  dm_pool_strdup(cmd->mem, vg_name))) {
			log_error("strlist allocation failed.");
			return ECMD_FAILED;
		}

		if (one_vgname_arg)
			break;
	}

	return ret_max;
}

struct processing_handle *init_processing_handle(struct cmd_context *cmd)
{
	struct processing_handle *handle;

	if (!(handle = dm_pool_zalloc(cmd->mem, sizeof(struct processing_handle)))) {
		log_error("_init_processing_handle: failed to allocate memory for processing handle");
		return NULL;
	}

	/*
	 * For any reporting tool, the internal_report_for_select is reset to 0
	 * automatically because the internal reporting/selection is simply not
	 * needed - the reporting/selection is already a part of the code path
	 * used there.
	 *
	 * *The internal report for select is only needed for non-reporting tools!*
	 */
	handle->internal_report_for_select = arg_is_set(cmd, select_ARG);

	return handle;
}

int init_selection_handle(struct cmd_context *cmd, struct processing_handle *handle,
			  report_type_t initial_report_type)
{
	struct selection_handle *sh;

	if (!(sh = dm_pool_zalloc(cmd->mem, sizeof(struct selection_handle)))) {
		log_error("_init_selection_handle: failed to allocate memory for selection handle");
		return 0;
	}

	sh->report_type = initial_report_type;
	if (!(sh->selection_rh = report_init_for_selection(cmd, &sh->report_type,
					arg_str_value(cmd, select_ARG, NULL)))) {
		dm_pool_free(cmd->mem, sh);
		return_0;
	}

	handle->selection_handle = sh;
	return 1;
}

void destroy_processing_handle(struct cmd_context *cmd, struct processing_handle *handle)
{
	if (handle) {
		if (handle->selection_handle && handle->selection_handle->selection_rh)
			dm_report_free(handle->selection_handle->selection_rh);
		dm_pool_free(cmd->mem, handle);
	}
}


int select_match_vg(struct cmd_context *cmd, struct processing_handle *handle,
		    struct volume_group *vg, int *selected)
{
	struct selection_handle *sh = handle->selection_handle;

	if (!handle->internal_report_for_select) {
		*selected = 1;
		return 1;
	}

	sh->orig_report_type = VGS;

	if (!report_for_selection(cmd, sh, NULL, vg, NULL)) {
		log_error("Selection failed for VG %s.", vg->name);
		return 0;
	}

	sh->orig_report_type = 0;
	*selected = sh->selected;

	return 1;
}

int select_match_lv(struct cmd_context *cmd, struct processing_handle *handle,
		    struct volume_group *vg, struct logical_volume *lv, int *selected)
{
	struct selection_handle *sh = handle->selection_handle;

	if (!handle->internal_report_for_select) {
		*selected = 1;
		return 1;
	}

	sh->orig_report_type = LVS;

	if (!report_for_selection(cmd, sh, NULL, vg, lv)) {
		log_error("Selection failed for LV %s.", lv->name);
		return 0;
	}

	sh->orig_report_type = 0;
	*selected = sh->selected;

	return 1;
}

int select_match_pv(struct cmd_context *cmd, struct processing_handle *handle,
		    struct volume_group *vg, struct physical_volume *pv, int *selected)
{
	struct selection_handle *sh = handle->selection_handle;

	if (!handle->internal_report_for_select) {
		*selected = 1;
		return 1;
	}

	sh->orig_report_type = PVS;

	if (!report_for_selection(cmd, sh, pv, vg, NULL)) {
		log_error("Selection failed for PV %s.", dev_name(pv->dev));
		return 0;
	}

	sh->orig_report_type = 0;
	*selected = sh->selected;

	return 1;
}

static int _process_vgnameid_list(struct cmd_context *cmd, uint32_t flags,
				  struct dm_list *vgnameids_to_process,
				  struct dm_list *arg_vgnames,
				  struct dm_list *arg_tags,
				  struct processing_handle *handle,
				  process_single_vg_fn_t process_single_vg)
{
	struct volume_group *vg;
	struct vgnameid_list *vgnl;
	const char *vg_name;
	const char *vg_uuid;
	int selected;
	int whole_selected = 0;
	int ret_max = ECMD_PROCESSED;
	int ret;
	int skip;
	int process_all = 0;

	/*
	 * If no VG names or tags were supplied, then process all VGs.
	 */
	if (dm_list_empty(arg_vgnames) && dm_list_empty(arg_tags))
		process_all = 1;

	/*
	 * FIXME If one_vgname_arg, only proceed if exactly one VG matches tags or selection.
	 */
	dm_list_iterate_items(vgnl, vgnameids_to_process) {
		if (sigint_caught())
			return_ECMD_FAILED;

		vg_name = vgnl->vg_name;
		vg_uuid = vgnl->vgid;
		skip = 0;

		vg = vg_read(cmd, vg_name, vg_uuid, flags);
		if (_ignore_vg(vg, vg_name, arg_vgnames, flags & READ_ALLOW_INCONSISTENT, &skip)) {
			stack;
			ret_max = ECMD_FAILED;
			release_vg(vg);
			continue;
		}
		if (skip) {
			release_vg(vg);
			continue;
		}

		/* Process this VG? */
		if ((process_all ||
		    (!dm_list_empty(arg_vgnames) && str_list_match_item(arg_vgnames, vg_name)) ||
		    (!dm_list_empty(arg_tags) && str_list_match_list(arg_tags, &vg->tags, NULL))) &&
		    select_match_vg(cmd, handle, vg, &selected) && selected) {
			ret = process_single_vg(cmd, vg_name, vg, handle);
			_update_selection_result(handle, &whole_selected);
			if (ret != ECMD_PROCESSED)
				stack;
			if (ret > ret_max)
				ret_max = ret;
		}

		if (vg_read_error(vg))
			release_vg(vg);
		else
			unlock_and_release_vg(cmd, vg, vg_name);
	}

	/* the VG is selected if at least one LV is selected */
	_set_final_selection_result(handle, whole_selected);
	return ret_max;
}

/*
 * Copy the contents of a str_list of VG names to a name list, filling
 * in the vgid with NULL (unknown).
 */
static int _copy_str_to_vgnameid_list(struct cmd_context *cmd, struct dm_list *sll,
				      struct dm_list *vgnll)
{
	const char *vgname;
	struct dm_str_list *sl;
	struct vgnameid_list *vgnl;

	dm_list_iterate_items(sl, sll) {
		vgname = sl->str;

		vgnl = dm_pool_alloc(cmd->mem, sizeof(*vgnl));
		if (!vgnl) {
			log_error("vgnameid_list allocation failed.");
			return ECMD_FAILED;
		}

		vgnl->vgid = NULL;
		vgnl->vg_name = dm_pool_strdup(cmd->mem, vgname);

		dm_list_add(vgnll, &vgnl->list);
	}

	return ECMD_PROCESSED;
}

/*
 * Call process_single_vg() for each VG selected by the command line arguments.
 */
int process_each_vg(struct cmd_context *cmd, int argc, char **argv,
		    uint32_t flags, struct processing_handle *handle,
		    process_single_vg_fn_t process_single_vg)
{
	int handle_supplied = handle != NULL;
	struct dm_list arg_tags;		/* str_list */
	struct dm_list arg_vgnames;		/* str_list */
	struct dm_list vgnameids_on_system;	/* vgnameid_list */
	struct dm_list vgnameids_to_process;	/* vgnameid_list */

	int enable_all_vgs = (cmd->command->flags & ALL_VGS_IS_DEFAULT);
	unsigned one_vgname_arg = (flags & ONE_VGNAME_ARG);
	int ret;

	cmd->error_foreign_vgs = 0;

	dm_list_init(&arg_tags);
	dm_list_init(&arg_vgnames);
	dm_list_init(&vgnameids_on_system);
	dm_list_init(&vgnameids_to_process);

	/*
	 * Find any VGs or tags explicitly provided on the command line.
	 */
	if ((ret = _get_arg_vgnames(cmd, argc, argv, one_vgname_arg, &arg_vgnames, &arg_tags)) != ECMD_PROCESSED)
		goto_out;

	/*
	 * Obtain the complete list of VGs present on the system if it is needed because:
	 *   any tags were supplied and need resolving; or
	 *   no VG names were given and the command defaults to processing all VGs.
	 */
	if (((dm_list_empty(&arg_vgnames) && enable_all_vgs) || !dm_list_empty(&arg_tags)) &&
	    !get_vgnameids(cmd, &vgnameids_on_system, NULL, 0))
		goto_out;

	if (dm_list_empty(&arg_vgnames) && dm_list_empty(&vgnameids_on_system)) {
		/* FIXME Should be log_print, but suppressed for reporting cmds */
		log_verbose("No volume groups found.");
		ret = ECMD_PROCESSED;
		goto out;
	}

	/*
	 * If we obtained a full list of VGs on the system, we need to work through them all;
	 * otherwise we can merely work through the VG names provided.
	 */
	if (!dm_list_empty(&vgnameids_on_system))
		dm_list_splice(&vgnameids_to_process, &vgnameids_on_system);
	else if ((ret = _copy_str_to_vgnameid_list(cmd, &arg_vgnames, &vgnameids_to_process)) != ECMD_PROCESSED)
		goto_out;

	if (!handle && !(handle = init_processing_handle(cmd)))
		goto_out;

	if (handle->internal_report_for_select && !handle->selection_handle &&
	    !init_selection_handle(cmd, handle, VGS))
		goto_out;

	ret = _process_vgnameid_list(cmd, flags, &vgnameids_to_process,
				     &arg_vgnames, &arg_tags, handle, process_single_vg);
out:
	if (!handle_supplied)
		destroy_processing_handle(cmd, handle);

	return ret;
}

int process_each_lv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  struct dm_list *arg_lvnames, const struct dm_list *tags_in,
			  int stop_on_error,
			  struct processing_handle *handle,
			  process_single_lv_fn_t process_single_lv)
{
	int ret_max = ECMD_PROCESSED;
	int ret = 0;
	int selected;
	int whole_selected = 0;
	int handle_supplied = handle != NULL;
	unsigned process_lv;
	unsigned process_all = 0;
	unsigned tags_supplied = 0;
	unsigned lvargs_supplied = 0;
	struct lv_list *lvl;
	struct dm_str_list *sl;
	struct dm_list final_lvs;
	struct lv_list *final_lvl;

	dm_list_init(&final_lvs);

	if (!vg_check_status(vg, EXPORTED_VG)) {
		ret_max = ECMD_FAILED;
		goto_out;
	}

	if (tags_in && !dm_list_empty(tags_in))
		tags_supplied = 1;

	if (arg_lvnames && !dm_list_empty(arg_lvnames))
		lvargs_supplied = 1;

	if (!handle && !(handle = init_processing_handle(cmd))) {
		ret_max = ECMD_FAILED;
		goto_out;
	}

	if (handle->internal_report_for_select && !handle->selection_handle &&
	    !init_selection_handle(cmd, handle, LVS)) {
		ret_max = ECMD_FAILED;
		goto_out;
	}

	/* Process all LVs in this VG if no restrictions given 
	 * or if VG tags match. */
	if ((!tags_supplied && !lvargs_supplied) ||
	    (tags_supplied && str_list_match_list(tags_in, &vg->tags, NULL)))
		process_all = 1;

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (sigint_caught()) {
			ret_max = ECMD_FAILED;
			goto_out;
		}

		if (lvl->lv->status & SNAPSHOT)
			continue;

		/* Skip availability change for non-virt snaps when processing all LVs */
		/* FIXME: pass process_all to process_single_lv() */
		if (process_all && arg_count(cmd, activate_ARG) &&
		    lv_is_cow(lvl->lv) && !lv_is_virtual_origin(origin_from_cow(lvl->lv)))
			continue;

		if (lv_is_virtual_origin(lvl->lv) && !arg_count(cmd, all_ARG)) {
			if (lvargs_supplied &&
			    str_list_match_item(arg_lvnames, lvl->lv->name))
				log_print_unless_silent("Ignoring virtual origin logical volume %s.",
							display_lvname(lvl->lv));
			continue;
		}

		/*
		 * Only let hidden LVs through it --all was used or the LVs 
		 * were specifically named on the command line.
		 */
		if (!lvargs_supplied && !lv_is_visible(lvl->lv) && !arg_count(cmd, all_ARG))
			continue;

		/*
		 * process the LV if one of the following:
		 * - process_all is set
		 * - LV name matches a supplied LV name
		 * - LV tag matches a supplied LV tag
		 * - LV matches the selection
		 */

		process_lv = process_all;

		if (lvargs_supplied && str_list_match_item(arg_lvnames, lvl->lv->name)) {
			/* Remove LV from list of unprocessed LV names */
			str_list_del(arg_lvnames, lvl->lv->name);
			process_lv = 1;
		}

		if (!process_lv && tags_supplied && str_list_match_list(tags_in, &lvl->lv->tags, NULL))
			process_lv = 1;

		process_lv = process_lv && select_match_lv(cmd, handle, vg, lvl->lv, &selected) && selected;

		if (sigint_caught()) {
			ret_max = ECMD_FAILED;
			goto_out;
		}

		if (!process_lv)
			continue;

		log_very_verbose("Adding %s/%s to the list of LVs to be processed.", vg->name, lvl->lv->name);

		if (!(final_lvl = dm_pool_zalloc(vg->vgmem, sizeof(struct lv_list)))) {
			log_error("Failed to allocate final LV list item.");
			ret_max = ECMD_FAILED;
			goto_out;
		}
		final_lvl->lv = lvl->lv;
		dm_list_add(&final_lvs, &final_lvl->list);
	}

	dm_list_iterate_items(lvl, &final_lvs) {
		/*
		 *  FIXME: Once we have index over vg->removed_lvs, check directly
		 *         LV presence there and remove LV_REMOVE flag/lv_is_removed fn
		 *         as they won't be needed anymore.
		 */
		if (lv_is_removed(lvl->lv))
			continue;

		log_very_verbose("Processing LV %s in VG %s.", lvl->lv->name, vg->name);

		ret = process_single_lv(cmd, lvl->lv, handle);
		if (handle_supplied)
			_update_selection_result(handle, &whole_selected);
		if (ret != ECMD_PROCESSED)
			stack;
		if (ret > ret_max)
			ret_max = ret;

		if (stop_on_error && ret != ECMD_PROCESSED)
			goto_out;
	}

	if (lvargs_supplied) {
		/*
		 * FIXME: lvm supports removal of LV with all its dependencies
		 * this leads to miscalculation that depends on the order of args.
		 */
		dm_list_iterate_items(sl, arg_lvnames) {
			log_error("Failed to find logical volume \"%s/%s\"",
				  vg->name, sl->str);
			if (ret_max < ECMD_FAILED)
				ret_max = ECMD_FAILED;
		}
	}
out:
	if (!handle_supplied)
		destroy_processing_handle(cmd, handle);
	else
		_set_final_selection_result(handle, whole_selected);
	return ret_max;
}

/*
 * If arg is tag, add it to arg_tags
 * else the arg is either vgname or vgname/lvname:
 * - add the vgname of each arg to arg_vgnames
 * - if arg has no lvname, add just vgname arg_lvnames,
 *   it represents all lvs in the vg
 * - if arg has lvname, add vgname/lvname to arg_lvnames
 */
static int _get_arg_lvnames(struct cmd_context *cmd,
			    int argc, char **argv,
			    struct dm_list *arg_vgnames,
			    struct dm_list *arg_lvnames,
			    struct dm_list *arg_tags)
{
	int opt = 0;
	int ret_max = ECMD_PROCESSED;
	char *vglv;
	size_t vglv_sz;
	const char *vgname;
	const char *lv_name;
	const char *tmp_lv_name;
	const char *vgname_def;
	unsigned dev_dir_found;

	log_verbose("Using logical volume(s) on command line.");

	for (; opt < argc; opt++) {
		lv_name = argv[opt];
		dev_dir_found = 0;

		/* Do we have a tag or vgname or lvname? */
		vgname = lv_name;

		if (*vgname == '@') {
			if (!validate_tag(vgname + 1)) {
				log_error("Skipping invalid tag %s.", vgname);
				continue;
			}
			if (!str_list_add(cmd->mem, arg_tags,
					  dm_pool_strdup(cmd->mem, vgname + 1))) {
				log_error("strlist allocation failed.");
				return ECMD_FAILED;
			}
			continue;
		}

		/* FIXME Jumbled parsing */
		vgname = skip_dev_dir(cmd, vgname, &dev_dir_found);

		if (*vgname == '/') {
			log_error("\"%s\": Invalid path for Logical Volume.",
				  argv[opt]);
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
			   (vgname_def = _default_vgname(cmd)))
			vgname = vgname_def;
		else
			lv_name = NULL;

		if (!str_list_add(cmd->mem, arg_vgnames,
				  dm_pool_strdup(cmd->mem, vgname))) {
			log_error("strlist allocation failed.");
			return ECMD_FAILED;
		}

		if (!lv_name) {
			if (!str_list_add(cmd->mem, arg_lvnames,
					  dm_pool_strdup(cmd->mem, vgname))) {
				log_error("strlist allocation failed.");
				return ECMD_FAILED;
			}
		} else {
			vglv_sz = strlen(vgname) + strlen(lv_name) + 2;
			if (!(vglv = dm_pool_alloc(cmd->mem, vglv_sz)) ||
			    dm_snprintf(vglv, vglv_sz, "%s/%s", vgname, lv_name) < 0) {
				log_error("vg/lv string alloc failed.");
				return ECMD_FAILED;
			}
			if (!str_list_add(cmd->mem, arg_lvnames, vglv)) {
				log_error("strlist allocation failed.");
				return ECMD_FAILED;
			}
		}
	}

	return ret_max;
}

static int _process_lv_vgnameid_list(struct cmd_context *cmd, uint32_t flags,
				     struct dm_list *vgnameids_to_process,
				     struct dm_list *arg_vgnames,
				     struct dm_list *arg_lvnames,
				     struct dm_list *arg_tags,
				     struct processing_handle *handle,
				     process_single_lv_fn_t process_single_lv)
{
	struct volume_group *vg;
	struct vgnameid_list *vgnl;
	struct dm_str_list *sl;
	struct dm_list *tags_arg;
	struct dm_list lvnames;
	const char *vg_name;
	const char *vg_uuid;
	const char *vgn;
	const char *lvn;
	int ret_max = ECMD_PROCESSED;
	int ret;
	int skip;

	dm_list_iterate_items(vgnl, vgnameids_to_process) {
		if (sigint_caught())
			return_ECMD_FAILED;

		vg_name = vgnl->vg_name;
		vg_uuid = vgnl->vgid;
		skip = 0;

		/*
		 * arg_lvnames contains some elements that are just "vgname"
		 * which means process all lvs in the vg.  Other elements
		 * are "vgname/lvname" which means process only the select
		 * lvs in the vg.
		 */
		tags_arg = arg_tags;
		dm_list_init(&lvnames);	/* LVs to be processed in this VG */

		dm_list_iterate_items(sl, arg_lvnames) {
			vgn = sl->str;
			lvn = strchr(vgn, '/');

			if (!lvn && !strcmp(vgn, vg_name)) {
				/* Process all LVs in this VG */
				tags_arg = NULL;
				dm_list_init(&lvnames);
				break;
			}
			
			if (lvn && !strncmp(vgn, vg_name, strlen(vg_name)) &&
			    strlen(vg_name) == (size_t) (lvn - vgn)) {
				if (!str_list_add(cmd->mem, &lvnames,
						  dm_pool_strdup(cmd->mem, lvn + 1))) {
					log_error("strlist allocation failed.");
					return ECMD_FAILED;
				}
			}
		}

		vg = vg_read(cmd, vg_name, vg_uuid, flags);
		if (_ignore_vg(vg, vg_name, arg_vgnames, flags & READ_ALLOW_INCONSISTENT, &skip)) {
			stack;
			ret_max = ECMD_FAILED;
			release_vg(vg);
			continue;

		}
		if (skip) {
			release_vg(vg);
			continue;
		}

		ret = process_each_lv_in_vg(cmd, vg, &lvnames, tags_arg, 0,
					    handle, process_single_lv);
		if (ret != ECMD_PROCESSED)
			stack;
		if (ret > ret_max)
			ret_max = ret;

		unlock_and_release_vg(cmd, vg, vg_name);
	}

	return ret_max;
}

/*
 * Call process_single_lv() for each LV selected by the command line arguments.
 */
int process_each_lv(struct cmd_context *cmd, int argc, char **argv, uint32_t flags,
		    struct processing_handle *handle, process_single_lv_fn_t process_single_lv)
{
	int handle_supplied = handle != NULL;
	struct dm_list arg_tags;		/* str_list */
	struct dm_list arg_vgnames;		/* str_list */
	struct dm_list arg_lvnames;		/* str_list */
	struct dm_list vgnameids_on_system;	/* vgnameid_list */
	struct dm_list vgnameids_to_process;	/* vgnameid_list */

	int enable_all_vgs = (cmd->command->flags & ALL_VGS_IS_DEFAULT);
	int need_vgnameids = 0;
	int ret;

	cmd->error_foreign_vgs = 0;

	dm_list_init(&arg_tags);
	dm_list_init(&arg_vgnames);
	dm_list_init(&arg_lvnames);
	dm_list_init(&vgnameids_on_system);
	dm_list_init(&vgnameids_to_process);

	/*
	 * Find any LVs, VGs or tags explicitly provided on the command line.
	 */
	if ((ret = _get_arg_lvnames(cmd, argc, argv, &arg_vgnames, &arg_lvnames, &arg_tags) != ECMD_PROCESSED))
		goto_out;

	if (!handle && !(handle = init_processing_handle(cmd)))
		goto_out;

	if (handle->internal_report_for_select && !handle->selection_handle &&
	    !init_selection_handle(cmd, handle, LVS))
		goto_out;

	/*
	 * Obtain the complete list of VGs present on the system if it is needed because:
	 *   any tags were supplied and need resolving; or
	 *   no VG names were given and the select option needs resolving; or
	 *   no VG names were given and the command defaults to processing all VGs.
	*/
	if (!dm_list_empty(&arg_tags))
		need_vgnameids = 1;
	else if (dm_list_empty(&arg_vgnames) && enable_all_vgs)
		need_vgnameids = 1;
	else if (dm_list_empty(&arg_vgnames) && handle->internal_report_for_select)
		need_vgnameids = 1;

	if (need_vgnameids && !get_vgnameids(cmd, &vgnameids_on_system, NULL, 0))
		goto_out;

	if (dm_list_empty(&arg_vgnames) && dm_list_empty(&vgnameids_on_system)) {
		/* FIXME Should be log_print, but suppressed for reporting cmds */
		log_verbose("No volume groups found.");
		ret = ECMD_PROCESSED;
		goto out;
	}

	/*
	 * If we obtained a full list of VGs on the system, we need to work through them all;
	 * otherwise we can merely work through the VG names provided.
	 */
	if (!dm_list_empty(&vgnameids_on_system))
		dm_list_splice(&vgnameids_to_process, &vgnameids_on_system);
	else if ((ret = _copy_str_to_vgnameid_list(cmd, &arg_vgnames, &vgnameids_to_process)) != ECMD_PROCESSED)
		goto_out;

	ret = _process_lv_vgnameid_list(cmd, flags, &vgnameids_to_process, &arg_vgnames, &arg_lvnames,
					&arg_tags, handle, process_single_lv);
out:
	if (!handle_supplied)
		destroy_processing_handle(cmd, handle);
	return ret;
}

static int _get_arg_pvnames(struct cmd_context *cmd,
			    int argc, char **argv,
			    struct dm_list *arg_pvnames,
			    struct dm_list *arg_tags)
{
	int opt = 0;
	char *at_sign, *tagname;
	char *arg_name;
	int ret_max = ECMD_PROCESSED;

	log_verbose("Using physical volume(s) on command line.");

	for (; opt < argc; opt++) {
		arg_name = argv[opt];

		dm_unescape_colons_and_at_signs(arg_name, NULL, &at_sign);
		if (at_sign && (at_sign == arg_name)) {
			tagname = at_sign + 1;

			if (!validate_tag(tagname)) {
				log_error("Skipping invalid tag %s.", tagname);
				if (ret_max < EINVALID_CMD_LINE)
					ret_max = EINVALID_CMD_LINE;
				continue;
			}
			if (!str_list_add(cmd->mem, arg_tags,
					  dm_pool_strdup(cmd->mem, tagname))) {
				log_error("strlist allocation failed.");
				return ECMD_FAILED;
			}
			continue;
		}

		if (!str_list_add(cmd->mem, arg_pvnames,
				  dm_pool_strdup(cmd->mem, arg_name))) {
			log_error("strlist allocation failed.");
			return ECMD_FAILED;
		}
	}

	return ret_max;
}

static int _get_arg_devices(struct cmd_context *cmd,
			    struct dm_list *arg_pvnames,
			    struct dm_list *arg_devices)
{
	struct dm_str_list *sl;
	struct device_id_list *dil;
	int ret_max = ECMD_PROCESSED;

	dm_list_iterate_items(sl, arg_pvnames) {
		if (!(dil = dm_pool_alloc(cmd->mem, sizeof(*dil)))) {
			log_error("device_id_list alloc failed.");
			return ECMD_FAILED;
		}

		if (!(dil->dev = dev_cache_get(sl->str, cmd->filter))) {
			log_error("Failed to find device for physical volume \"%s\".", sl->str);
			ret_max = ECMD_FAILED;
		} else {
			strncpy(dil->pvid, dil->dev->pvid, ID_LEN);
			dm_list_add(arg_devices, &dil->list);
		}
	}

	return ret_max;
}

static int _get_all_devices(struct cmd_context *cmd, struct dm_list *all_devices)
{
	struct dev_iter *iter;
	struct device *dev;
	struct device_id_list *dil;
	int r = ECMD_FAILED;

	lvmcache_seed_infos_from_lvmetad(cmd);

	if (!(iter = dev_iter_create(cmd->full_filter, 1))) {
		log_error("dev_iter creation failed.");
		return ECMD_FAILED;
	}

	while ((dev = dev_iter_get(iter))) {
		if (!(dil = dm_pool_alloc(cmd->mem, sizeof(*dil)))) {
			log_error("device_id_list alloc failed.");
			goto out;
		}

		strncpy(dil->pvid, dev->pvid, ID_LEN);
		dil->dev = dev;
		dm_list_add(all_devices, &dil->list);
	}

	r = ECMD_PROCESSED;
out:
	dev_iter_destroy(iter);
	return r;
}

static int _device_list_remove(struct dm_list *devices, struct device *dev)
{
	struct device_id_list *dil;

	dm_list_iterate_items(dil, devices) {
		if (dil->dev == dev) {
			dm_list_del(&dil->list);
			return 1;
		}
	}

	return 0;
}

static struct device_id_list *_device_list_find_dev(struct dm_list *devices, struct device *dev)
{
	struct device_id_list *dil;

	dm_list_iterate_items(dil, devices) {
		if (dil->dev == dev)
			return dil;
	}

	return NULL;
}

static struct device_id_list *_device_list_find_pvid(struct dm_list *devices, struct physical_volume *pv)
{
	struct device_id_list *dil;

	dm_list_iterate_items(dil, devices) {
		if (id_equal((struct id *) dil->pvid, &pv->id))
			return dil;
	}

	return NULL;
}

static int _process_device_list(struct cmd_context *cmd, struct dm_list *all_devices,
				struct processing_handle *handle,
				process_single_pv_fn_t process_single_pv)
{
	struct physical_volume pv_dummy;
	struct physical_volume *pv;
	struct device_id_list *dil;
	int ret_max = ECMD_PROCESSED;
	int ret = 0;

	/*
	 * Pretend that each device is a PV with dummy values.
	 * FIXME Formalise this extension or find an alternative.
	 */
	dm_list_iterate_items(dil, all_devices) {
		if (sigint_caught())
			return_ECMD_FAILED;

		memset(&pv_dummy, 0, sizeof(pv_dummy));
		dm_list_init(&pv_dummy.tags);
		dm_list_init(&pv_dummy.segments);
		pv_dummy.dev = dil->dev;
		pv = &pv_dummy;

		log_very_verbose("Processing device %s.", dev_name(dil->dev));

		ret = process_single_pv(cmd, NULL, pv, handle);

		if (ret > ret_max)
			ret_max = ret;
	}

	return ECMD_PROCESSED;
}

static int _process_pvs_in_vg(struct cmd_context *cmd,
			      struct volume_group *vg,
			      struct dm_list *all_devices,
			      struct dm_list *arg_devices,
			      struct dm_list *arg_tags,
			      int process_all_pvs,
			      int process_all_devices,
			      int skip,
			      struct processing_handle *handle,
			      process_single_pv_fn_t process_single_pv)
{
	int handle_supplied = handle != NULL;
	struct physical_volume *pv;
	struct pv_list *pvl;
	struct device_id_list *dil;
	struct device *dev_orig;
	const char *pv_name;
	int selected;
	int process_pv;
	int dev_found;
	int ret_max = ECMD_PROCESSED;
	int ret = 0;

	if (!handle && (!(handle = init_processing_handle(cmd)))) {
		ret_max = ECMD_FAILED;
		goto_out;
	}

	if (handle->internal_report_for_select && !handle->selection_handle &&
	    !init_selection_handle(cmd, handle, PVS)) {
		ret_max = ECMD_FAILED;
		goto_out;
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (sigint_caught()) {
			ret_max = ECMD_FAILED;
			goto_out;
		}

		pv = pvl->pv;
		pv_name = pv_dev_name(pv);

		process_pv = process_all_pvs;

		/* Remove each arg_devices entry as it is processed. */

		if (!process_pv && !dm_list_empty(arg_devices) &&
		    (dil = _device_list_find_dev(arg_devices, pv->dev))) {
			process_pv = 1;
			_device_list_remove(arg_devices, dil->dev);
		}

		/* Select the PV if the device arg has the same pvid. */

		if (!process_pv && !dm_list_empty(arg_devices) &&
		    (dil = _device_list_find_pvid(arg_devices, pv))) {
			process_pv = 1;
			_device_list_remove(arg_devices, dil->dev);
		}

		if (!process_pv && !dm_list_empty(arg_tags) &&
		    str_list_match_list(arg_tags, &pv->tags, NULL))
			process_pv = 1;

		process_pv = process_pv && select_match_pv(cmd, handle, vg, pv, &selected) && selected;

		if (process_pv) {
			if (skip)
				log_verbose("Skipping PV %s in VG %s.", pv_name, vg->name);
			else
				log_very_verbose("Processing PV %s in VG %s.", pv_name, vg->name);

			dev_found = _device_list_remove(all_devices, pv->dev);

			/*
			 * FIXME PVs with no mdas may turn up in an orphan VG when
			 * not using lvmetad as well as their correct VG.  They
			 * will be missing from all_devices the second time
			 * around but must not be processed twice or trigger a message.
			 *
			 * Missing PVs will also need processing even though they are
			 * not present in all_devices.
			 */
			if (!dev_found && !is_missing_pv(pv)) {
				log_verbose("Skipping PV %s in VG %s: not in device list.", pv_name, vg->name);
				continue;
			}

			if (!skip) {
				ret = process_single_pv(cmd, vg, pv, handle);
				if (ret != ECMD_PROCESSED)
					stack;
				if (ret > ret_max)
					ret_max = ret;
			}

			/*
			 * This is a very rare and obscure case where multiple
			 * duplicate devices are specified on the command line
			 * referring to this PV.  In this case we want to
			 * process this PV once for each specified device.
			 */

			if (!skip && !dm_list_empty(arg_devices)) {
				while ((dil = _device_list_find_pvid(arg_devices, pv))) {
					_device_list_remove(arg_devices, dil->dev);

					/*
					 * Replace pv->dev with this dil->dev
					 * in lvmcache so the duplicate dev
					 * info will be reported.  FIXME: it
					 * would be nicer to override pv->dev
					 * without munging lvmcache content.
					 */
					dev_orig = pv->dev;
					lvmcache_replace_dev(cmd, pv, dil->dev);

					log_very_verbose("Processing PV %s device %s in VG %s.",
							 pv_name, dev_name(dil->dev), vg->name);

					ret = process_single_pv(cmd, vg, pv, handle);
					if (ret != ECMD_PROCESSED)
						stack;
					if (ret > ret_max)
						ret_max = ret;

					/* Put the cache state back as it was. */
					lvmcache_replace_dev(cmd, pv, dev_orig);
				}
			}

			/*
			 * This is another rare and obscure case where multiple
			 * duplicate devices are being displayed by pvs -a, and
			 * we want each of them to be displayed in the context
			 * of this VG, so that this VG name appears next to it.
			 */

			if (process_all_devices && lvmcache_found_duplicate_pvs()) {
				while ((dil = _device_list_find_pvid(all_devices, pv))) {
					_device_list_remove(all_devices, dil->dev);

					dev_orig = pv->dev;
					lvmcache_replace_dev(cmd, pv, dil->dev);

					ret = process_single_pv(cmd, vg, pv, handle);
					if (ret != ECMD_PROCESSED)
						stack;
					if (ret > ret_max)
						ret_max = ret;

					lvmcache_replace_dev(cmd, pv, dev_orig);
				}
			}
		}

		/*
		 * When processing only specific PVs, we can quit once they've all been found.
	 	 */
		if (!process_all_pvs && dm_list_empty(arg_tags) && dm_list_empty(arg_devices))
			break;
	}
out:
	if (!handle_supplied)
		destroy_processing_handle(cmd, handle);
	return ret_max;
}

/*
 * Iterate through all PVs in each listed VG.  Process a PV if
 * its dev or tag matches arg_devices or arg_tags.  If both
 * arg_devices and arg_tags are empty, then process all PVs.
 * No PV should be processed more than once.
 *
 * Each PV is removed from arg_devices and all_devices when it is
 * processed.  Any names remaining in arg_devices were not found, and
 * should produce an error.  Any devices remaining in all_devices were
 * not found and should be processed by process_device_list().
 */
static int _process_pvs_in_vgs(struct cmd_context *cmd, uint32_t flags,
			       struct dm_list *all_vgnameids,
			       struct dm_list *all_devices,
			       struct dm_list *arg_devices,
			       struct dm_list *arg_tags,
			       int process_all_pvs,
			       int process_all_devices,
			       struct processing_handle *handle,
			       process_single_pv_fn_t process_single_pv)
{
	struct volume_group *vg;
	struct vgnameid_list *vgnl;
	const char *vg_name;
	const char *vg_uuid;
	int ret_max = ECMD_PROCESSED;
	int ret;
	int skip;

	dm_list_iterate_items(vgnl, all_vgnameids) {
		if (sigint_caught())
			return_ECMD_FAILED;

		vg_name = vgnl->vg_name;
		vg_uuid = vgnl->vgid;
		skip = 0;

		vg = vg_read(cmd, vg_name, vg_uuid, flags | READ_WARN_INCONSISTENT);
		if (_ignore_vg(vg, vg_name, NULL, flags & READ_ALLOW_INCONSISTENT, &skip)) {
			stack;
			ret_max = ECMD_FAILED;
			if (!skip) {
				release_vg(vg);
				continue;
			}
			/* Drop through to eliminate a clustered VG's PVs from the devices list */
		}
		
		/*
		 * Don't continue when skip is set, because we need to remove
		 * vg->pvs entries from devices list.
		 */
		
		ret = _process_pvs_in_vg(cmd, vg, all_devices, arg_devices, arg_tags,
					 process_all_pvs, process_all_devices, skip,
					 handle, process_single_pv);
		if (ret != ECMD_PROCESSED)
			stack;
		if (ret > ret_max)
			ret_max = ret;

		if (skip)
			release_vg(vg);
		else
			unlock_and_release_vg(cmd, vg, vg->name);

		/* Quit early when possible. */
		if (!process_all_pvs && dm_list_empty(arg_tags) && dm_list_empty(arg_devices))
			return ret_max;
	}

	return ret_max;
}

int process_each_pv(struct cmd_context *cmd,
		    int argc, char **argv,
		    const char *only_this_vgname,
		    uint32_t flags,
		    struct processing_handle *handle,
		    process_single_pv_fn_t process_single_pv)
{
	struct dm_list arg_tags;	/* str_list */
	struct dm_list arg_pvnames;	/* str_list */
	struct dm_list arg_devices;	/* device_id_list */
	struct dm_list all_vgnameids;	/* vgnameid_list */
	struct dm_list all_devices;	/* device_id_list */
	struct device_id_list *dil;
	int process_all_pvs;
	int process_all_devices;
	int ret_max = ECMD_PROCESSED;
	int ret;

	cmd->error_foreign_vgs = 0;

	dm_list_init(&arg_tags);
	dm_list_init(&arg_pvnames);
	dm_list_init(&arg_devices);
	dm_list_init(&all_vgnameids);
	dm_list_init(&all_devices);

	/*
	 * Create two lists from argv:
	 * arg_pvnames: pvs explicitly named in argv
	 * arg_tags: tags explicitly named in argv
	 *
	 * Then convert arg_pvnames, which are free-form, user-specified,
	 * names/paths into arg_devices which can be used to match below.
	 */
	if ((ret = _get_arg_pvnames(cmd, argc, argv, &arg_pvnames, &arg_tags)) != ECMD_PROCESSED) {
		stack;
		return ret;
	}

	process_all_pvs = dm_list_empty(&arg_pvnames) && dm_list_empty(&arg_tags);

	process_all_devices = process_all_pvs && (cmd->command->flags & ENABLE_ALL_DEVS) &&
			      arg_count(cmd, all_ARG);

	/*
	 * Need pvid's set on all PVs before processing so that pvid's
	 * can be compared to find duplicates while processing.
	 */
	lvmcache_seed_infos_from_lvmetad(cmd);

	if (!get_vgnameids(cmd, &all_vgnameids, only_this_vgname, 1)) {
		stack;
		return ret;
	}

	/*
	 * If the caller wants to process all devices (not just PVs), then all PVs
	 * from all VGs are processed first, removing them from all_devices.  Then
	 * any devs remaining in all_devices are processed.
	 */
	if ((ret = _get_all_devices(cmd, &all_devices) != ECMD_PROCESSED)) {
		stack;
		return ret;
	}

	if ((ret = _get_arg_devices(cmd, &arg_pvnames, &arg_devices) != ECMD_PROCESSED))
		/* get_arg_devices reports the error for any PV names not found. */
		ret_max = ECMD_FAILED;

	ret = _process_pvs_in_vgs(cmd, flags, &all_vgnameids, &all_devices,
				  &arg_devices, &arg_tags,
				  process_all_pvs, process_all_devices,
				  handle, process_single_pv);
	if (ret != ECMD_PROCESSED)
		stack;
	if (ret > ret_max)
		ret_max = ret;

	dm_list_iterate_items(dil, &arg_devices) {
		log_error("Failed to find physical volume \"%s\".", dev_name(dil->dev));
		ret_max = ECMD_FAILED;
	}

	if (!process_all_devices)
		goto out;

	ret = _process_device_list(cmd, &all_devices, handle, process_single_pv);
	if (ret != ECMD_PROCESSED)
		stack;
	if (ret > ret_max)
		ret_max = ret;
out:
	return ret_max;
}

int process_each_pv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  struct processing_handle *handle,
			  process_single_pv_fn_t process_single_pv)
{
	int whole_selected = 0;
	int ret_max = ECMD_PROCESSED;
	int ret;
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (sigint_caught())
			return_ECMD_FAILED;

		ret = process_single_pv(cmd, vg, pvl->pv, handle);
		_update_selection_result(handle, &whole_selected);
		if (ret != ECMD_PROCESSED)
			stack;
		if (ret > ret_max)
			ret_max = ret;
	}

	_set_final_selection_result(handle, whole_selected);
	return ret_max;
}

int lvremove_single(struct cmd_context *cmd, struct logical_volume *lv,
		    struct processing_handle *handle __attribute__((unused)))
{
	/*
	 * Single force is equivalent to single --yes
	 * Even multiple --yes are equivalent to single --force
	 * When we require -ff it cannot be replaced with -f -y
	 */
	force_t force = (force_t) arg_count(cmd, force_ARG)
		? : (arg_is_set(cmd, yes_ARG) ? DONT_PROMPT : PROMPT);

	if (!lv_remove_with_dependencies(cmd, lv, force, 0))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}
