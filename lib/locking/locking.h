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

#ifndef _LVM_LOCKING_H
#define _LVM_LOCKING_H

#include "lib/uuid/uuid.h"
#include "lib/config/config.h"

struct logical_volume;

int init_locking(struct cmd_context *cmd, int file_locking_sysinit, int file_locking_readonly, int file_locking_ignorefail);
void fin_locking(void);
void reset_locking(void);
int vg_write_lock_held(void);

/*
 * LCK_VG:
 *   Lock/unlock on-disk volume group data.
 *   Use VG_ORPHANS to lock all orphan PVs.
 *   Use VG_GLOBAL as a global lock and to wipe the internal cache.
 *   char *vol holds volume group name.
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
int lock_vol(struct cmd_context *cmd, const char *vol, uint32_t flags, const struct logical_volume *lv);

/*
 * Internal locking representation.
 *   LCK_VG: Uses prefix V_ unless the vol begins with # (i.e. #global or #orphans)
 */

/*
 * Lock type - these numbers are the same as VMS and the IBM DLM
 */
#define LCK_TYPE_MASK	0x00000007U

#define LCK_READ	0x00000001U	/* LCK$_CRMODE (Activate) */
#define LCK_WRITE	0x00000004U	/* LCK$_PWMODE (Suspend) */
#define LCK_UNLOCK      0x00000006U	/* This is ours (Resume) */

/*
 * Lock scope
 */
#define LCK_SCOPE_MASK	0x00001008U
#define LCK_VG		0x00000000U	/* Volume Group */

/*
 * Lock bits.
 * Bottom 8 bits except LCK_LOCAL form args[0] in cluster comms.
 */
#define LCK_NONBLOCK	0x00000010U	/* Don't block waiting for lock? */
#define LCK_HOLD	0x00000020U	/* Hold lock when lock_vol returns? */

/*
 * Special cases of VG locks.
 */
#define VG_ORPHANS	"#orphans"
#define VG_GLOBAL	"#global"

/*
 * Common combinations
 */
#define LCK_VG_READ		(LCK_VG | LCK_READ | LCK_HOLD)
#define LCK_VG_WRITE		(LCK_VG | LCK_WRITE | LCK_HOLD)
#define LCK_VG_UNLOCK		(LCK_VG | LCK_UNLOCK)

#define LCK_MASK (LCK_TYPE_MASK | LCK_SCOPE_MASK)

#define unlock_vg(cmd, vg, vol)	\
	do { \
		if (vg && !lvmetad_vg_update_finish(vg)) \
			stack; \
		if (is_real_vg(vol) && !sync_local_dev_names(cmd)) \
			stack; \
		if (!lock_vol(cmd, vol, LCK_VG_UNLOCK, NULL)) \
			stack;	\
	} while (0)
#define unlock_and_release_vg(cmd, vg, vol) \
	do { \
		unlock_vg(cmd, vg, vol); \
		release_vg(vg); \
	} while (0)

int sync_local_dev_names(struct cmd_context* cmd);

/* Process list of LVs */
struct volume_group;
int activate_lvs(struct cmd_context *cmd, struct dm_list *lvs, unsigned exclusive);

#endif
