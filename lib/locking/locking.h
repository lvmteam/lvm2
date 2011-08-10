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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_LOCKING_H
#define _LVM_LOCKING_H

#include "uuid.h"
#include "config.h"

int init_locking(int type, struct cmd_context *cmd, int suppress_messages);
void fin_locking(void);
void reset_locking(void);
int vg_write_lock_held(void);
int locking_is_clustered(void);

int remote_lock_held(const char *vol, int *exclusive);

/*
 * LCK_VG:
 *   Lock/unlock on-disk volume group data.
 *   Use VG_ORPHANS to lock all orphan PVs.
 *   Use VG_GLOBAL as a global lock and to wipe the internal cache.
 *   char *vol holds volume group name.
 *   Set LCK_CACHE flag when manipulating 'vol' metadata in the internal cache.
 *   (Like commit, revert or invalidate metadata.)
 *   If more than one lock needs to be held simultaneously, they must be
 *   acquired in alphabetical order of 'vol' (to avoid deadlocks), with
 *   VG_ORPHANS last.
 *
 *   Use VG_SYNC_NAMES to ensure /dev is up-to-date for example, with udev,
 *   by waiting for any asynchronous events issued to have completed.
 *
 * LCK_LV:
 *   Lock/unlock an individual logical volume
 *   char *vol holds lvid
 */
int lock_vol(struct cmd_context *cmd, const char *vol, uint32_t flags);

/*
 * Internal locking representation.
 *   LCK_VG: Uses prefix V_ unless the vol begins with # (i.e. #global or #orphans)
 *           or the LCK_CACHE flag is set when it uses the prefix P_.
 * If LCK_CACHE is set, we do not take out a real lock.
 * NB In clustered situations, LCK_CACHE is not propagated directly to remote nodes.
 * (It can be deduced from lock name.)
 */

/*
 * Does the LVM1 driver have this VG active?
 */
int check_lvm1_vg_inactive(struct cmd_context *cmd, const char *vgname);

/*
 * Lock type - these numbers are the same as VMS and the IBM DLM
 */
#define LCK_TYPE_MASK	0x00000007U

#define LCK_NULL	0x00000000U	/* LCK$_NLMODE */
#define LCK_READ	0x00000001U	/* LCK$_CRMODE */
					/* LCK$_CWMODE */
#define LCK_PREAD       0x00000003U	/* LCK$_PRMODE */
#define LCK_WRITE	0x00000004U	/* LCK$_PWMODE */
#define LCK_EXCL	0x00000005U	/* LCK$_EXMODE */
#define LCK_UNLOCK      0x00000006U	/* This is ours */

/*
 * Lock flags - these numbers are the same as DLM
 */
#define LCKF_NOQUEUE	0x00000001U	/* LKF$_NOQUEUE */
#define LCKF_CONVERT	0x00000004U	/* LKF$_CONVERT */

/*
 * Lock scope
 */
#define LCK_SCOPE_MASK	0x00000008U
#define LCK_VG		0x00000000U
#define LCK_LV		0x00000008U

/*
 * Lock bits
 */
#define LCK_NONBLOCK	0x00000010U	/* Don't block waiting for lock? */
#define LCK_HOLD	0x00000020U	/* Hold lock when lock_vol returns? */
#define LCK_LOCAL	0x00000040U	/* Don't propagate to other nodes */
#define LCK_CLUSTER_VG	0x00000080U	/* VG is clustered */
#define LCK_CACHE	0x00000100U	/* Operation on cache only using P_ lock */
#define LCK_ORIGIN_ONLY	0x00000200U	/* Operation should bypass any snapshots */

/*
 * Additional lock bits for cluster communication via args[1]
 */
#define LCK_PARTIAL_MODE        	0x01	/* Partial activation? */
#define LCK_MIRROR_NOSYNC_MODE		0x02	/* Mirrors don't require sync */
#define LCK_DMEVENTD_MONITOR_MODE	0x04	/* Register with dmeventd */
#define LCK_CONVERT			0x08	/* Convert existing lock */
#define LCK_ORIGIN_ONLY_MODE		0x20	/* Same as above */
#define LCK_TEST_MODE			0x10    /* Test mode: No activation */

/*
 * Special cases of VG locks.
 */
#define VG_ORPHANS	"#orphans"
#define VG_GLOBAL	"#global"
#define VG_SYNC_NAMES	"#sync_names"

/*
 * Common combinations
 */
