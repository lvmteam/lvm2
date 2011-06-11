/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
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

/*
 * Increments *count by the number of _new_ monitored devices.
 */
static int _monitor_lvs_in_vg(struct cmd_context *cmd,
			      struct volume_group *vg, int reg, int *count)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct lvinfo info;
	int lv_active;
	int r = 1;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!lv_info(cmd, lv, 0, &info, 0, 0))
			lv_active = 0;
		else
			lv_active = info.exists;

		/*
		 * FIXME: Need to consider all cases... PVMOVE, etc
		 */
		if ((lv->status & PVMOVE) || !lv_active)
			continue;

		if (!monitor_dev_for_events(cmd, lv, 0, reg)) {
			r = 0;
			continue;
		} else
			(*count)++;
	}

	return r;
}

static int _poll_lvs_in_vg(struct cmd_context *cmd,
			   struct volume_group *vg)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct lvinfo info;
	int lv_active;
	int count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!lv_info(cmd, lv, 0, &info, 0, 0))
			lv_active = 0;
		else
			lv_active = info.exists;

		if (lv_active &&
		    (lv->status & (PVMOVE|CONVERTING|MERGING))) {
			lv_spawn_background_polling(cmd, lv);
			count++;
		}
	}

	/*
	 * returns the number of polled devices
	 * - there is no way to know if lv is already being polled
	 */

	return count;
}

static int _activate_lvs_in_vg(struct cmd_context *cmd,
			       struct volume_group *vg, int activate)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	int count = 0, expected_count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!lv_is_visible(lv))
			continue;

		/* Only request activation of snapshot origin devices */
		if ((lv->status & SNAPSHOT) || lv_is_cow(lv))
			continue;

		/* Only request activation of mirror LV */
		if ((lv->status & MIRROR_IMAGE) || (lv->status & MIRROR_LOG))
			continue;

		/* Only request activation of the first replicator-dev LV */
		/* Avoids retry with all heads in case of failure */
		if (lv_is_replicator_dev(lv) && (lv != first_replicator_dev(lv)))
			continue;

		/* Can't deactivate a pvmove LV */
		/* FIXME There needs to be a controlled way of doing this */
		if (((activate == CHANGE_AN) || (activate == CHANGE_ALN)) &&
		    ((lv->status & PVMOVE) ))
			continue;

		/*
		 * If the LV is active exclusive remotely,
		 * then ignore it here
		 */
		if (lv_is_active_exclusive_remotely(lv)) {
			log_verbose("%s/%s is exclusively active on"
				    " a remote node", vg->name, lv->name);
			continue;
		}

		expected_count++;

		if (activate == CHANGE_AN) {
			if (!deactivate_lv(cmd, lv)) {
				stack;
				continue;
			}
		} else if (activate == CHANGE_ALN) {
			if (!deactivate_lv_local(cmd, lv)) {
				stack;
				continue;
			}
		} else if (lv_is_origin(lv) || (activate == CHANGE_AE)) {
			if (!activate_lv_excl(cmd, lv)) {
				stack;
				continue;
			}
		} else if (activate == CHANGE_ALY) {
			if (!activate_lv_local(cmd, lv)) {
				stack;
				continue;
			}
		} else if (!activate_lv(cmd, lv)) {
			stack;
			continue;
		}

		if (background_polling() &&
		    activate != CHANGE_AN && activate != CHANGE_ALN &&
		    (lv->status & (PVMOVE|CONVERTING|MERGING)))
			lv_spawn_background_polling(cmd, lv);

		count++;
	}

	if (expected_count)
		log_verbose("%s %d logical volumes in volume group %s",
			    (activate == CHANGE_AN || activate == CHANGE_ALN)?
			    "Deactivated" : "Activated", count, vg->name);

	return (expected_count != count) ? 0 : 1;
}

static int _vgchange_monitoring(struct cmd_context *cmd, struct volume_group *vg)
{
	int r = 1;
	int monitored = 0;

	if (lvs_in_vg_activated(vg) &&
	    dmeventd_monitor_mode() != DMEVENTD_MONITOR_IGNORE) {
		if (!_monitor_lvs_in_vg(cmd, vg, dmeventd_monitor_mode(), &monitored))
			r = 0;
		log_print("%d logical volume(s) in volume group "
			    "\"%s\" %smonitored",
			    monitored, vg->name, (dmeventd_monitor_mode()) ? "" : "un");
	}

	return r;
}

