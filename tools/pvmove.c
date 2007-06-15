/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved. 
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
#include "polldaemon.h"
#include "display.h"

/* Allow /dev/vgname/lvname, vgname/lvname or lvname */
static const char *_extract_lvname(struct cmd_context *cmd, const char *vgname,
				   const char *arg)
{
	const char *lvname;

	/* Is an lvname supplied directly? */
	if (!strchr(arg, '/'))
		return arg;

	lvname = skip_dev_dir(cmd, arg, NULL);
	while (*lvname == '/')
		lvname++;
	if (!strchr(lvname, '/')) {
		log_error("--name takes a logical volume name");
		return NULL;
	}
	if (strncmp(vgname, lvname, strlen(vgname)) ||
	    (lvname += strlen(vgname), *lvname != '/')) {
		log_error("Named LV and old PV must be in the same VG");
		return NULL;
	}
	while (*lvname == '/')
		lvname++;
	if (!*lvname) {
		log_error("Incomplete LV name supplied with --name");
		return NULL;
	}
	return lvname;
}

static struct volume_group *_get_vg(struct cmd_context *cmd, const char *vgname)
{
	int consistent = 1;
	struct volume_group *vg;

	dev_close_all();

	if (!lock_vol(cmd, vgname, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vgname);
		return NULL;
	}

	if (!(vg = vg_read(cmd, vgname, NULL, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" doesn't exist", vgname);
		unlock_vg(cmd, vgname);
		return NULL;
	}

	if (!vg_check_status(vg, CLUSTERED | EXPORTED_VG | LVM_WRITE)) {
		unlock_vg(cmd, vgname);
		return NULL;
	}

	return vg;
}

/* Create list of PVs for allocation of replacement extents */
static struct list *_get_allocatable_pvs(struct cmd_context *cmd, int argc,
					 char **argv, struct volume_group *vg,
					 struct physical_volume *pv,
					 alloc_policy_t alloc)
{
	struct list *allocatable_pvs, *pvht, *pvh;
	struct pv_list *pvl;

	if (argc) {
		if (!(allocatable_pvs = create_pv_list(cmd->mem, vg, argc,
						       argv, 1))) {
			stack;
			return NULL;
		}
	} else {
		if (!(allocatable_pvs = clone_pv_list(cmd->mem, &vg->pvs))) {
			stack;
			return NULL;
		}
	}

	list_iterate_safe(pvh, pvht, allocatable_pvs) {
		pvl = list_item(pvh, struct pv_list);

		/* Don't allocate onto the PV we're clearing! */
		if ((alloc != ALLOC_ANYWHERE) && (pvl->pv->dev == pv_dev(pv))) {
			list_del(&pvl->list);
			continue;
		}

		/* Remove PV if full */
		if ((pvl->pv->pe_count == pvl->pv->pe_alloc_count))
			list_del(&pvl->list);
	}

	if (list_empty(allocatable_pvs)) {
		log_error("No extents available for allocation");
		return NULL;
	}

	return allocatable_pvs;
}

/* Create new LV with mirror segments for the required copies */
static struct logical_volume *_set_up_pvmove_lv(struct cmd_context *cmd,
						struct volume_group *vg,
						struct list *source_pvl,
						const char *lv_name,
						struct list *allocatable_pvs,
						alloc_policy_t alloc,
						struct list **lvs_changed)
{
	struct logical_volume *lv_mirr, *lv;
	struct lv_list *lvl;

	/* FIXME Cope with non-contiguous => splitting existing segments */
	if (!(lv_mirr = lv_create_empty(vg->fid, "pvmove%d", NULL,
					LVM_READ | LVM_WRITE,
					ALLOC_CONTIGUOUS, 0, vg))) {
		log_error("Creation of temporary pvmove LV failed");
		return NULL;
	}

	lv_mirr->status |= (PVMOVE | LOCKED);

	if (!(*lvs_changed = dm_pool_alloc(cmd->mem, sizeof(**lvs_changed)))) {
		log_error("lvs_changed list struct allocation failed");
		return NULL;
	}

	list_init(*lvs_changed);

	/* Find segments to be moved and set up mirrors */
	list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if ((lv == lv_mirr) ||
		    (lv_name && strcmp(lv->name, lv_name)))
			continue;
		if (lv_is_origin(lv) || lv_is_cow(lv)) {
			log_print("Skipping snapshot-related LV %s", lv->name);
			continue;
		}
		if (lv->status & MIRRORED) {
			log_print("Skipping mirror LV %s", lv->name);
			continue;
		}
		if (lv->status & MIRROR_LOG) {
			log_print("Skipping mirror log LV %s", lv->name);
			continue;
		}
		if (lv->status & MIRROR_IMAGE) {
			log_print("Skipping mirror image LV %s", lv->name);
			continue;
		}
		if (lv->status & LOCKED) {
			log_print("Skipping locked LV %s", lv->name);
			continue;
		}
		if (!insert_pvmove_mirrors(cmd, lv_mirr, source_pvl, lv,
					   allocatable_pvs, alloc,
					   *lvs_changed)) {
			stack;
			return NULL;
		}
	}

