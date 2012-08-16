/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "log.h"

#include "lvm2cmd.h"
#include "dmeventd_lvm.h"

#include <pthread.h>
#include <syslog.h>

extern int dmeventd_debug;

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

/*
 * Currently only one event can be processed at a time.
 */
static pthread_mutex_t _event_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * FIXME Do not pass things directly to syslog, rather use the existing logging
 * facilities to sort logging ... however that mechanism needs to be somehow
 * configurable and we don't have that option yet
 */
static void _temporary_log_fn(int level,
			      const char *file __attribute__((unused)),
			      int line __attribute__((unused)),
			      int dm_errno __attribute__((unused)),
			      const char *message)
{
	level &= ~(_LOG_STDERR | _LOG_ONCE);

	switch (level) {
	case _LOG_DEBUG:
		if (dmeventd_debug >= 3)
			syslog(LOG_DEBUG, "%s", message);
		break;
	case _LOG_INFO:
		if (dmeventd_debug >= 2)
			syslog(LOG_INFO, "%s", message);
		break;
	case _LOG_NOTICE:
		if (dmeventd_debug >= 1)
			syslog(LOG_NOTICE, "%s", message);
		break;
	case _LOG_WARN:
		syslog(LOG_WARNING, "%s", message);
		break;
	case _LOG_ERR:
		syslog(LOG_ERR, "%s", message);
		break;
	default:
		syslog(LOG_CRIT, "%s", message);
	}
}

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

	/*
	 * Need some space for allocations.  1024 should be more
	 * than enough for what we need (device mapper name splitting)
	 */
	if (!_mem_pool && !(_mem_pool = dm_pool_create("mirror_dso", 1024)))
		goto out;

	if (!_lvm_handle) {
		lvm2_log_fn(_temporary_log_fn);
		if (!(_lvm_handle = lvm2_init())) {
			dm_pool_destroy(_mem_pool);
			_mem_pool = NULL;
			goto out;
		}
		lvm2_disable_dmeventd_monitoring(_lvm_handle);
		/* FIXME Temporary: move to dmeventd core */
		lvm2_run(_lvm_handle, "_memlock_inc");
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
		lvm2_run(_lvm_handle, "_memlock_dec");
		dm_pool_destroy(_mem_pool);
		_mem_pool = NULL;
		lvm2_exit(_lvm_handle);
		_lvm_handle = NULL;
	}

	pthread_mutex_unlock(&_register_mutex);
}

struct dm_pool *dmeventd_lvm2_pool(void)
{
	return _mem_pool;
}

int dmeventd_lvm2_run(const char *cmdline)
{
	return lvm2_run(_lvm_handle, cmdline);
}

int dmeventd_lvm2_command(struct dm_pool *mem, char *buffer, size_t size,
			  const char *cmd, const char *device)
{
	char *vg = NULL, *lv = NULL, *layer;
	int r;

	if (!dm_split_lvm_name(mem, device, &vg, &lv, &layer)) {
		syslog(LOG_ERR, "Unable to determine VG name from %s.\n",
		       device);
		return 0;
	}

	/* strip off the mirror component designations */
	layer = strstr(lv, "_mlog");
	if (layer)
		*layer = '\0';

	r = dm_snprintf(buffer, size, "%s %s/%s", cmd, vg, lv);

	dm_pool_free(mem, vg);

	if (r < 0) {
		syslog(LOG_ERR, "Unable to form LVM command. (too long).\n");
		return 0;
	}

	return 1;
}
