/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "metadata.h"
#include "uuid.h"
#include "config.h"

int init_locking(int type, struct config_file *cf);
void fin_locking(void);

/*
 * LCK_VG:
 *   Lock/unlock on-disk volume group data
 *   Use "" to lock orphan PVs
 *   char *vol holds volume group name
 *
 * LCK_LV:
 *   Lock/unlock an individual logical volume
 *   Also suspends/resumes the LV if it's active.
 *   char *vol holds "VG_name/LV_uuid"
 *
 * FIXME: Change to something like
 *   int lock_vol(struct cmd_context *cmd, const struct id *id, int flags);
 */
int lock_vol(struct cmd_context *cmd, const char *vol, int flags);

/*
 * Lock type
 */
#define LCK_TYPE_MASK	0x000000FF
#define LCK_NONE	0x00000000
#define LCK_READ	0x00000001
#define LCK_WRITE	0x00000002

/*
 * Lock scope
 */
#define LCK_SCOPE_MASK	0x0000FF00
#define LCK_VG		0x00000000
#define LCK_LV		0x00000100

/*
 * Lock bits
 */
#define LCK_NONBLOCK	0x00010000

