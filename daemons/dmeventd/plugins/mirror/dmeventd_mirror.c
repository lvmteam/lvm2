/*
 * Copyright (C) 2005-2012 Red Hat, Inc. All rights reserved.
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
#include "defaults.h"

#include <syslog.h> /* FIXME Replace syslog with multilog */
/* FIXME Missing openlog? */
/* FIXME Replace most syslogs with log_error() style messages and add complete context. */
/* FIXME Reformat to 80 char lines. */

#define ME_IGNORE    0
#define ME_INSYNC    1
#define ME_FAILURE   2

static int _process_status_code(const char status_code, const char *dev_name,
				const char *dev_type, int r)
{
	/*
	 *    A => Alive - No failures
	 *    D => Dead - A write failure occurred leaving mirror out-of-sync
	 *    F => Flush failed.
	 *    S => Sync - A sychronization failure occurred, mirror out-of-sync
	 *    R => Read - A read failure occurred, mirror data unaffected
	 *    U => Unclassified failure (bug)
	 */ 
	if (status_code == 'F') {
		syslog(LOG_ERR, "%s device %s flush failed.",
		       dev_type, dev_name);
		r = ME_FAILURE;
	} else if (status_code == 'S')
		syslog(LOG_ERR, "%s device %s sync failed.",
		       dev_type, dev_name);
	else if (status_code == 'R')
		syslog(LOG_ERR, "%s device %s read failed.",
		       dev_type, dev_name);
	else if (status_code != 'A') {
		syslog(LOG_ERR, "%s device %s has failed (%c).",
		       dev_type, dev_name, status_code);
		r = ME_FAILURE;
	}

	return r;
}

static int _get_mirror_event(char *params)
{
	int i, r = ME_INSYNC;
	char **args = NULL;
	char *dev_status_str;
	char *log_status_str;
	char *sync_str;
	char *p = NULL;
	int log_argc, num_devs;

	/*
	 * dm core parms:	     0 409600 mirror
	 * Mirror core parms:	     2 253:4 253:5 400/400
	 * New-style failure params: 1 AA
	 * New-style log params:     3 cluster 253:3 A
	 *			 or  3 disk 253:3 A
	 *			 or  1 core
	 */

	/* number of devices */
	if (!dm_split_words(params, 1, 0, &p))
		goto out_parse;

	if (!(num_devs = atoi(p)) ||
	    (num_devs > DEFAULT_MIRROR_MAX_IMAGES) || (num_devs < 0))
		goto out_parse;
	p += strlen(p) + 1;

	/* devices names + "400/400" + "1 AA" + 1 or 3 log parms + NULL */
	args = dm_malloc((num_devs + 7) * sizeof(char *));
	if (!args || dm_split_words(p, num_devs + 7, 0, args) < num_devs + 5)
		goto out_parse;

	/* FIXME: Code differs from lib/mirror/mirrored.c */
	dev_status_str = args[2 + num_devs];
	log_argc = atoi(args[3 + num_devs]);
	log_status_str = args[3 + num_devs + log_argc];
	sync_str = args[num_devs];

	/* Check for bad mirror devices */
	for (i = 0; i < num_devs; i++)
		r = _process_status_code(dev_status_str[i], args[i],
			i ? "Secondary mirror" : "Primary mirror", r);

	/* Check for bad disk log device */
	if (log_argc > 1)
		r = _process_status_code(log_status_str[0],
					 args[2 + num_devs + log_argc],
					 "Log", r);

	if (r == ME_FAILURE)
		goto out;

	p = strstr(sync_str, "/");
	if (p) {
		p[0] = '\0';
		if (strcmp(sync_str, p+1))
			r = ME_IGNORE;
		p[0] = '/';
	} else
		goto out_parse;

out:
	dm_free(args);
	return r;
	
out_parse:
	dm_free(args);
	syslog(LOG_ERR, "Unable to parse mirror status string.");
	return ME_IGNORE;
}

static int _remove_failed_devices(const char *device)
{
	int r;
#define CMD_SIZE 256	/* FIXME Use system restriction */
	char cmd_str[CMD_SIZE];

	if (!dmeventd_lvm2_command(dmeventd_lvm2_pool(), cmd_str, sizeof(cmd_str),
				  "lvconvert --config devices{ignore_suspended_devices=1} "
				  "--repair --use-policies", device))
		return -ENAMETOOLONG; /* FIXME Replace with generic error return - reason for failure has already got logged */

	r = dmeventd_lvm2_run(cmd_str);

	syslog(LOG_INFO, "Repair of mirrored device %s %s.", device,
	       (r == ECMD_PROCESSED) ? "finished successfully" : "failed");

	return (r == ECMD_PROCESSED) ? 0 : -1;
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask event __attribute__((unused)),
		   void **unused __attribute__((unused)))
{
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	const char *device = dm_task_get_name(dmt);

	dmeventd_lvm2_lock();

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);

		if (!target_type) {
			syslog(LOG_INFO, "%s mapping lost.", device);
			continue;
		}

		if (strcmp(target_type, "mirror")) {
			syslog(LOG_INFO, "%s has unmirrored portion.", device);
			continue;
		}

		switch(_get_mirror_event(params)) {
		case ME_INSYNC:
			/* FIXME: all we really know is that this
			   _part_ of the device is in sync
			   Also, this is not an error
			*/
			syslog(LOG_NOTICE, "%s is now in-sync.", device);
			break;
		case ME_FAILURE:
			syslog(LOG_ERR, "Device failure in %s.", device);
			if (_remove_failed_devices(device))
				/* FIXME Why are all the error return codes unused? Get rid of them? */
				syslog(LOG_ERR, "Failed to remove faulty devices in %s.",
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
			/* FIXME Provide value then! */
			syslog(LOG_INFO, "Unknown event received.");
		}
	} while (next);

	dmeventd_lvm2_unlock();
}

int register_device(const char *device,
		    const char *uuid __attribute__((unused)),
		    int major __attribute__((unused)),
		    int minor __attribute__((unused)),
		    void **unused __attribute__((unused)))
{
	if (!dmeventd_lvm2_init())
		return 0;

	syslog(LOG_INFO, "Monitoring mirror device %s for events.", device);

	return 1;
}

int unregister_device(const char *device,
		      const char *uuid __attribute__((unused)),
		      int major __attribute__((unused)),
		      int minor __attribute__((unused)),
		      void **unused __attribute__((unused)))
{
	syslog(LOG_INFO, "No longer monitoring mirror device %s for events.",
	       device);
	dmeventd_lvm2_exit();

	return 1;
}