#define LCK_NONE		(LCK_VG | LCK_NULL)

#define LCK_VG_READ		(LCK_VG | LCK_READ | LCK_HOLD)
#define LCK_VG_WRITE		(LCK_VG | LCK_WRITE | LCK_HOLD)
#define LCK_VG_UNLOCK		(LCK_VG | LCK_UNLOCK)
#define LCK_VG_DROP_CACHE	(LCK_VG | LCK_WRITE | LCK_CACHE)

/* FIXME: LCK_HOLD abused here */
#define LCK_VG_COMMIT		(LCK_VG | LCK_WRITE | LCK_CACHE | LCK_HOLD)
#define LCK_VG_REVERT		(LCK_VG | LCK_READ  | LCK_CACHE | LCK_HOLD)

#define LCK_VG_BACKUP		(LCK_VG | LCK_CACHE)

#define LCK_VG_SYNC		(LCK_NONE | LCK_CACHE)
#define LCK_VG_SYNC_LOCAL	(LCK_NONE | LCK_CACHE | LCK_LOCAL)

#define LCK_LV_EXCLUSIVE	(LCK_LV | LCK_EXCL)
#define LCK_LV_SUSPEND		(LCK_LV | LCK_WRITE)
#define LCK_LV_RESUME		(LCK_LV | LCK_UNLOCK)
#define LCK_LV_ACTIVATE		(LCK_LV | LCK_READ)
#define LCK_LV_DEACTIVATE	(LCK_LV | LCK_NULL)

#define LCK_MASK (LCK_TYPE_MASK | LCK_SCOPE_MASK)

#define LCK_LV_CLUSTERED(lv)	\
	(vg_is_clustered((lv)->vg) ? LCK_CLUSTER_VG : 0)

#define lock_lv_vol(cmd, lv, flags)	\
	(find_replicator_vgs((lv)) ? \
		lock_vol(cmd, (lv)->lvid.s, flags | LCK_LV_CLUSTERED(lv)) : \
		0)

#define unlock_vg(cmd, vol)	\
	do { \
		if (is_real_vg(vol)) \
			sync_dev_names(cmd); \
		lock_vol(cmd, vol, LCK_VG_UNLOCK); \
	} while (0)
#define unlock_and_release_vg(cmd, vg, vol) \
	do { \
		unlock_vg(cmd, vol); \
		release_vg(vg); \
	} while (0)

#define resume_lv(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_RESUME)
#define resume_lv_origin(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_RESUME | LCK_ORIGIN_ONLY)
#define suspend_lv(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_SUSPEND | LCK_HOLD)
#define suspend_lv_origin(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_SUSPEND | LCK_HOLD | LCK_ORIGIN_ONLY)
#define deactivate_lv(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_DEACTIVATE)
#define activate_lv(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_ACTIVATE | LCK_HOLD)
#define activate_lv_excl(cmd, lv)	\
				lock_lv_vol(cmd, lv, LCK_LV_EXCLUSIVE | LCK_HOLD)
#define activate_lv_local(cmd, lv)	\
	lock_lv_vol(cmd, lv, LCK_LV_ACTIVATE | LCK_HOLD | LCK_LOCAL)
#define deactivate_lv_local(cmd, lv)	\
	lock_lv_vol(cmd, lv, LCK_LV_DEACTIVATE | LCK_LOCAL)
#define drop_cached_metadata(vg)	\
	lock_vol((vg)->cmd, (vg)->name, LCK_VG_DROP_CACHE)
#define remote_commit_cached_metadata(vg)	\
	lock_vol((vg)->cmd, (vg)->name, LCK_VG_COMMIT)
#define remote_revert_cached_metadata(vg)	\
	lock_vol((vg)->cmd, (vg)->name, LCK_VG_REVERT)
#define remote_backup_metadata(vg)	\
	lock_vol((vg)->cmd, (vg)->name, LCK_VG_BACKUP)

int sync_local_dev_names(struct cmd_context* cmd);
int sync_dev_names(struct cmd_context* cmd);

/* Process list of LVs */
int suspend_lvs(struct cmd_context *cmd, struct dm_list *lvs);
int resume_lvs(struct cmd_context *cmd, struct dm_list *lvs);
int activate_lvs(struct cmd_context *cmd, struct dm_list *lvs, unsigned exclusive);

/* Interrupt handling */
void sigint_clear(void);
void sigint_allow(void);
void sigint_restore(void);
int sigint_caught(void);

#endif
