/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "log.h"
#include "locking.h"
#include "locking_types.h"
#include "lvm-string.h"
#include "activate.h"

#include <signal.h>

/*
 * No locking
 */

static void _no_fin_locking(void)
{
	return;
}

static int _no_lock_resource(struct cmd_context *cmd, const char *resource,
			     int flags)
{
	switch (flags & LCK_SCOPE_MASK) {
	case LCK_VG:
		break;
	case LCK_LV:
		switch (flags & LCK_TYPE_MASK) {
		case LCK_UNLOCK:
			return lv_resume_if_active(cmd, resource);
		case LCK_READ:
			return lv_activate(cmd, resource);
		case LCK_WRITE:
			return lv_suspend_if_active(cmd, resource);
		case LCK_EXCL:
			return lv_deactivate(cmd, resource);
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

int init_no_locking(struct locking_type *locking, struct config_file *cf)
{
	locking->lock_resource = _no_lock_resource;
	locking->fin_locking = _no_fin_locking;

	return 1;
}
