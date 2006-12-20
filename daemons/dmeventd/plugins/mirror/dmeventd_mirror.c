/*
 * Copyright (C) 2005 Red Hat, Inc. All rights reserved.
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

#include "libdevmapper.h"
#include "libdevmapper-event.h"
#include "lvm2cmd.h"
#include "lvm-string.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include <syslog.h> /* FIXME Replace syslog with multilog */
/* FIXME Missing openlog? */

#define ME_IGNORE    0
#define ME_INSYNC    1
#define ME_FAILURE   2

static pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;

/* FIXME: We may need to lock around operations to these */
static int _register_count = 0;

/* FIXME Unsafe static? */
static struct dm_pool *_mem_pool = NULL;

static int _get_mirror_event(char *params)
{
	int i, r = ME_INSYNC;

#define MAX_ARGS 30;  /* should support at least 8-way mirrors */
/* FIXME Remove unnecessary limit.  It tells you how many devices there are - use it! */

	char *args[MAX_ARGS];
	char *dev_status_str;
	char *log_status_str;
	char *sync_str;
	char *p;
	int log_argc, num_devs, num_failures=0;

	/* FIXME Remove unnecessary limit - get num_devs here */
	if (MAX_ARGS <= dm_split_words(params, MAX_ARGS, 0, args)) {
		syslog(LOG_ERR, "Unable to split mirror parameters: Arg list too long");
		return -E2BIG;	/* FIXME Why? Unused */
	}

	/*
	 * Unused:  0 409600 mirror
	 * Used  :  2 253:4 253:5 400/400 1 AA 3 cluster 253:3 A
	*/
	num_devs = atoi(args[0]);

	/* FIXME *Now* split rest of args */

	dev_status_str = args[3 + num_devs];
	log_argc = atoi(args[4 + num_devs]);
	log_status_str = args[4 + num_devs + log_argc];
	sync_str = args[1 + num_devs];

	/* Check for bad mirror devices */
	for (i = 0; i < num_devs; i++)
		if (dev_status_str[i] == 'D') {
			syslog(LOG_ERR, "Mirror device, %s, has failed.\n", args[i+1]);
			num_failures++;
		}

	/* Check for bad log device */
	if (log_status_str[0] == 'D') {
		syslog(LOG_ERR, "Log device, %s, has failed.\n",
		       args[3 + num_devs + log_argc]);
		num_failures++;
	}

	if (num_failures) {
		r = ME_FAILURE;
		goto out;
	}

	p = strstr(sync_str, "/");
	if (p) {
		p[0] = '\0';
		if (strcmp(sync_str, p+1))
			r = ME_IGNORE;
		p[0] = '/';
	} else {
		/*
		 * How the hell did we get this?
		 * Might mean all our parameters are screwed.
		 */
		syslog(LOG_ERR, "Unable to parse sync string.");
		r = ME_IGNORE;
	}
 out:
	return r;
}

static void _temporary_log_fn(int level, const char *file,
			      int line, const char *format)
{
	if (!strncmp(format, "WARNING: ", 9) && (level < 5))
		syslog(LOG_CRIT, "%s", format);
	else
		syslog(LOG_DEBUG, "%s", format);
}

static int _remove_failed_devices(const char *device)
{
	int r;
	void *handle;
#define CMD_SIZE 256	/* FIXME Use system restriction */
	char cmd_str[CMD_SIZE];
	char *vg = NULL, *lv = NULL, *layer = NULL;

	if (strlen(device) > 200)  /* FIXME Use real restriction */
		return -ENAMETOOLONG;	/* FIXME These return code distinctions are not used so remove them! */

	if (!dm_split_lvm_name(_mem_pool, device, &vg, &lv, &layer)) {
		syslog(LOG_ERR, "Unable to determine VG name from %s",
		       device);
		return -ENOMEM;	/* FIXME Replace with generic error return - reason for failure has already got logged */
	}

	/* FIXME Is any sanity-checking required on %s? */
	if (CMD_SIZE <= snprintf(cmd_str, CMD_SIZE, "vgreduce --removemissing %s", vg)) {
		/* this error should be caught above, but doesn't hurt to check again */
		syslog(LOG_ERR, "Unable to form LVM command: Device name too long");
		dm_pool_empty(_mem_pool);  /* FIXME: not safe with multiple threads */
		return -ENAMETOOLONG; /* FIXME Replace with generic error return - reason for failure has already got logged */
	}

	lvm2_log_fn(_temporary_log_fn);
	handle = lvm2_init();
	lvm2_log_level(handle, 1);
	r = lvm2_run(handle, cmd_str);

	dm_pool_empty(_mem_pool);  /* FIXME: not safe with multiple threads */
	return (r == 1) ? 0 : -1;
}

void process_event(const char *device, enum dm_event_type event)
{
	struct dm_task *dmt;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;

	if (pthread_mutex_trylock(&_lock)) {
		syslog(LOG_NOTICE, "Another thread is handling an event.  Waiting...");
		pthread_mutex_lock(&_lock);
	}
	/* FIXME Move inside libdevmapper */
	if (!(dmt = dm_task_create(DM_DEVICE_STATUS))) {
		syslog(LOG_ERR, "Unable to create dm_task.\n");
		goto fail;
	}

	if (!dm_task_set_name(dmt, device)) {
		syslog(LOG_ERR, "Unable to set device name.\n");
		goto fail;
	}

	if (!dm_task_run(dmt)) {
		syslog(LOG_ERR, "Unable to run task.\n");
		goto fail;
	}

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);

		if (!target_type)
			syslog(LOG_INFO, "%s mapping lost.\n", device);
			continue;

		if (strcmp(target_type, "mirror")) {
			syslog(LOG_INFO, "%s has unmirrored portion.\n", device);
			continue;
		}

		switch(_get_mirror_event(params)) {
		case ME_INSYNC:
			/* FIXME: all we really know is that this
			   _part_ of the device is in sync
			   Also, this is not an error
			*/
			syslog(LOG_NOTICE, "%s is now in-sync\n", device);
			break;
		case ME_FAILURE:
			syslog(LOG_ERR, "Device failure in %s\n", device);
			if (_remove_failed_devices(device))
				/* FIXME Why are all the error return codes unused? Get rid of them? */
				syslog(LOG_ERR, "Failed to remove faulty devices in %s\n",
				       device);
			/* Should check before warning user that device is now linear
			else
				syslog(LOG_NOTICE, "%s is now a linear device.\n",
					device);
			*/
			break;
		case ME_IGNORE:
			break;
		default:
			/* FIXME Wrong: it can also return -E2BIG but it's never used! */
			syslog(LOG_INFO, "Unknown event received.\n");
		}
	} while (next);

 fail:
	if (dmt)
		dm_task_destroy(dmt);
	pthread_mutex_unlock(&_lock);
}

int register_device(const char *device)
{
	syslog(LOG_INFO, "Monitoring mirror device, %s for events\n", device);

	/*
	 * Need some space for allocations.  1024 should be more
	 * than enough for what we need (device mapper name splitting)
	 */
	if (!_mem_pool && !(_mem_pool = dm_pool_create("mirror_dso", 1024)))
		return 0;

	_register_count++;

        return 1;
}

int unregister_device(const char *device)
{
	if (!--_register_count) {
		dm_pool_destroy(_mem_pool);
		_mem_pool = NULL;
	}

        return 1;
}
