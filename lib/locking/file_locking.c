/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "lib.h"
#include "locking.h"
#include "locking_types.h"
#include "activate.h"
#include "config.h"
#include "defaults.h"
#include "lvm-file.h"
#include "lvm-string.h"

#include <limits.h>
#include <unistd.h>
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

static sig_t _oldhandler;
static sigset_t _fullsigset, _intsigset;
static int _handler_installed;

static int _release_lock(const char *file)
{
	struct lock_list *ll;
	struct list *llh, *llt;

	struct stat buf1, buf2;

	list_iterate_safe(llh, llt, &_lock_list) {
		ll = list_item(llh, struct lock_list);

		if (!file || !strcmp(ll->res, file)) {
			list_del(llh);
			log_very_verbose("Unlocking %s", ll->res);

			if (flock(ll->lf, LOCK_NB | LOCK_UN))
				log_sys_error("flock", ll->res);

			if (!flock(ll->lf, LOCK_NB | LOCK_EX) &&
			    !stat(ll->res, &buf1) &&
			    !fstat(ll->lf, &buf2) &&
			    !memcmp(&buf1.st_ino, &buf2.st_ino, sizeof(ino_t)))
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

static void _remove_ctrl_c_handler()
{
	siginterrupt(SIGINT, 0);
	if (!_handler_installed || _oldhandler == SIG_ERR)
		return;

	sigprocmask(SIG_SETMASK, &_fullsigset, NULL);
	if (signal(SIGINT, _oldhandler) == SIG_ERR)
		log_sys_error("signal", "_remove_ctrl_c_handler");

	_handler_installed = 0;
}

void _trap_ctrl_c(int signal)
{
	_remove_ctrl_c_handler();
	log_error("CTRL-c detected: giving up waiting for lock");
	return;
}

static void _install_ctrl_c_handler()
{
	if ((_oldhandler = signal(SIGINT, _trap_ctrl_c)) == SIG_ERR)
		return;

	sigprocmask(SIG_SETMASK, &_intsigset, NULL);
	siginterrupt(SIGINT, 1);

	_handler_installed = 1;
}

static int _lock_file(const char *file, int flags)
{
	int operation;
	int r = 1;

	struct lock_list *ll;
	struct stat buf1, buf2;
	char state;

	switch (flags & LCK_TYPE_MASK) {
	case LCK_READ:
		operation = LOCK_SH;
		state = 'R';
		break;
	case LCK_WRITE:
		operation = LOCK_EX;
		state = 'W';
		break;
	case LCK_UNLOCK:
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

	ll->lf = -1;

	log_very_verbose("Locking %s %c%c", ll->res, state,
			 flags & LCK_NONBLOCK ? ' ' : 'B');
	do {
		if (ll->lf > -1)
			close(ll->lf);

		if ((ll->lf = open(file, O_CREAT | O_APPEND | O_RDWR, 0777))
		    < 0) {
			log_sys_error("open", file);
			goto err;
		}

		if ((flags & LCK_NONBLOCK))
			operation |= LOCK_NB;
		else
			_install_ctrl_c_handler();

		r = flock(ll->lf, operation);
		if (!(flags & LCK_NONBLOCK))
			_remove_ctrl_c_handler();

		if (r) {
			log_sys_error("flock", ll->res);
			goto err;
		}

		if (!stat(ll->res, &buf1) && !fstat(ll->lf, &buf2) &&
		    !memcmp(&buf1.st_ino, &buf2.st_ino, sizeof(ino_t)))
			break;
	} while (!(flags & LCK_NONBLOCK));

	list_add(&_lock_list, &ll->list);
	return 1;

      err:
	dbg_free(ll->res);
	dbg_free(ll);
	return 0;
}

int file_lock_resource(struct cmd_context *cmd, const char *resource, int flags)
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
		if (!_lock_file(lockfile, flags))
			return 0;
		break;
	case LCK_LV:
		switch (flags & LCK_TYPE_MASK) {
		case LCK_UNLOCK:
			if (!lv_resume_if_active(cmd, resource))
				return 0;
			break;
		case LCK_READ:
			if (!lv_activate(cmd, resource))
				return 0;
			break;
		case LCK_WRITE:
			if (!lv_suspend_if_active(cmd, resource))
				return 0;
			break;
		case LCK_EXCL:
			if (!lv_deactivate(cmd, resource))
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

int init_file_locking(struct locking_type *locking, struct config_tree *cf)
{
	locking->lock_resource = file_lock_resource;
	locking->fin_locking = fin_file_locking;

	/* Get lockfile directory from config file */
	strncpy(_lock_dir, find_config_str(cf->root, "global/locking_dir",
					   '/', DEFAULT_LOCK_DIR),
		sizeof(_lock_dir));

	if (!create_dir(_lock_dir))
		return 0;

	/* Trap a read-only file system */
	if ((access(_lock_dir, R_OK | W_OK | X_OK) == -1) && (errno == EROFS))
		return 0;

	list_init(&_lock_list);

	if (sigfillset(&_intsigset) || sigfillset(&_fullsigset)) {
		log_sys_error("sigfillset", "init_file_locking");
		return 0;
	}

	if (sigdelset(&_intsigset, SIGINT)) {
		log_sys_error("sigdelset", "init_file_locking");
		return 0;
	}

	return 1;
}
