/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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
#include "polldaemon.h"
#include "lvm2cmdline.h"
#include <signal.h>
#include <sys/wait.h>

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
static int _become_daemon(struct cmd_context *cmd)
{
	pid_t pid;
	struct sigaction act = {
		{_sigchld_handler},
		.sa_flags = SA_NOCLDSTOP,
	};

	log_verbose("Forking background process");

	sigaction(SIGCHLD, &act, NULL);

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
	init_verbose(VERBOSE_BASE_LEVEL);

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	strncpy(*cmd->argv, "(lvm2)", strlen(*cmd->argv));

	reset_locking();
	if (!lvmcache_init())
		/* FIXME Clean up properly here */
		_exit(ECMD_FAILED);
	dev_close_all();

	return 1;
}

progress_t poll_mirror_progress(struct cmd_context *cmd,
				struct logical_volume *lv, const char *name,
				struct daemon_parms *parms)
{
	percent_t segment_percent = PERCENT_0, overall_percent = PERCENT_0;
	uint32_t event_nr = 0;

	if (!lv_is_mirrored(lv) ||
	    !lv_mirror_percent(cmd, lv, !parms->interval, &segment_percent,
			       &event_nr) ||
	    (segment_percent == PERCENT_INVALID)) {
		log_error("ABORTING: Mirror percentage check failed.");
		return PROGRESS_CHECK_FAILED;
	}

	overall_percent = copy_percent(lv);
	if (parms->progress_display)
		log_print("%s: %s: %.1f%%", name, parms->progress_title,
			  percent_to_float(overall_percent));
	else
		log_verbose("%s: %s: %.1f%%", name, parms->progress_title,
			    percent_to_float(overall_percent));

	if (segment_percent != PERCENT_100)
		return PROGRESS_UNFINISHED;

	if (overall_percent == PERCENT_100)
		return PROGRESS_FINISHED_ALL;

	return PROGRESS_FINISHED_SEGMENT;
}

static int _check_lv_status(struct cmd_context *cmd,
			    struct volume_group *vg,
			    struct logical_volume *lv,
			    const char *name, struct daemon_parms *parms,
			    int *finished)
{
	struct dm_list *lvs_changed;
	progress_t progress;

	/* By default, caller should not retry */
	*finished = 1;

	if (parms->aborting) {
		if (!(lvs_changed = lvs_using_lv(cmd, vg, lv))) {
			log_error("Failed to generate list of copied LVs: "
				  "can't abort.");
			return 0;
		}
		if (!parms->poll_fns->finish_copy(cmd, vg, lv, lvs_changed))
			return_0;

		return 1;
	}

	progress = parms->poll_fns->poll_progress(cmd, lv, name, parms);
	if (progress == PROGRESS_CHECK_FAILED)
		return_0;

	if (progress == PROGRESS_UNFINISHED) {
		/* The only case the caller *should* try again later */
		*finished = 0;
		return 1;
	}

	if (!(lvs_changed = lvs_using_lv(cmd, vg, lv))) {
		log_error("ABORTING: Failed to generate list of copied LVs");
		return 0;
	}

	/* Finished? Or progress to next segment? */
	if (progress == PROGRESS_FINISHED_ALL) {
		if (!parms->poll_fns->finish_copy(cmd, vg, lv, lvs_changed))
			return_0;
	} else {
		if (parms->poll_fns->update_metadata &&
		    !parms->poll_fns->update_metadata(cmd, vg, lv, lvs_changed, 0)) {
			log_error("ABORTING: Segment progression failed.");
			parms->poll_fns->finish_copy(cmd, vg, lv, lvs_changed);
			return 0;
		}
		*finished = 0;	/* Another segment */
	}

	return 1;
}

static void _sleep_and_rescan_devices(struct daemon_parms *parms)
{
	/* FIXME Use alarm for regular intervals instead */
	if (parms->interval && !parms->aborting) {
		sleep(parms->interval);
		/* Devices might have changed while we slept */
		init_full_scan_done(0);
	}
}

static int _wait_for_single_lv(struct cmd_context *cmd, const char *name, const char *uuid,
			       struct daemon_parms *parms)
{
	struct volume_group *vg;
	struct logical_volume *lv;
	int finished = 0;