static int _vgchange_background_polling(struct cmd_context *cmd, struct volume_group *vg)
{
	int polled;

	if (lvs_in_vg_activated(vg) && background_polling()) {
	        polled = _poll_lvs_in_vg(cmd, vg);
		if (polled)
			log_print("Background polling started for %d logical volume(s) "
				  "in volume group \"%s\"",
				  polled, vg->name);
	}

	return 1;
}

static int _vgchange_available(struct cmd_context *cmd, struct volume_group *vg)
{
	int lv_open, active, monitored = 0;
	int available, r = 1;
	int activate = 1;

	/*
	 * Safe, since we never write out new metadata here. Required for
	 * partial activation to work.
	 */
	cmd->handles_missing_pvs = 1;

	available = arg_uint_value(cmd, available_ARG, 0);

	if ((available == CHANGE_AN) || (available == CHANGE_ALN))
		activate = 0;

	/* FIXME: Force argument to deactivate them? */
	if (!activate && (lv_open = lvs_in_vg_opened(vg))) {
		log_error("Can't deactivate volume group \"%s\" with %d open "
			  "logical volume(s)", vg->name, lv_open);
		return 0;
	}

	/* FIXME Move into library where clvmd can use it */
	if (activate)
		check_current_backup(vg);

	if (activate && (active = lvs_in_vg_activated(vg))) {
		log_verbose("%d logical volume(s) in volume group \"%s\" "
			    "already active", active, vg->name);
		if (dmeventd_monitor_mode() != DMEVENTD_MONITOR_IGNORE) {
			if (!_monitor_lvs_in_vg(cmd, vg, dmeventd_monitor_mode(), &monitored))
				r = 0;
			log_verbose("%d existing logical volume(s) in volume "
				    "group \"%s\" %smonitored",
				    monitored, vg->name,
				    dmeventd_monitor_mode() ? "" : "un");
		}
	}

	if (!_activate_lvs_in_vg(cmd, vg, available))
		r = 0;

	/* Print message only if there was not found a missing VG */
	if (!vg->cmd_missing_vgs)
		log_print("%d logical volume(s) in volume group \"%s\" now active",
			  lvs_in_vg_activated(vg), vg->name);
	return r;
}

static int _vgchange_refresh(struct cmd_context *cmd, struct volume_group *vg)
{
	log_verbose("Refreshing volume group \"%s\"", vg->name);

	if (!vg_refresh_visible(cmd, vg)) {
		stack;
		return 0;
	}

	return 1;
}

static int _vgchange_alloc(struct cmd_context *cmd, struct volume_group *vg)
{
	alloc_policy_t alloc;

	alloc = arg_uint_value(cmd, alloc_ARG, ALLOC_NORMAL);

	/* FIXME: make consistent with vg_set_alloc_policy() */
	if (alloc == vg->alloc) {
		log_error("Volume group allocation policy is already %s",
			  get_alloc_string(vg->alloc));
		return 0;
	}

	if (!vg_set_alloc_policy(vg, alloc))
		return_0;

	return 1;
}

static int _vgchange_resizeable(struct cmd_context *cmd,
				struct volume_group *vg)
{
	int resizeable = !strcmp(arg_str_value(cmd, resizeable_ARG, "n"), "y");

	if (resizeable && vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" is already resizeable",
			  vg->name);
		return 0;
	}

	if (!resizeable && !vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" is already not resizeable",
			  vg->name);
		return 0;
	}

	if (resizeable)
		vg->status |= RESIZEABLE_VG;
	else
		vg->status &= ~RESIZEABLE_VG;

	return 1;
}

static int _vgchange_clustered(struct cmd_context *cmd,
			       struct volume_group *vg)
{
	int clustered = !strcmp(arg_str_value(cmd, clustered_ARG, "n"), "y");

	if (clustered && (vg_is_clustered(vg))) {
		log_error("Volume group \"%s\" is already clustered",
			  vg->name);
		return 0;
	}

	if (!clustered && !(vg_is_clustered(vg))) {
		log_error("Volume group \"%s\" is already not clustered",
			  vg->name);
		return 0;
	}

	if (!vg_set_clustered(vg, clustered))
		return_0;

	return 1;
}

static int _vgchange_logicalvolume(struct cmd_context *cmd,
				   struct volume_group *vg)
{
	uint32_t max_lv = arg_uint_value(cmd, logicalvolume_ARG, 0);

	if (!vg_set_max_lv(vg, max_lv))
		return_0;

	return 1;
}

