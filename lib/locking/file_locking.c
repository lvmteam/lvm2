/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "locking.h"
#include "locking_types.h"
#include "activate.h"
#include "config.h"
#include "defaults.h"
#include "lvm-string.h"
#include "lvm-flock.h"
#include "lvmcache.h"

#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

static char _lock_dir[PATH_MAX];

static void _fin_file_locking(void)
{
	release_flocks(1);
}

static void _reset_file_locking(void)
{
	release_flocks(0);
}

static int _file_lock_resource(struct cmd_context *cmd, const char *resource,
			       uint32_t flags, const struct logical_volume *lv)
{
	char lockfile[PATH_MAX];
	unsigned origin_only = (flags & LCK_ORIGIN_ONLY) ? 1 : 0;
	unsigned revert = (flags & LCK_REVERT) ? 1 : 0;

	switch (flags & LCK_SCOPE_MASK) {
	case LCK_ACTIVATION:
		if (dm_snprintf(lockfile, sizeof(lockfile),
				"%s/A_%s", _lock_dir, resource + 1) < 0) {
			log_error("Too long locking filename %s/A_%s.", _lock_dir, resource + 1);
			return 0;
		}

		if (!lock_file(lockfile, flags))
			return_0;
		break;
	case LCK_VG:
		if (!strcmp(resource, VG_SYNC_NAMES)) {
			fs_unlock();
		} else if (strcmp(resource, VG_GLOBAL))
			/* Skip cache refresh for VG_GLOBAL - the caller handles it */
			lvmcache_drop_metadata(resource, 0);

		/* LCK_CACHE does not require a real lock */
		if (flags & LCK_CACHE)
			break;

		if (is_orphan_vg(resource) || is_global_vg(resource)) {
			if (dm_snprintf(lockfile, sizeof(lockfile),
					"%s/P_%s", _lock_dir, resource + 1) < 0) {
				log_error("Too long locking filename %s/P_%s.",
					  _lock_dir, resource + 1);
				return 0;
			}
		} else
			if (dm_snprintf(lockfile, sizeof(lockfile),
					"%s/V_%s", _lock_dir, resource) < 0) {
				log_error("Too long locking filename %s/V_%s.",
					  _lock_dir, resource);
				return 0;
			}

		if (!lock_file(lockfile, flags))
			return_0;
		break;
	case LCK_LV:
		switch (flags & LCK_TYPE_MASK) {
		case LCK_UNLOCK:
			log_very_verbose("Unlocking LV %s%s%s", resource, origin_only ? " without snapshots" : "", revert ? " (reverting)" : "");
			if (!lv_resume_if_active(cmd, resource, origin_only, 0, revert, lv_ondisk(lv)))
				return 0;
			break;
		case LCK_NULL:
			log_very_verbose("Locking LV %s (NL)", resource);
			if (!lv_deactivate(cmd, resource, lv_ondisk(lv)))
				return 0;
			break;
		case LCK_READ:
			log_very_verbose("Locking LV %s (R)", resource);
			if (!lv_activate_with_filter(cmd, resource, 0, lv->status & LV_NOSCAN ? 1 : 0,
						     lv->status & LV_TEMPORARY ? 1 : 0, lv_ondisk(lv)))
				return 0;
			break;
		case LCK_PREAD:
			log_very_verbose("Locking LV %s (PR) - ignored", resource);
			break;
		case LCK_WRITE:
			log_very_verbose("Locking LV %s (W)%s", resource, origin_only ? " without snapshots" : "");
			if (!lv_suspend_if_active(cmd, resource, origin_only, 0, lv_ondisk(lv), lv))
				return 0;
			break;
		case LCK_EXCL:
			log_very_verbose("Locking LV %s (EX)", resource);
			if (!lv_activate_with_filter(cmd, resource, 1, lv->status & LV_NOSCAN ? 1 : 0,
						     lv->status & LV_TEMPORARY ? 1 : 0, lv_ondisk(lv)))
				return 0;
			break;
		default:
			break;
		}
		break;
	default:
		log_error("Unrecognised lock scope: %d",
			  flags & LCK_SCOPE_MASK);
		return 0;
	}

	return 1;
}

int init_file_locking(struct locking_type *locking, struct cmd_context *cmd,
		      int suppress_messages)
{
	int r;
	const char *locking_dir;

	init_flock(cmd);

	locking->lock_resource = _file_lock_resource;
	locking->reset_locking = _reset_file_locking;
	locking->fin_locking = _fin_file_locking;
	locking->flags = 0;

	/* Get lockfile directory from config file */
	locking_dir = find_config_tree_str(cmd, global_locking_dir_CFG, NULL);
	if (strlen(locking_dir) >= sizeof(_lock_dir)) {
		log_error("Path for locking_dir %s is invalid.", locking_dir);
		return 0;
	}

	strcpy(_lock_dir, locking_dir);

	(void) dm_prepare_selinux_context(_lock_dir, S_IFDIR);
	r = dm_create_dir(_lock_dir);
	(void) dm_prepare_selinux_context(NULL, 0);

	if (!r)
		return 0;

	/* Trap a read-only file system */
	if ((access(_lock_dir, R_OK | W_OK | X_OK) == -1) && (errno == EROFS))
		return 0;

	return 1;
}