	/* Is temporary mirror empty? */
	if (!lv_mirr->le_count) {
		log_error("No data to move for %s", vg->name);
		return NULL;
	}

	return lv_mirr;
}

static int _update_metadata(struct cmd_context *cmd, struct volume_group *vg,
			    struct logical_volume *lv_mirr,
			    struct list *lvs_changed, int first_time)
{
	log_verbose("Updating volume group metadata");
	if (!vg_write(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		return 0;
	}

	backup(vg);

	/* Suspend lvs_changed */
	if (!suspend_lvs(cmd, lvs_changed)) {
		stack;
		return 0;
	}

	/* Suspend mirrors on subsequent calls */
	if (!first_time) {
		if (!suspend_lv(cmd, lv_mirr)) {
			stack;
			resume_lvs(cmd, lvs_changed);
			vg_revert(vg);
			return 0;
		}
	}

	/* Commit on-disk metadata */
	if (!vg_commit(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		if (!first_time)
			resume_lv(cmd, lv_mirr);
		resume_lvs(cmd, lvs_changed);
		return 0;
	}

	/* Activate the temporary mirror LV */
	/* Only the first mirror segment gets activated as a mirror */
	/* FIXME: Add option to use a log */
	if (first_time) {
		if (!activate_lv_excl(cmd, lv_mirr)) {
			if (!test_mode())
				log_error("ABORTING: Temporary mirror "
					  "activation failed.  "
					  "Run pvmove --abort.");
			/* FIXME Resume using *original* metadata here! */
			resume_lvs(cmd, lvs_changed);
			return 0;
		}
	} else if (!resume_lv(cmd, lv_mirr)) {
		log_error("Unable to reactivate logical volume \"%s\"",
			  lv_mirr->name);
		resume_lvs(cmd, lvs_changed);
		return 0;
	}

	/* Unsuspend LVs */
	if (!resume_lvs(cmd, lvs_changed)) {
		log_error("Unable to resume logical volumes");
		return 0;
	}

	return 1;
}

static int _set_up_pvmove(struct cmd_context *cmd, const char *pv_name,
			  int argc, char **argv)
{
	const char *lv_name = NULL;
	char *pv_name_arg;
	struct volume_group *vg;
	struct list *source_pvl;
	struct list *allocatable_pvs;
	alloc_policy_t alloc;
	struct list *lvs_changed;
	struct physical_volume *pv;
	struct logical_volume *lv_mirr;
	int first_time = 1;

	pv_name_arg = argv[0];
	argc--;
	argv++;

	/* Find PV (in VG) */
	if (!(pv = find_pv_by_name(cmd, pv_name))) {
		stack;
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, name_ARG)) {
		if (!(lv_name = _extract_lvname(cmd, pv_vg_name(pv),
						arg_value(cmd, name_ARG)))) {
			stack;
			return EINVALID_CMD_LINE;
		}
	}

	/* Read VG */
	log_verbose("Finding volume group \"%s\"", pv_vg_name(pv));

	if (!(vg = _get_vg(cmd, pv_vg_name(pv)))) {
		stack;
		return ECMD_FAILED;
	}

	if ((lv_mirr = find_pvmove_lv(vg, pv_dev(pv), PVMOVE))) {
		log_print("Detected pvmove in progress for %s", pv_name);
		if (argc || lv_name)
			log_error("Ignoring remaining command line arguments");

		if (!(lvs_changed = lvs_using_lv(cmd, vg, lv_mirr))) {
			log_error
			    ("ABORTING: Failed to generate list of moving LVs");
			unlock_vg(cmd, pv_vg_name(pv));
			return ECMD_FAILED;
		}

		/* Ensure mirror LV is active */
		if (!activate_lv_excl(cmd, lv_mirr)) {
			log_error
			    ("ABORTING: Temporary mirror activation failed.");
			unlock_vg(cmd, pv_vg_name(pv));
			return ECMD_FAILED;
		}

		first_time = 0;
	} else {
		/* Determine PE ranges to be moved */
		if (!(source_pvl = create_pv_list(cmd->mem, vg, 1,
						  &pv_name_arg, 0))) {
			stack;
			unlock_vg(cmd, pv_vg_name(pv));
			return ECMD_FAILED;
		}

		alloc = arg_uint_value(cmd, alloc_ARG, ALLOC_INHERIT);
		if (alloc == ALLOC_INHERIT)
			alloc = vg->alloc;

		/* Get PVs we can use for allocation */
		if (!(allocatable_pvs = _get_allocatable_pvs(cmd, argc, argv,
							     vg, pv, alloc))) {
			stack;
			unlock_vg(cmd, pv_vg_name(pv));
			return ECMD_FAILED;
		}

		if (!archive(vg)) {
			unlock_vg(cmd, pv_vg_name(pv));
			stack;
			return ECMD_FAILED;
		}

		if (!(lv_mirr = _set_up_pvmove_lv(cmd, vg, source_pvl, lv_name,
						  allocatable_pvs, alloc,
						  &lvs_changed))) {
			stack;
			unlock_vg(cmd, pv_vg_name(pv));
			return ECMD_FAILED;
		}
	}

	/* Lock lvs_changed for exclusive use and activate (with old metadata) */
	if (!activate_lvs_excl(cmd, lvs_changed)) {
		stack;
		unlock_vg(cmd, pv_vg_name(pv));
		return ECMD_FAILED;
	}

	/* FIXME Presence of a mirror once set PVMOVE - now remove associated logic */
	/* init_pvmove(1); */
	/* vg->status |= PVMOVE; */

	if (first_time) {
		if (!_update_metadata
		    (cmd, vg, lv_mirr, lvs_changed, first_time)) {
			stack;
			unlock_vg(cmd, pv_vg_name(pv));
			return ECMD_FAILED;
		}
	}

	/* LVs are all in status LOCKED */
	unlock_vg(cmd, pv_vg_name(pv));

	return ECMD_PROCESSED;
}

static int _finish_pvmove(struct cmd_context *cmd, struct volume_group *vg,
			  struct logical_volume *lv_mirr,
			  struct list *lvs_changed)
{
	int r = 1;

