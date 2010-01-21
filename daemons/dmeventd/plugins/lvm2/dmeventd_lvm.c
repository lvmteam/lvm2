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

#include "lvm2cmd.h"
#include "errors.h"
#include "libdevmapper-event.h"
#include "dmeventd_lvm.h"

#include <pthread.h>
#include <syslog.h>

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

/* FIXME Remove this: Pass messages back to dmeventd core for processing. */
static void _temporary_log_fn(int level,
			      const char *file __attribute((unused)),
			      int line __attribute((unused)),
			      int dm_errno __attribute((unused)),
			      const char *format)
{
	level &= ~_LOG_STDERR;

	if (!strncmp(format, "WARNING: ", 9) && (level < 5))
		syslog(LOG_CRIT, "%s", format);
	else
		syslog(LOG_DEBUG, "%s", format);
}

void dmeventd_lvm2_lock(void)
{
	if (pthread_mutex_trylock(&_event_mutex)) {
		syslog(LOG_NOTICE, "Another thread is handling an event. Waiting...");
		pthread_mutex_lock(&_event_mutex);
	}
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
		dm_pool_destroy(_mem_pool);
		_mem_pool = NULL;
		lvm2_run(_lvm_handle, "_memlock_dec");
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

