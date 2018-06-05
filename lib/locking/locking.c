/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/locking/locking.h"
#include "locking_types.h"
#include "lib/misc/lvm-string.h"
#include "lib/activate/activate.h"
#include "lib/commands/toolcontext.h"
#include "lib/mm/memlock.h"
#include "lib/config/defaults.h"
#include "lib/cache/lvmcache.h"
#include "lib/misc/lvm-signal.h"

#include <assert.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

static struct locking_type _locking;

static int _vg_lock_count = 0;		/* Number of locks held */
static int _vg_write_lock_held = 0;	/* VG write lock held? */
static int _blocking_supported = 0;

static void _unblock_signals(void)
{
	/* Don't unblock signals while any locks are held */
	if (!_vg_lock_count)
		unblock_signals();
}

void reset_locking(void)
{
	int was_locked = _vg_lock_count;

	_vg_lock_count = 0;
	_vg_write_lock_held = 0;

	if (_locking.reset_locking)
		_locking.reset_locking();

	if (was_locked)
		_unblock_signals();

	memlock_reset();
}

static void _update_vg_lock_count(const char *resource, uint32_t flags)
{
	/* Ignore locks not associated with updating VG metadata */
	if ((flags & LCK_SCOPE_MASK) != LCK_VG ||
	    (flags & LCK_CACHE) ||
	    !strcmp(resource, VG_GLOBAL))
		return;

	if ((flags & LCK_TYPE_MASK) == LCK_UNLOCK)
		_vg_lock_count--;
	else
		_vg_lock_count++;

	/* We don't bother to reset this until all VG locks are dropped */
	if ((flags & LCK_TYPE_MASK) == LCK_WRITE)
		_vg_write_lock_held = 1;
	else if (!_vg_lock_count)
		_vg_write_lock_held = 0;
}

/*
 * Select a locking type
 * type: locking type; if < 0, then read config tree value
 */
int init_locking(int type, struct cmd_context *cmd, int suppress_messages)
{
	if (getenv("LVM_SUPPRESS_LOCKING_FAILURE_MESSAGES"))
		suppress_messages = 1;

	if (type < 0)
		type = find_config_tree_int(cmd, global_locking_type_CFG, NULL);

	_blocking_supported = find_config_tree_bool(cmd, global_wait_for_locks_CFG, NULL);

	if (type != 1)
		log_warn("WARNING: locking_type deprecated, using file locking.");

	if (type == 3)
		log_warn("WARNING: See lvmlockd(8) for information on using cluster/clvm VGs.");

	log_very_verbose("%sFile-based locking selected.", _blocking_supported ? "" : "Non-blocking ");

	if (!init_file_locking(&_locking, cmd, suppress_messages))
		log_error_suppress(suppress_messages, "File-based locking initialisation failed.");

	return 1;
}

void fin_locking(void)
{
	_locking.fin_locking();
}

/*
 * VG locking is by VG name.
 * FIXME This should become VG uuid.
 */
static int _lock_vol(struct cmd_context *cmd, const char *resource, uint32_t flags)
{
	uint32_t lck_type = flags & LCK_TYPE_MASK;
	uint32_t lck_scope = flags & LCK_SCOPE_MASK;
	int ret = 0;

	block_signals(flags);

	assert(resource);

	if (!*resource) {
		log_error(INTERNAL_ERROR "Use of P_orphans is deprecated.");
		goto out;
	}

	if ((is_orphan_vg(resource) || is_global_vg(resource)) && (flags & LCK_CACHE)) {
		log_error(INTERNAL_ERROR "P_%s referenced.", resource);
		goto out;
	}

	if (cmd->metadata_read_only && lck_type == LCK_WRITE &&
	    strcmp(resource, VG_GLOBAL)) {
		log_error("Operation prohibited while global/metadata_read_only is set.");
		goto out;
	}

	if ((ret = _locking.lock_resource(cmd, resource, flags, NULL))) {
		if (lck_scope == LCK_VG && !(flags & LCK_CACHE)) {
			if (lck_type != LCK_UNLOCK)
				lvmcache_lock_vgname(resource, lck_type == LCK_READ);
			dev_reset_error_count(cmd);
		}

		_update_vg_lock_count(resource, flags);
	} else
		stack;

	/* If unlocking, always remove lock from lvmcache even if operation failed. */
	if (lck_scope == LCK_VG && !(flags & LCK_CACHE) && lck_type == LCK_UNLOCK) {
		lvmcache_unlock_vgname(resource);
		if (!ret)
			_update_vg_lock_count(resource, flags);
	}
out:
	_unblock_signals();

	return ret;
}

int lock_vol(struct cmd_context *cmd, const char *vol, uint32_t flags, const struct logical_volume *lv)
{
	char resource[258] __attribute__((aligned(8)));
	int lck_type = flags & LCK_TYPE_MASK;

	if (flags == LCK_NONE) {
		log_debug_locking(INTERNAL_ERROR "%s: LCK_NONE lock requested", vol);
		return 1;
	}

	switch (flags & LCK_SCOPE_MASK) {
	case LCK_VG:
		if (!_blocking_supported)
			flags |= LCK_NONBLOCK;

		/* Global VG_ORPHANS lock covers all orphan formats. */
		if (is_orphan_vg(vol))
			vol = VG_ORPHANS;
		break;
	default:
		log_error("Unrecognised lock scope: %d",
			  flags & LCK_SCOPE_MASK);
		return 0;
	}

	if (!dm_strncpy(resource, vol, sizeof(resource))) {
		log_error(INTERNAL_ERROR "Resource name %s is too long.", vol);
		return 0;
	}

	if (!_lock_vol(cmd, resource, flags))
		return_0;

	/*
	 * If a real lock was acquired (i.e. not LCK_CACHE),
	 * perform an immediate unlock unless LCK_HOLD was requested.
	 */
	if ((lck_type == LCK_NULL) || (lck_type == LCK_UNLOCK) ||
	    (flags & (LCK_CACHE | LCK_HOLD)))
		return 1;

	if (!_lock_vol(cmd, resource, (flags & ~LCK_TYPE_MASK) | LCK_UNLOCK))
		return_0;

	return 1;
}

/* Lock a list of LVs */
int activate_lvs(struct cmd_context *cmd, struct dm_list *lvs, unsigned exclusive)
{
	struct dm_list *lvh;
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, lvs) {
		if (!activate_lv(cmd, lvl->lv)) {
			log_error("Failed to activate %s", display_lvname(lvl->lv));

			dm_list_uniterate(lvh, lvs, &lvl->list) {
				lvl = dm_list_item(lvh, struct lv_list);
				if (!deactivate_lv(cmd, lvl->lv))
					stack;
			}
			return 0;
		}
	}

	return 1;
}

int vg_write_lock_held(void)
{
	return _vg_write_lock_held;
}

int locking_is_clustered(void)
{
	return (_locking.flags & LCK_CLUSTERED) ? 1 : 0;
}

int sync_local_dev_names(struct cmd_context* cmd)
{
	memlock_unlock(cmd);

	return lock_vol(cmd, VG_SYNC_NAMES, LCK_VG_SYNC_LOCAL, NULL);
}

int sync_dev_names(struct cmd_context* cmd)
{
	memlock_unlock(cmd);

	return lock_vol(cmd, VG_SYNC_NAMES, LCK_VG_SYNC, NULL);
}

