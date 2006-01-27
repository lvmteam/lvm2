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

#define ME_IGNORE    0
#define ME_INSYNC    1
#define ME_FAILURE   2

/* FIXME: We may need to lock around operations to these */
static int register_count = 0;
static struct dm_pool *mem_pool = NULL;

static int _get_mirror_event(char *params)
{
	int i, rtn = ME_INSYNC;
	int max_args = 30;  /* should support at least 8-way mirrors */
	char *args[max_args];
	char *dev_status_str;
	char *log_status_str;
	char *sync_str;
	char *p;
	int log_argc, num_devs, num_failures=0;

	if (max_args <= split_words(params, max_args, args)) {
		syslog(LOG_ERR, "Unable to split mirror parameters: Arg list too long");
		return -E2BIG;
	}

	/*
	 * Unused:  0 409600 mirror
	 * Used  :  2 253:4 253:5 400/400 1 AA 3 cluster 253:3 A
	*/
	num_devs = atoi(args[0]);
	dev_status_str = args[3 + num_devs];
	log_argc = atoi(args[4 + num_devs]);
	log_status_str = args[4 + num_devs + log_argc];
	sync_str = args[1 + num_devs];

	syslog(LOG_DEBUG, " num_devs = %d", num_devs);
	syslog(LOG_DEBUG, " dev_status_str: %s", dev_status_str);
	syslog(LOG_DEBUG, " log_argc = %d", log_argc);
	syslog(LOG_DEBUG, " log_status_str: %s", log_status_str);
	syslog(LOG_DEBUG, " sync_str: %s", sync_str);

	/* Check for bad mirror devices */
	for (i = 0; i < num_devs; i++) {
		if (dev_status_str[i] == 'D') {
			syslog(LOG_ERR, "Mirror device, %s, has failed.\n", args[i+1]);
			num_failures++;
		}
	}

	/* Check for bad log device */
	if (log_status_str[0] == 'D') {
		syslog(LOG_ERR, "Log device, %s, has failed.\n",
		       args[3 + num_devs + log_argc]);
		num_failures++;
	}

	if (num_failures) {
		rtn = ME_FAILURE;
		goto out;
	}

	for(p = strstr(sync_str, "/"), i = (p - sync_str) - 1;
	    p && (i >= 0);
	    i--) {
		syslog(LOG_DEBUG, "p[%d] (%c) ?= sync_str[%d] (%c)",
		       i+1, p[i+1], i, sync_str[i]);
		if(p[i+1] != sync_str[i]){
			rtn = ME_IGNORE;
			break;
		}
	}

 out:
	return rtn;
}

static void _temporary_log_fn(int level, const char *file, int line, const char *format)
{
	syslog(LOG_DEBUG, "%s", format);
}

static int _remove_failed_devices(const char *device)
{
	int r;
	void *handle;
	int cmd_size = 256;	/* FIXME Use system restriction */
	char cmd_str[cmd_size];
	char *vg = NULL, *lv = NULL, *layer = NULL;

	// syslog(LOG_DEBUG, "Entering remove_failed_device\n");
	if (strlen(device) > 200)
		return -ENAMETOOLONG;

	if (!split_dm_name(mem_pool, device, &vg, &lv, &layer)) {
		syslog(LOG_ERR, "Unable to determine VG name from %s",
		       device);
		return -ENOMEM;
	}

	/* FIXME Is any sanity-checking required on %s? */
	syslog(LOG_INFO, "vgreduce --removemissing %s\n", vg);
	if (cmd_size <= snprintf(cmd_str, cmd_size, "vgreduce --removemissing %s", vg)) {
		/* this error should be caught above, but doesn't hurt to check again */
		syslog(LOG_ERR, "Unable to form LVM command: Device name too long");
		dm_pool_empty(mem_pool);  /* FIXME: not safe with multiple threads */
		return -ENAMETOOLONG;
	}

	lvm2_log_fn(_temporary_log_fn);
	handle = lvm2_init();
	lvm2_log_level(handle, 1);
	r = lvm2_run(handle, cmd_str);

	syslog(LOG_INFO, "lvconvert -m0 %s/%s\n", vg, lv);
	if (cmd_size <= snprintf(cmd_str, cmd_size, "lvconvert -m0 %s/%s\n", vg, lv)) {
		/* this error should be caught above, but doesn't hurt to check again */
		syslog(LOG_ERR, "Unable to form LVM command: Device name too long");
		return -ENAMETOOLONG;
	}
	r = lvm2_run(handle, cmd_str);

	// syslog(LOG_DEBUG, "Exiting remove_failed_device\n");

	dm_pool_empty(mem_pool);  /* FIXME: not safe with multiple threads */
	return (r == 1)? 0: -1;
}

void process_event(const char *device, enum dm_event_type event)
{
	int pid;
	struct dm_task *dmt;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;

	/* FIXME Too late to fork here under low memory! */
	/* FIXME This should run within a dmeventd thread instead. */
	pid = fork();
	if (pid > 0)
		return;

	/*
	 * Fork twice to prevent the process from
	 * becoming a zombie after exiting.
	 */
	pid = fork();
	if(pid > 0) {
		exit(EXIT_SUCCESS);
	} else if (pid < 0) {
		openlog("dmeventd (mirror_dso)", LOG_PID, LOG_DAEMON);
		syslog(LOG_ERR, "Fork new process to handle device failure");
		syslog(LOG_ERR, "Handling failure anyway (a harmless zombie process will result)");
	} else {
		setsid();
		openlog("dmeventd (mirror_dso)", LOG_PID, LOG_DAEMON);
	}

	syslog(LOG_NOTICE, "An event occurred on %s\n", device);

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
				syslog(LOG_ERR, "Failed to remove faulty devices in %s\n",
				       device);
			else
				syslog(LOG_NOTICE, "%s is now a linear device.\n",
					device);
			break;
		case ME_IGNORE:
			break;
		default:
			syslog(LOG_INFO, "Unknown event received.\n");
		}
	} while (next);

 fail:
	if (dmt)
		dm_task_destroy(dmt);

	exit(0);
}

int register_device(const char *device)
{
	syslog(LOG_INFO, "Monitoring %s for events\n", device);

	/*
	 * Need some space for allocations.  1024 should be more
	 * than enough for what we need (device mapper name splitting)
	 */
	if (!mem_pool)
		mem_pool = dm_pool_create("mirror_dso", 1024);

	if (!mem_pool)
		return 0;

	register_count++;

        return 1;
}

int unregister_device(const char *device)
{
        syslog(LOG_INFO, "Stopped monitoring %s for events\n", device);

	if (!(--register_count)) {
		dm_pool_destroy(mem_pool);
		mem_pool = NULL;
	}

        return 1;
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