	/* Poll for completion */
	while (!finished) {
		if (parms->wait_before_testing)
			_sleep_and_rescan_devices(parms);

		/* Locks the (possibly renamed) VG again */
		vg = parms->poll_fns->get_copy_vg(cmd, name, uuid);
		if (vg_read_error(vg)) {
			release_vg(vg);
			log_error("ABORTING: Can't reread VG for %s", name);
			/* What more could we do here? */
			return 0;
		}

		lv = parms->poll_fns->get_copy_lv(cmd, vg, name, uuid, parms->lv_type);

		if (!lv && parms->lv_type == PVMOVE) {
			log_print("%s: no pvmove in progress - already finished or aborted.",
				  name);
			unlock_and_release_vg(cmd, vg, vg->name);
			return 1;
		}

		if (!lv) {
			log_error("ABORTING: Can't find LV in %s for %s",
				  vg->name, name);
			unlock_and_release_vg(cmd, vg, vg->name);
			return 0;
		}

		if (!_check_lv_status(cmd, vg, lv, name, parms, &finished)) {
			unlock_and_release_vg(cmd, vg, vg->name);
			return_0;
		}

		unlock_and_release_vg(cmd, vg, vg->name);

		/*
		 * FIXME Sleeping after testing, while preferred, also works around
		 * unreliable "finished" state checking in _percent_run.  If the
		 * above _check_lv_status is deferred until after the first sleep it
		 * may be that a polldaemon will run without ever completing.
		 *
		 * This happens when one snapshot-merge polldaemon is racing with
		 * another (polling the same LV).  The first to see the LV status
		 * reach the "finished" state will alter the LV that the other
		 * polldaemon(s) are polling.  These other polldaemon(s) can then
		 * continue polling an LV that doesn't have a "status".
		 */
		if (!parms->wait_before_testing)
			_sleep_and_rescan_devices(parms);
	}

	return 1;
}

static int _poll_vg(struct cmd_context *cmd, const char *vgname,
		    struct volume_group *vg, void *handle)
{
	struct daemon_parms *parms = (struct daemon_parms *) handle;
	struct lv_list *lvl;
	struct logical_volume *lv;
	const char *name;
	int finished;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if (!(lv->status & parms->lv_type))
			continue;
		name = parms->poll_fns->get_copy_name_from_lv(lv);
		if (!name && !parms->aborting)
			continue;

		/* FIXME Need to do the activation from _set_up_pvmove here
		 *       if it's not running and we're not aborting */
		if (_check_lv_status(cmd, vg, lv, name, parms, &finished) &&
		    !finished)
			parms->outstanding_count++;
	}

	return ECMD_PROCESSED;

}

static void _poll_for_all_vgs(struct cmd_context *cmd,
			      struct daemon_parms *parms)
{
	while (1) {
		parms->outstanding_count = 0;
		process_each_vg(cmd, 0, NULL, READ_FOR_UPDATE, parms, _poll_vg);
		if (!parms->outstanding_count)
			break;
		sleep(parms->interval);
	}
}

/*
 * Only allow *one* return from poll_daemon() (the parent).
 * If there is a child it must exit (ignoring the memory leak messages).
 * - 'background' is advisory so a child polldaemon may not be used even
 *   if it was requested.
 */
int poll_daemon(struct cmd_context *cmd, const char *name, const char *uuid,
		unsigned background,
		uint32_t lv_type, struct poll_functions *poll_fns,
		const char *progress_title)
{
	struct daemon_parms parms;
	int daemon_mode = 0;
	int ret = ECMD_PROCESSED;
	sign_t interval_sign;

	parms.aborting = arg_is_set(cmd, abort_ARG);
	parms.background = background;
	interval_sign = arg_sign_value(cmd, interval_ARG, 0);
	if (interval_sign == SIGN_MINUS)
		log_error("Argument to --interval cannot be negative");
	parms.interval = arg_uint_value(cmd, interval_ARG,
					find_config_tree_int(cmd, "activation/polling_interval",
							     DEFAULT_INTERVAL));
	parms.wait_before_testing = (interval_sign == SIGN_PLUS);
	parms.progress_display = 1;
	parms.progress_title = progress_title;
	parms.lv_type = lv_type;
	parms.poll_fns = poll_fns;

	if (parms.interval && !parms.aborting)
		log_verbose("Checking progress %s waiting every %u seconds",
			    (parms.wait_before_testing ? "after" : "before"),
			    parms.interval);

	if (!parms.interval) {
		parms.progress_display = 0;

		/* FIXME Disabled multiple-copy wait_event */
		if (!name)
			parms.interval = find_config_tree_int(cmd, "activation/polling_interval",
							      DEFAULT_INTERVAL);
	}

	if (parms.background) {
		daemon_mode = _become_daemon(cmd);
		if (daemon_mode == 0)
			return ECMD_PROCESSED;	    /* Parent */
		else if (daemon_mode == 1)
			parms.progress_display = 0; /* Child */
		/* FIXME Use wait_event (i.e. interval = 0) and */
		/*       fork one daemon per copy? */
	}

	/*
	 * Process one specific task or all incomplete tasks?
	 */
	if (name) {
		if (!_wait_for_single_lv(cmd, name, uuid, &parms)) {
			stack;
			ret = ECMD_FAILED;
		}
	} else
		_poll_for_all_vgs(cmd, &parms);

	if (parms.background && daemon_mode == 1) {
		/*
		 * child was successfully forked:
		 * background polldaemon must not return to the caller
		 * because it will redundantly continue performing the
		 * caller's task (that the parent already performed)
		 */
		/* FIXME Attempt proper cleanup */
		_exit(lvm_return_code(ret));
	}

	return ret;
}