	/* Update metadata to remove mirror segments and break dependencies */
	if (!remove_pvmove_mirrors(vg, lv_mirr)) {
		log_error("ABORTING: Removal of temporary mirror failed");
		return 0;
	}

	/* Store metadata without dependencies on mirror segments */
	if (!vg_write(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		return 0;
	}

	/* Suspend LVs changed */
	if (!suspend_lvs(cmd, lvs_changed)) {
		log_error("Locking LVs to remove temporary mirror failed");
		r = 0;
	}

	/* Suspend mirror LV to flush pending I/O */
	if (!suspend_lv(cmd, lv_mirr)) {
		log_error("Suspension of temporary mirror LV failed");
		r = 0;
	}

	/* Store metadata without dependencies on mirror segments */
	if (!vg_commit(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		vg_revert(vg);
		resume_lv(cmd, lv_mirr);
		resume_lvs(cmd, lvs_changed);
		return 0;
	}

	/* Release mirror LV.  (No pending I/O because it's been suspended.) */
	if (!resume_lv(cmd, lv_mirr)) {
		log_error("Unable to reactivate logical volume \"%s\"",
			  lv_mirr->name);
		r = 0;
	}

	/* Unsuspend LVs */
	resume_lvs(cmd, lvs_changed);

	/* Deactivate mirror LV */
	if (!deactivate_lv(cmd, lv_mirr)) {
		log_error("ABORTING: Unable to deactivate temporary logical "
			  "volume \"%s\"", lv_mirr->name);
		r = 0;
	}

	log_verbose("Removing temporary pvmove LV");
	if (!lv_remove(lv_mirr)) {
		log_error("ABORTING: Removal of temporary pvmove LV failed");
		return 0;
	}

	/* Store it on disks */
	log_verbose("Writing out final volume group after pvmove");
	if (!vg_write(vg) || !vg_commit(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		return 0;
	}

	/* FIXME backup positioning */
	backup(vg);

	return r;
}

static struct volume_group *_get_move_vg(struct cmd_context *cmd,
					 const char *name)
{
	struct physical_volume *pv;

	/* Reread all metadata in case it got changed */
	if (!(pv = find_pv_by_name(cmd, name))) {
		log_error("ABORTING: Can't reread PV %s", name);
		/* What more could we do here? */
		return NULL;
	}

	return _get_vg(cmd, pv_vg_name(pv));
}

static struct poll_functions _pvmove_fns = {
	.get_copy_name_from_lv = get_pvmove_pvname_from_lv_mirr,
	.get_copy_vg = _get_move_vg,
	.get_copy_lv = find_pvmove_lv_from_pvname,
	.update_metadata = _update_metadata,
	.finish_copy = _finish_pvmove,
};

int pvmove_poll(struct cmd_context *cmd, const char *pv_name,
		unsigned background)
{
	return poll_daemon(cmd, pv_name, background, PVMOVE, &_pvmove_fns);
}

int pvmove(struct cmd_context *cmd, int argc, char **argv)
{
	char *pv_name = NULL;
	char *colon;
	int ret;

	if (argc) {
		pv_name = argv[0];

		/* Drop any PE lists from PV name */
		if ((colon = strchr(pv_name, ':'))) {
			if (!(pv_name = dm_pool_strndup(cmd->mem, pv_name,
						     (unsigned) (colon -
								 pv_name)))) {
				log_error("Failed to clone PV name");
				return 0;
			}
		}

		if (!arg_count(cmd, abort_ARG) &&
		    (ret = _set_up_pvmove(cmd, pv_name, argc, argv)) !=
		    ECMD_PROCESSED) {
			stack;
			return ret;
		}

	}

	return pvmove_poll(cmd, pv_name,
			   arg_count(cmd, background_ARG) ? 1U : 0);
}