static int _vgchange_physicalvolumes(struct cmd_context *cmd,
				     struct volume_group *vg)
{
	uint32_t max_pv = arg_uint_value(cmd, maxphysicalvolumes_ARG, 0);

	if (!vg_set_max_pv(vg, max_pv))
		return_0;

	return 1;
}

static int _vgchange_pesize(struct cmd_context *cmd, struct volume_group *vg)
{
	uint32_t extent_size;

	if (arg_uint64_value(cmd, physicalextentsize_ARG, 0) > MAX_EXTENT_SIZE) {
		log_error("Physical extent size cannot be larger than %s",
				  display_size(cmd, (uint64_t) MAX_EXTENT_SIZE));
		return 1;
	}

	extent_size = arg_uint_value(cmd, physicalextentsize_ARG, 0);
	/* FIXME: remove check - redundant with vg_change_pesize */
	if (extent_size == vg->extent_size) {
		log_error("Physical extent size of VG %s is already %s",
			  vg->name, display_size(cmd, (uint64_t) extent_size));
		return 1;
	}

	if (!vg_set_extent_size(vg, extent_size))
		return_0;

	return 1;
}

static int _vgchange_addtag(struct cmd_context *cmd, struct volume_group *vg)
{
	return change_tag(cmd, vg, NULL, NULL, addtag_ARG);
}

static int _vgchange_deltag(struct cmd_context *cmd, struct volume_group *vg)
{
	return change_tag(cmd, vg, NULL, NULL, deltag_ARG);
}

static int _vgchange_uuid(struct cmd_context *cmd __attribute__((unused)),
			  struct volume_group *vg)
{
	struct lv_list *lvl;

	if (lvs_in_vg_activated(vg)) {
		log_error("Volume group has active logical volumes");
		return 0;
	}

	if (!id_create(&vg->id)) {
		log_error("Failed to generate new random UUID for VG %s.",
			  vg->name);
		return 0;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		memcpy(&lvl->lv->lvid, &vg->id, sizeof(vg->id));
	}

	return 1;
}

static int _vgchange_metadata_copies(struct cmd_context *cmd,
				     struct volume_group *vg)
{
	uint32_t mda_copies = arg_uint_value(cmd, vgmetadatacopies_ARG, DEFAULT_VGMETADATACOPIES);

	if (mda_copies == vg_mda_copies(vg)) {
		if (vg_mda_copies(vg) == VGMETADATACOPIES_UNMANAGED)
			log_error("Number of metadata copies for VG %s is already unmanaged.",
				  vg->name);
		else
			log_error("Number of metadata copies for VG %s is already %" PRIu32,
				  vg->name, mda_copies);
		return 1;
	}

	if (!vg_set_mda_copies(vg, mda_copies))
		return_0;

	return 1;
}

static int vgchange_single(struct cmd_context *cmd, const char *vg_name,
			   struct volume_group *vg,
			   void *handle __attribute__((unused)))
{
	int dmeventd_mode;
	int archived = 0;
	int i;

	static struct {
		int arg;
		int (*fn)(struct cmd_context *cmd, struct volume_group *vg);
	} _vgchange_args[] = {
		{ logicalvolume_ARG, &_vgchange_logicalvolume },
		{ maxphysicalvolumes_ARG, &_vgchange_physicalvolumes },
		{ resizeable_ARG, &_vgchange_resizeable },
		{ deltag_ARG, &_vgchange_deltag },
		{ addtag_ARG, &_vgchange_addtag },
		{ physicalextentsize_ARG, &_vgchange_pesize },
		{ uuid_ARG, &_vgchange_uuid },
		{ alloc_ARG, &_vgchange_alloc },
		{ clustered_ARG, &_vgchange_clustered },
		{ vgmetadatacopies_ARG, &_vgchange_metadata_copies },
		{ -1, NULL },
	};

	if (vg_is_exported(vg)) {
		log_error("Volume group \"%s\" is exported", vg_name);
		return ECMD_FAILED;
	}

	if (!get_activation_monitoring_mode(cmd, vg, &dmeventd_mode))
		return ECMD_FAILED;

	init_dmeventd_monitor(dmeventd_mode);

	/*
	 * FIXME: DEFAULT_BACKGROUND_POLLING should be "unspecified".
	 * If --poll is explicitly provided use it; otherwise polling
	 * should only be started if the LV is not already active. So:
	 * 1) change the activation code to say if the LV was actually activated
	 * 2) make polling of an LV tightly coupled with LV activation
	 *
	 * Do not initiate any polling if --sysinit option is used.
	 */
	init_background_polling(arg_count(cmd, sysinit_ARG) ? 0 :
						arg_int_value(cmd, poll_ARG,
						DEFAULT_BACKGROUND_POLLING));

	for (i = 0; _vgchange_args[i].arg >= 0; i++) {
		if (arg_count(cmd, _vgchange_args[i].arg)) {
			if (!archived && !archive(vg)) {
				stack;
				return ECMD_FAILED;
			}
			archived = 1;
			if (!_vgchange_args[i].fn(cmd, vg)) {
				stack;
				return ECMD_FAILED;
			}
		}
	}

	if (archived) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			stack;
			return ECMD_FAILED;
		}

		backup(vg);

		log_print("Volume group \"%s\" successfully changed", vg->name);
	}

	if (arg_count(cmd, available_ARG)) {
		if (!_vgchange_available(cmd, vg))
			return ECMD_FAILED;
	}

	if (arg_count(cmd, refresh_ARG)) {
		/* refreshes the visible LVs (which starts polling) */
		if (!_vgchange_refresh(cmd, vg))
			return ECMD_FAILED;
	}

	if (!arg_count(cmd, available_ARG) &&
	    !arg_count(cmd, refresh_ARG) &&
	    arg_count(cmd, monitor_ARG)) {
		/* -ay* will have already done monitoring changes */
		if (!_vgchange_monitoring(cmd, vg))
			return ECMD_FAILED;
	}

	if (!arg_count(cmd, refresh_ARG) &&
	    background_polling())
		if (!_vgchange_background_polling(cmd, vg))
			return ECMD_FAILED;

        return ECMD_PROCESSED;
}

