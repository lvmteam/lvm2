/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "log.h"
#include "locking.h"
#include "locking_types.h"
#include "activate.h"
#include "config.h"
#include "defaults.h"
#include "lvm-file.h"
#include "lvm-string.h"
#include "dbg_malloc.h"

#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <signal.h>

struct lock_list {
	struct list list;
	int lf;
	char *res;
};

static struct list _lock_list;
static char _lock_dir[NAME_LEN];

static int _release_lock(const char *file)
{
	struct lock_list *ll;
	struct list *llh, *llt;

	list_iterate_safe(llh, llt, &_lock_list) {
		ll = list_item(llh, struct lock_list);

		if (!file || !strcmp(ll->res, file)) {
			list_del(llh);
			log_very_verbose("Unlocking %s", ll->res);

			/* 
			 * If this is the last pid using the file, remove it 
			 */
			if (!flock(ll->lf, LOCK_NB | LOCK_EX))
				if (unlink(ll->res))
					log_sys_error("unlink", ll->res);

			if (close(ll->lf) < 0)
				log_sys_error("close", ll->res);

			dbg_free(ll->res);
			dbg_free(llh);

			if (file)
				return 1;
		}
	}

	return 0;
}

void fin_file_locking(void)
{
	_release_lock(NULL);
}

void _trap_ctrl_c(int signal)
{
	log_error("CTRL-c detected: giving up waiting for lock");
	return;
}

static void _install_ctrl_c_handler()
{
	siginterrupt(SIGINT, 1);
	signal(SIGINT, _trap_ctrl_c);
}

static void _remove_ctrl_c_handler()
{
	signal(SIGINT, SIG_IGN);
	siginterrupt(SIGINT, 0);
}

static int _lock_file(const char *file, int flags)
{
	int operation;
	int r = 1;

	struct lock_list *ll;

	switch (flags & LCK_TYPE_MASK) {
	case LCK_READ:
		operation = LOCK_SH;
		break;
	case LCK_WRITE:
		operation = LOCK_EX;
		break;
	case LCK_NONE:
		return _release_lock(file);
	default:
		log_error("Unrecognised lock type: %d", flags & LCK_TYPE_MASK);
		return 0;
	}

	if (!(ll = dbg_malloc(sizeof(struct lock_list))))
		return 0;

	if (!(ll->res = dbg_strdup(file))) {
		dbg_free(ll);
		return 0;
	}

	log_very_verbose("Locking %s", ll->res);
	if ((ll->lf = open(file, O_CREAT | O_APPEND | O_RDWR, 0777)) < 0) {
		log_sys_error("open", file);
		dbg_free(ll->res);
		dbg_free(ll);
		return 0;
	}

	if ((flags & LCK_NONBLOCK))
		operation |= LOCK_NB;
	else
		_install_ctrl_c_handler();

	if (flock(ll->lf, operation)) {
		log_sys_error("flock", ll->res);
		dbg_free(ll->res);
		dbg_free(ll);
		r = 0;
	} else
		list_add(&_lock_list, &ll->list);


	if (!(flags & LCK_NONBLOCK))
		_remove_ctrl_c_handler();

	return r;
}

int lock_resource(const char *resource, int flags)
{
	char lockfile[PATH_MAX];

	switch (flags & LCK_SCOPE_MASK) {
	case LCK_VG:
		if (!resource || !*resource)
			lvm_snprintf(lockfile, sizeof(lockfile),
				     "%s/P_orphans", _lock_dir);
		else
			lvm_snprintf(lockfile, sizeof(lockfile),
				     "%s/V_%s", _lock_dir, resource);
		break;
	case LCK_LV:
		/* No-op: see FIXME below */
		return 1;
	default:
		log_error("Unrecognised lock scope: %d",
			  flags & LCK_SCOPE_MASK);
		return 0;
	}

	if (!_lock_file(lockfile, flags))
		return 0;

	return 1;
}

/****** FIXME  This is stuck a layer above until activate unit 
		can take labels and read its own metadata

	if ((flags & LCK_SCOPE_MASK) == LCK_LV)	{
		switch (flags & LCK_TYPE_MASK) {
		case LCK_NONE:
			if (lv_active_by_id(resource))
				lv_resume_by_id(resource);
			break;
		case LCK_WRITE:
			if (lv_active_by_id(resource))
				lv_suspend_by_id(resource);
			break;
		default:
			break;
		}
	}
*******/

int init_file_locking(struct locking_type *locking, struct config_file *cf)
{
	locking->lock_resource = lock_resource;
	locking->fin_locking = fin_file_locking;

	/* Get lockfile directory from config file */
	strncpy(_lock_dir, find_config_str(cf->root, "global/locking_dir",
					   '/', DEFAULT_LOCK_DIR),
		sizeof(_lock_dir));

	if (!create_dir(_lock_dir))
		return 0;

	list_init(&_lock_list);

	return 1;
}
