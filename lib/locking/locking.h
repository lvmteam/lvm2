/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "metadata.h"
#include "uuid.h"
#include "config.h"

int init_locking(int type, struct config_tree *cf);
void fin_locking(void);
void reset_locking(void);
int vg_write_lock_held(void);

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
					/* LCK$_PRMODE */
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

/*
 * Common combinations
 */
#define LCK_VG_READ		(LCK_VG | LCK_READ | LCK_HOLD)
#define LCK_VG_WRITE		(LCK_VG | LCK_WRITE | LCK_HOLD)
#define LCK_VG_UNLOCK		(LCK_VG | LCK_UNLOCK)

#define LCK_LV_DEACTIVATE	(LCK_LV | LCK_EXCL)
#define LCK_LV_SUSPEND		(LCK_LV | LCK_WRITE)
#define LCK_LV_ACTIVATE		(LCK_LV | LCK_READ)
#define LCK_LV_UNLOCK		(LCK_LV | LCK_UNLOCK)

#define unlock_lv(cmd, vol)	lock_vol(cmd, vol, LCK_LV_UNLOCK)
#define unlock_vg(cmd, vol)	lock_vol(cmd, vol, LCK_VG_UNLOCK)

/* Process list of LVs */
int lock_lvs(struct cmd_context *cmd, struct list *lvs, int flags);
int unlock_lvs(struct cmd_context *cmd, struct list *lvs);

