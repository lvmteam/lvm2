/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
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

#ifndef _LVM_LOCKING_H
#define _LVM_LOCKING_H

#include "uuid.h"
#include "config.h"

int init_locking(int type, struct cmd_context *cmd);
void fin_locking(void);
void reset_locking(void);
int vg_write_lock_held(void);
int locking_is_clustered(void);

/*
 * LCK_VG:
 *   Lock/unlock on-disk volume group data
 *   Use "" to lock orphan PVs
 *   char *vol holds volume group name
 *
 * LCK_LV:
 *   Lock/unlock an individual logical volume
 *   char *vol holds lvid
 */
int lock_vol(struct cmd_context *cmd, const char *vol, int flags);

/*
 * Does the LVM1 driver have this VG active?
 */
int check_lvm1_vg_inactive(struct cmd_context *cmd, const char *vgname);

/*
 * Lock type - these numbers are the same as VMS and the IBM DLM
 */
#define LCK_TYPE_MASK	0x00000007

#define LCK_NULL	0x00000000	/* LCK$_NLMODE */
#define LCK_READ	0x00000001	/* LCK$_CRMODE */
					/* LCK$_CWMODE */
#define LCK_PREAD       0x00000003      /* LCK$_PRMODE */
#define LCK_WRITE	0x00000004	/* LCK$_PWMODE */
#define LCK_EXCL	0x00000005	/* LCK$_EXMODE */
#define LCK_UNLOCK      0x00000006	/* This is ours */

/*
 * Lock scope
 */
#define LCK_SCOPE_MASK	0x00000008
#define LCK_VG		0x00000000
#define LCK_LV		0x00000008

/*
 * Lock bits
 */
#define LCK_NONBLOCK	0x00000010	/* Don't block waiting for lock? */
#define LCK_HOLD	0x00000020	/* Hold lock when lock_vol returns? */
#define LCK_LOCAL	0x00000040	/* Don't propagate to other nodes */
#define LCK_CLUSTER_VG	0x00000080	/* VG is clustered */

/*
 * Additional lock bits for cluster communication
 */
#define LCK_PARTIAL_MODE	0x00000001	/* Running in partial mode */
#define LCK_MIRROR_NOSYNC_MODE	0x00000002	/* Mirrors don't require sync */
#define LCK_DMEVENTD_MONITOR_MODE	0x00000004	/* Register with dmeventd */


/*
 * Common combinations
 */
#define LCK_VG_READ		(LCK_VG | LCK_READ | LCK_HOLD)
#define LCK_VG_WRITE		(LCK_VG | LCK_WRITE | LCK_HOLD)
#define LCK_VG_UNLOCK		(LCK_VG | LCK_UNLOCK)

#define LCK_LV_EXCLUSIVE	(LCK_LV | LCK_EXCL | LCK_NONBLOCK)
#define LCK_LV_SUSPEND		(LCK_LV | LCK_WRITE | LCK_NONBLOCK)
#define LCK_LV_RESUME		(LCK_LV | LCK_UNLOCK | LCK_NONBLOCK)
#define LCK_LV_ACTIVATE		(LCK_LV | LCK_READ | LCK_NONBLOCK)
#define LCK_LV_DEACTIVATE	(LCK_LV | LCK_NULL | LCK_NONBLOCK)

#define LCK_LV_CLUSTERED(lv)	\
	(((lv)->vg->status & CLUSTERED) ? LCK_CLUSTER_VG : 0)

#define lock_lv_vol(cmd, lv, flags)	\
	lock_vol(cmd, (lv)->lvid.s, flags | LCK_LV_CLUSTERED(lv))

#define unlock_vg(cmd, vol)	lock_vol(cmd, vol, LCK_VG_UNLOCK)

#define resume_lv(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_RESUME)
#define suspend_lv(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_SUSPEND | LCK_HOLD)
#define deactivate_lv(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_DEACTIVATE)
#define activate_lv(cmd, lv)	lock_lv_vol(cmd, lv, LCK_LV_ACTIVATE | LCK_HOLD)
#define activate_lv_excl(cmd, lv)	\
				lock_lv_vol(cmd, lv, LCK_LV_EXCLUSIVE | LCK_HOLD)
#define activate_lv_local(cmd, lv)	\
	lock_lv_vol(cmd, lv, LCK_LV_ACTIVATE | LCK_HOLD | LCK_LOCAL)
#define deactivate_lv_local(cmd, lv)	\
	lock_lv_vol(cmd, lv, LCK_LV_DEACTIVATE | LCK_LOCAL)

/* Process list of LVs */
int suspend_lvs(struct cmd_context *cmd, struct list *lvs);
int resume_lvs(struct cmd_context *cmd, struct list *lvs);
int activate_lvs_excl(struct cmd_context *cmd, struct list *lvs);

/* Interrupt handling */
void sigint_clear(void);
void sigint_allow(void);
void sigint_restore(void);
int sigint_caught(void);

#endif