int vgchange(struct cmd_context *cmd, int argc, char **argv)
{
	/* Update commands that can be combined */
	int update = 
		arg_count(cmd, logicalvolume_ARG) ||
		arg_count(cmd, maxphysicalvolumes_ARG) ||
		arg_count(cmd, resizeable_ARG) ||
		arg_count(cmd, deltag_ARG) ||
		arg_count(cmd, addtag_ARG) ||
		arg_count(cmd, uuid_ARG) ||
		arg_count(cmd, physicalextentsize_ARG) ||
		arg_count(cmd, clustered_ARG) ||
		arg_count(cmd, alloc_ARG) ||
		arg_count(cmd, vgmetadatacopies_ARG);

	if (!update &&
	    !arg_count(cmd, available_ARG) &&
	    !arg_count(cmd, monitor_ARG) &&
	    !arg_count(cmd, poll_ARG) &&
	    !arg_count(cmd, refresh_ARG)) {
		log_error("Need 1 or more of -a, -c, -l, -p, -s, -x, "
			  "--refresh, --uuid, --alloc, --addtag, --deltag, "
			  "--monitor, --poll, --vgmetadatacopies or "
			  "--metadatacopies");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, available_ARG) && arg_count(cmd, refresh_ARG)) {
		log_error("Only one of -a and --refresh permitted.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_count(cmd, ignorelockingfailure_ARG) ||
	     arg_count(cmd, sysinit_ARG)) && update) {
		log_error("Only -a permitted with --ignorelockingfailure and --sysinit");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, available_ARG) &&
	    (arg_count(cmd, monitor_ARG) || arg_count(cmd, poll_ARG))) {
		int activate = arg_uint_value(cmd, available_ARG, 0);
		if (activate == CHANGE_AN || activate == CHANGE_ALN) {
			log_error("Only -ay* allowed with --monitor or --poll.");
			return EINVALID_CMD_LINE;
		}
	}

	if (arg_count(cmd, poll_ARG) && arg_count(cmd, sysinit_ARG)) {
		log_error("Only one of --poll and --sysinit permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, available_ARG) == 1
	    && arg_count(cmd, autobackup_ARG)) {
		log_error("-A option not necessary with -a option");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, maxphysicalvolumes_ARG) &&
	    arg_sign_value(cmd, maxphysicalvolumes_ARG, 0) == SIGN_MINUS) {
		log_error("MaxPhysicalVolumes may not be negative");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, physicalextentsize_ARG) &&
	    arg_sign_value(cmd, physicalextentsize_ARG, 0) == SIGN_MINUS) {
		log_error("Physical extent size may not be negative");
		return EINVALID_CMD_LINE;
	}

	return process_each_vg(cmd, argc, argv, update ? READ_FOR_UPDATE : 0,
			       NULL, &vgchange_single);
}
