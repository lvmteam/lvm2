/*
 * Copyright (C) 2003  Sistina Software
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
#include <signal.h>
#include <sys/wait.h>

struct pvmove_parms {
	unsigned interval;
	unsigned aborting;
	unsigned background;
	unsigned outstanding_count;
	unsigned progress_display;
};

static void _sigchld_handler(int sig)
{
	while (wait4(-1, NULL, WNOHANG | WUNTRACED, NULL) > 0) ;
}

static int _become_daemon(struct cmd_context *cmd)
{
	pid_t pid;
	struct sigaction act = {
		{_sigchld_handler},
		.sa_flags = SA_NOCLDSTOP,
	};

	log_verbose("Forking background process");

	sigaction(SIGCHLD, &act, NULL);

	if ((pid = fork()) == -1) {
		log_error("fork failed: %s", strerror(errno));
		return 1;
	}

	/* Parent */
	if (pid > 0)
		return 0;

	/* Child */
	if (setsid() == -1)
		log_error("Background process failed to setsid: %s",
			  strerror(errno));
	init_verbose(VERBOSE_BASE_LEVEL);

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	strncpy(*cmd->argv, "(pvmove)", strlen(*cmd->argv));

	reset_locking();

	return 1;
}

/* Allow /dev/vgname/lvname, vgname/lvname or lvname */
static const char *_extract_lvname(struct cmd_context *cmd, const char *vgname,
				   const char *arg)
{
	const char *lvname;

	/* Is an lvname supplied directly? */
	if (!strchr(arg, '/'))
		return arg;

	lvname = arg;
	if (!strncmp(lvname, cmd->dev_dir, strlen(cmd->dev_dir)))
		lvname += strlen(cmd->dev_dir);
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

static struct physical_volume *_find_pv_by_name(struct cmd_context *cmd,
						const char *pv_name)
{
	struct physical_volume *pv;

	if (!(pv = pv_read(cmd, pv_name, NULL, NULL))) {
		log_error("Physical volume %s not found", pv_name);
		return NULL;
	}

	if (!pv->vg_name[0]) {
		log_error("Physical volume %s not in a volume group", pv_name);
		return NULL;
	}

	return pv;
}

static struct volume_group *_get_vg(struct cmd_context *cmd, const char *vgname)
{
	int consistent = 1;
	struct volume_group *vg;

	if (!lock_vol(cmd, vgname, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vgname);
		return NULL;
	}

	if (!(vg = vg_read(cmd, vgname, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" doesn't exist", vgname);
		unlock_vg(cmd, vgname);
		return NULL;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vgname);
		unlock_vg(cmd, vgname);
		return NULL;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", vgname);
		unlock_vg(cmd, vgname);
		return NULL;
	}

	return vg;
}

/* Create list of PVs for allocation of replacement extents */
static struct list *_get_allocatable_pvs(struct cmd_context *cmd, int argc,
					 char **argv, struct volume_group *vg,
					 struct physical_volume *pv)
{
	struct list *allocatable_pvs, *pvht, *pvh;
	struct pv_list *pvl;

	if (argc) {
		if (!(allocatable_pvs = create_pv_list(cmd->mem, vg, argc,
						       argv))) {
			stack;
			return NULL;
		}
	} else {
		if (!(allocatable_pvs = clone_pv_list(cmd->mem, &vg->pvs))) {
			stack;
			return NULL;
		}
	}

	/* Don't allocate onto the PV we're clearing! */
	/* Remove PV if full */
	list_iterate_safe(pvh, pvht, allocatable_pvs) {
		pvl = list_item(pvh, struct pv_list);
		if ((pvl->pv->dev == pv->dev) ||
		    (pvl->pv->pe_count == pvl->pv->pe_alloc_count))
			    list_del(&pvl->list);
	}

	if (list_empty(allocatable_pvs)) {
		log_error("No extents available for allocation");
		return NULL;
	}

	return allocatable_pvs;
}

/* Create new LV with mirror segments for the required moves */
static struct logical_volume *_set_up_pvmove_lv(struct cmd_context *cmd,
						struct volume_group *vg,
						struct physical_volume *pv,
						const char *lv_name,
						struct list *allocatable_pvs,
						struct list **lvs_changed)
{
	struct logical_volume *lv_mirr, *lv;
	struct lv_list *lvl;

	/* FIXME Cope with non-contiguous => splitting existing segments */
	if (!(lv_mirr = lv_create_empty(vg->fid, NULL, "pvmove%d",
					LVM_READ | LVM_WRITE,
					ALLOC_CONTIGUOUS, vg))) {
		log_error("Creation of temporary pvmove LV failed");
		return NULL;
	}

	lv_mirr->status |= (PVMOVE | LOCKED);

	if (!(*lvs_changed = pool_alloc(cmd->mem, sizeof(**lvs_changed)))) {
		log_error("lvs_changed list struct allocation failed");
		return NULL;
	}

	list_init(*lvs_changed);

	/* Find segments to be moved and set up mirrors */
	list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if ((lv == lv_mirr) || (lv_name && strcmp(lv->name, lv_name)))
			continue;
		if (lv_is_origin(lv) || lv_is_cow(lv)) {
			log_print("Skipping snapshot-related LV %s", lv->name);
			continue;
		}
		if (lv->status & LOCKED) {
			log_print("Skipping locked LV %s", lv->name);
			continue;
		}
		if (!insert_pvmove_mirrors(cmd, lv_mirr, pv, lv,
					   allocatable_pvs, *lvs_changed)) {
			stack;
			return NULL;
		}
	}

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

	if (!lock_lvs(cmd, lvs_changed, LCK_LV_SUSPEND | LCK_HOLD)) {
		stack;
		return 0;
	}

	if (!first_time) {
		if (!lock_vol(cmd, lv_mirr->lvid.s, LCK_LV_SUSPEND | LCK_HOLD)) {
			stack;
			unlock_lvs(cmd, lvs_changed);
			vg_revert(vg);
			return 0;
		}
	}

	if (!vg_commit(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		if (!first_time)
			unlock_lv(cmd, lv_mirr->lvid.s);
		unlock_lvs(cmd, lvs_changed);
		return 0;
	}

	if (first_time) {
		if (!lock_vol(cmd, lv_mirr->lvid.s, LCK_LV_ACTIVATE)) {
			log_error
			    ("ABORTING: Temporary mirror activation failed.");
			unlock_lvs(cmd, lvs_changed);
			return 0;
		}
	} else if (!unlock_lv(cmd, lv_mirr->lvid.s)) {
		log_error("Unable to unlock logical volume \"%s\"",
			  lv_mirr->name);
		unlock_lvs(cmd, lvs_changed);
		return 0;
	}

	if (!unlock_lvs(cmd, lvs_changed)) {
		log_error("Unable to unlock logical volumes");
		return 0;
	}

	return 1;
}

static int _set_up_pvmove(struct cmd_context *cmd, const char *pv_name,
			  int argc, char **argv)
{
	const char *lv_name = NULL;
	struct volume_group *vg;
	struct list *allocatable_pvs;
	struct list *lvs_changed;
	struct physical_volume *pv;
	struct logical_volume *lv_mirr;
	int first_time = 1;

	/* Find PV (in VG) */
	if (!(pv = _find_pv_by_name(cmd, pv_name))) {
		stack;
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, name_ARG)) {
		if (!(lv_name = _extract_lvname(cmd, pv->vg_name,
						arg_value(cmd, name_ARG)))) {
			stack;
			return EINVALID_CMD_LINE;
		}
	}

	/* Read VG */
	log_verbose("Finding volume group \"%s\"", pv->vg_name);

	if (!(vg = _get_vg(cmd, pv->vg_name))) {
		stack;
		return ECMD_FAILED;
	}

	if ((lv_mirr = find_pvmove_lv(vg, pv->dev))) {
		log_print("Detected pvmove in progress for %s", pv_name);
		if (argc || lv_name)
			log_error("Ignoring remaining command line arguments");

		if (!(lvs_changed = lvs_using_lv(cmd, vg, lv_mirr))) {
			log_error
			    ("ABORTING: Failed to generate list of moving LVs");
			unlock_vg(cmd, pv->vg_name);
			return ECMD_FAILED;
		}

		/* Ensure mirror LV is active */
		if (!lock_vol(cmd, lv_mirr->lvid.s, LCK_LV_ACTIVATE)) {
			log_error
			    ("ABORTING: Temporary mirror activation failed.");
			unlock_vg(cmd, pv->vg_name);
			return ECMD_FAILED;
		}

		first_time = 0;
	} else {

		if (!(allocatable_pvs = _get_allocatable_pvs(cmd, argc, argv,
							     vg, pv))) {
			stack;
			unlock_vg(cmd, pv->vg_name);
			return ECMD_FAILED;
		}

		if (!archive(vg)) {
			unlock_vg(cmd, pv->vg_name);
			stack;
			return ECMD_FAILED;
		}

		if (!(lv_mirr = _set_up_pvmove_lv(cmd, vg, pv, lv_name,
						  allocatable_pvs,
						  &lvs_changed))) {
			stack;
			unlock_vg(cmd, pv->vg_name);
			return ECMD_FAILED;
		}
	}

	if (!lock_lvs(cmd, lvs_changed, LCK_LV_ACTIVATE)) {
		stack;
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (first_time) {
		if (!_update_metadata
		    (cmd, vg, lv_mirr, lvs_changed, first_time)) {
			stack;
			unlock_vg(cmd, pv->vg_name);
			return ECMD_FAILED;
		}
	}

	unlock_vg(cmd, pv->vg_name);

	return ECMD_PROCESSED;
}

static int _finish_pvmove(struct cmd_context *cmd, struct volume_group *vg,
			  struct logical_volume *lv_mirr,
			  struct list *lvs_changed)
{
	int r = 1;

	if (!remove_pvmove_mirrors(vg, lv_mirr)) {
		log_error("ABORTING: Removal of temporary mirror failed");
		return 0;
	}

	if (!vg_write(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		return 0;
	}

	if (!lock_lvs(cmd, lvs_changed, LCK_LV_SUSPEND | LCK_HOLD)) {
		log_error("Locking LVs to remove temporary mirror failed");
		r = 0;
	}

	if (!lock_vol(cmd, lv_mirr->lvid.s, LCK_LV_SUSPEND | LCK_HOLD)) {
		log_error("Suspension of temporary mirror LV failed");
		r = 0;
	}

	if (!vg_commit(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		vg_revert(vg);
		unlock_lv(cmd, lv_mirr->lvid.s);
		unlock_lvs(cmd, lvs_changed);
		return 0;
	}

	if (!unlock_lv(cmd, lv_mirr->lvid.s)) {
		log_error("Unable to unlock logical volume \"%s\"",
			  lv_mirr->name);
		r = 0;
	}

	unlock_lvs(cmd, lvs_changed);

	if (!lock_vol(cmd, lv_mirr->lvid.s, LCK_LV_DEACTIVATE)) {
		log_error("ABORTING: Unable to deactivate temporary logical "
			  "volume \"%s\"", lv_mirr->name);
		r = 0;
	}

	log_verbose("Removing temporary pvmove LV");
	if (!lv_remove(vg, lv_mirr)) {
		log_error("ABORTING: Removal of temporary pvmove LV failed");
		return 0;
	}

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

static int _check_pvmove_status(struct cmd_context *cmd,
				struct volume_group *vg,
				struct logical_volume *lv_mirr,
				const char *pv_name, struct pvmove_parms *parms,
				int *finished)
{
	struct list *lvs_changed;
	float segment_percent = 0.0, overall_percent = 0.0;
	uint32_t event_nr = 0;

	*finished = 1;

	if (parms->aborting) {
		if (!(lvs_changed = lvs_using_lv(cmd, vg, lv_mirr))) {
			log_error("Failed to generate list of moved LVs: "
				  "can't abort.");
			return 0;
		}
		_finish_pvmove(cmd, vg, lv_mirr, lvs_changed);
		return 0;
	}

	if (!lv_mirror_percent(lv_mirr, !parms->interval, &segment_percent,
			       &event_nr)) {
		log_error("ABORTING: Mirror percentage check failed.");
		return 0;
	}

	overall_percent = pvmove_percent(lv_mirr);
	if (parms->progress_display)
		log_print("%s: Moved: %.1f%%", pv_name, overall_percent);
	else
		log_verbose("%s: Moved: %.1f%%", pv_name, overall_percent);

	if (segment_percent < 100.0) {
		*finished = 0;
		return 1;
	}

	if (!(lvs_changed = lvs_using_lv(cmd, vg, lv_mirr))) {
		log_error("ABORTING: Failed to generate list of moved LVs");
		return 0;
	}

	if (overall_percent >= 100.0) {
		if (!_finish_pvmove(cmd, vg, lv_mirr, lvs_changed))
			return 0;
	} else {
		if (!_update_metadata(cmd, vg, lv_mirr, lvs_changed, 0)) {
			log_error("ABORTING: Segment progression failed.");
			_finish_pvmove(cmd, vg, lv_mirr, lvs_changed);
			return 0;
		}
		*finished = 0;	/* Another segment */
	}

	return 1;
}

static int _wait_for_single_pvmove(struct cmd_context *cmd, const char *pv_name,
				   struct pvmove_parms *parms)
{
	struct volume_group *vg;
	struct logical_volume *lv_mirr;
	struct physical_volume *pv;
	int finished = 0;

	while (!finished) {
		if (parms->interval && !parms->aborting)
			sleep(parms->interval);

		if (!(pv = _find_pv_by_name(cmd, pv_name))) {
			log_error("ABORTING: Can't reread PV %s", pv_name);
			return 0;
		}

		if (!(vg = _get_vg(cmd, pv->vg_name))) {
			log_error("ABORTING: Can't reread VG %s", pv->vg_name);
			return 0;
		}

		if (!(lv_mirr = find_pvmove_lv(vg, pv->dev))) {
			log_error("ABORTING: Can't reread mirror LV in %s",
				  vg->name);
			unlock_vg(cmd, pv->vg_name);
			return 0;
		}

		if (!_check_pvmove_status(cmd, vg, lv_mirr, pv_name, parms,
					  &finished)) {
			unlock_vg(cmd, pv->vg_name);
			return 0;
		}

		unlock_vg(cmd, pv->vg_name);
	}

	return 1;
}

static int _poll_pvmove_vgs(struct cmd_context *cmd, const char *vgname,
			    struct volume_group *vg, int consistent,
			    void *handle)
{
	struct pvmove_parms *parms = (struct pvmove_parms *) handle;
	struct lv_list *lvl;
	struct logical_volume *lv_mirr;
	struct physical_volume *pv;
	int finished;

	if (!vg) {
		log_error("Couldn't read volume group %s", vgname);
		return ECMD_FAILED;
	}

	if (!consistent) {
		log_error("Volume Group %s inconsistent - skipping", vgname);
		/* FIXME Should we silently recover it here or not? */
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg->name);
		return ECMD_FAILED;
	}

	list_iterate_items(lvl, &vg->lvs) {
		lv_mirr = lvl->lv;
		if (!(lv_mirr->status & PVMOVE))
			continue;
		if (!(pv = get_pvmove_pv_from_lv_mirr(lv_mirr)))
			continue;
		if (_check_pvmove_status(cmd, vg, lv_mirr, dev_name(pv->dev),
					 parms, &finished) && !finished)
			parms->outstanding_count++;
	}

	return ECMD_PROCESSED;

}

static void _poll_for_all_pvmoves(struct cmd_context *cmd,
				  struct pvmove_parms *parms)
{
	while (1) {
		parms->outstanding_count = 0;
		process_each_vg(cmd, 0, NULL, LCK_VG_WRITE, 1,
				parms, _poll_pvmove_vgs);
		if (!parms->outstanding_count)
			break;
		sleep(parms->interval);
	}
}

int pvmove_poll(struct cmd_context *cmd, const char *pv_name,
		unsigned background)
{
	struct pvmove_parms parms;

	parms.aborting = arg_count(cmd, abort_ARG) ? 1 : 0;
	parms.background = background;
	parms.interval = arg_uint_value(cmd, interval_ARG, DEFAULT_INTERVAL);
	parms.progress_display = 1;

	if (parms.interval && !parms.aborting)
		log_verbose("Checking progress every %u seconds",
			    parms.interval);

	if (!parms.interval) {
		parms.progress_display = 0;

		if (!pv_name)
			parms.interval = DEFAULT_INTERVAL;
	}

	if (parms.background) {
		if (!_become_daemon(cmd))
			return ECMD_PROCESSED;	/* Parent */
		parms.progress_display = 0;
	}

	if (pv_name) {
		if (!_wait_for_single_pvmove(cmd, pv_name, &parms))
			return ECMD_FAILED;
	} else
		_poll_for_all_pvmoves(cmd, &parms);

	return ECMD_PROCESSED;
}

int pvmove(struct cmd_context *cmd, int argc, char **argv)
{
	char *pv_name = NULL;
	int ret;

	if (argc) {
		pv_name = argv[0];
		argc--;
		argv++;

		if (!arg_count(cmd, abort_ARG) &&
		    (ret = _set_up_pvmove(cmd, pv_name, argc, argv)) !=
		     ECMD_PROCESSED) {
			stack;
			return ret;
		}
	}

	return pvmove_poll(cmd, pv_name,
			   arg_count(cmd, background_ARG) ? 1 : 0);
}
