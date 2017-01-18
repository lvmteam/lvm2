/*
 * Copyright (C) 2010-2015 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "dmeventd_lvm.h"
#include "libdevmapper-event.h"
#include "lvm2cmd.h"

#include <pthread.h>

/*
 * register_device() is called first and performs initialisation.
 * Only one device may be registered or unregistered at a time.
 */
static pthread_mutex_t _register_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Number of active registrations.
 */
static int _register_count = 0;
static struct dm_pool *_mem_pool = NULL;
static void *_lvm_handle = NULL;

DM_EVENT_LOG_FN("#lvm")

static void _lvm2_print_log(int level, const char *file, int line,
			    int dm_errno_or_class, const char *msg)
{
	print_log(level, file, line, dm_errno_or_class, "%s", msg);
}

/*
 * Currently only one event can be processed at a time.
 */
static pthread_mutex_t _event_mutex = PTHREAD_MUTEX_INITIALIZER;

void dmeventd_lvm2_lock(void)
{
	pthread_mutex_lock(&_event_mutex);
}

void dmeventd_lvm2_unlock(void)
{
	pthread_mutex_unlock(&_event_mutex);
}

int dmeventd_lvm2_init(void)
{
	int r = 0;

	pthread_mutex_lock(&_register_mutex);

	if (!_lvm_handle) {
		lvm2_log_fn(_lvm2_print_log);

		if (!(_lvm_handle = lvm2_init()))
			goto out;

		/*
		 * Need some space for allocations.  1024 should be more
		 * than enough for what we need (device mapper name splitting)
		 */
		if (!_mem_pool && !(_mem_pool = dm_pool_create("mirror_dso", 1024))) {
			lvm2_exit(_lvm_handle);
			_lvm_handle = NULL;
			goto out;
		}

		lvm2_disable_dmeventd_monitoring(_lvm_handle);
		/* FIXME Temporary: move to dmeventd core */
		lvm2_run(_lvm_handle, "_memlock_inc");
		log_debug("lvm plugin initilized.");
	}

	_register_count++;
	r = 1;

out:
	pthread_mutex_unlock(&_register_mutex);
	return r;
}

void dmeventd_lvm2_exit(void)
{
	pthread_mutex_lock(&_register_mutex);

	if (!--_register_count) {
		log_debug("lvm plugin shuting down.");
		lvm2_run(_lvm_handle, "_memlock_dec");
		dm_pool_destroy(_mem_pool);
		_mem_pool = NULL;
		lvm2_exit(_lvm_handle);
		_lvm_handle = NULL;
		log_debug("lvm plugin exited.");
	}

	pthread_mutex_unlock(&_register_mutex);
}

struct dm_pool *dmeventd_lvm2_pool(void)
{
	return _mem_pool;
}

int dmeventd_lvm2_run(const char *cmdline)
{
	return (lvm2_run(_lvm_handle, cmdline) == LVM2_COMMAND_SUCCEEDED);
}

int dmeventd_lvm2_command(struct dm_pool *mem, char *buffer, size_t size,
			  const char *cmd, const char *device)
{
	static char _internal_prefix[] =  "_dmeventd_";
	char *vg = NULL, *lv = NULL, *layer;
	int r;

	if (!dm_split_lvm_name(mem, device, &vg, &lv, &layer)) {
		log_error("Unable to determine VG name from %s.",
			  device);
		return 0;
	}

	/* strip off the mirror component designations */
	if ((layer = strstr(lv, "_mimagetmp")) ||
	    (layer = strstr(lv, "_mlog")))
		*layer = '\0';

	if (!strncmp(cmd, _internal_prefix, sizeof(_internal_prefix) - 1)) {
		dmeventd_lvm2_lock();
		/* output of internal command passed via env var */
		if (!dmeventd_lvm2_run(cmd))
			cmd = NULL;
		else if ((cmd = getenv(cmd)))
			cmd = dm_pool_strdup(mem, cmd); /* copy with lock */
		dmeventd_lvm2_unlock();

		if (!cmd) {
			log_error("Unable to find configured command.");
			return 0;
		}
	}

	r = dm_snprintf(buffer, size, "%s %s/%s", cmd, vg, lv);

	dm_pool_free(mem, vg);

	if (r < 0) {
		log_error("Unable to form LVM command. (too long).");
		return 0;
	}

	return 1;
}
