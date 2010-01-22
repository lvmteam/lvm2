/*
 * Copyright (C) 2007-2008 Red Hat, Inc. All rights reserved.
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

#include "lvm-string.h"

#include <pthread.h>
#include <syslog.h> /* FIXME Replace syslog with multilog */
/* FIXME Missing openlog? */

/* First warning when snapshot is 80% full. */
#define WARNING_THRESH 80
/* Further warnings at 85%, 90% and 95% fullness. */
#define WARNING_STEP 5

struct snap_status {
	int invalid;
	int used;
	int max;
};

/* FIXME possibly reconcile this with target_percent when we gain
   access to regular LVM library here. */
static void _parse_snapshot_params(char *params, struct snap_status *stat)
{
	char *p;
	/*
	 * xx/xx	-- fractions used/max
	 * Invalid	-- snapshot invalidated
	 * Unknown	-- status unknown
	 */
	stat->used = stat->max = 0;

	if (!strncmp(params, "Invalid", 7)) {
		stat->invalid = 1;
		return;
	}

	/*
	 * When we return without setting non-zero max, the parent is
	 * responsible for reporting errors.
	 */
	if (!strncmp(params, "Unknown", 7))
		return;

	if (!(p = strstr(params, "/")))
		return;

	*p = '\0';
	p++;

	stat->used = atoi(params);
	stat->max = atoi(p);
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask event __attribute((unused)),
		   void **private)
{
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	struct snap_status stat = { 0 };
	const char *device = dm_task_get_name(dmt);
	int percent, *percent_warning = (int*)private;

	/* No longer monitoring, waiting for remove */
	if (!*percent_warning)
		return;

	dmeventd_lvm2_lock();

	dm_get_next_target(dmt, next, &start, &length, &target_type, &params);
	if (!target_type)
		goto out;

	_parse_snapshot_params(params, &stat);
	/*
	 * If the snapshot has been invalidated or we failed to parse
	 * the status string. Report the full status string to syslog.
	 */
	if (stat.invalid || !stat.max) {
		syslog(LOG_ERR, "Snapshot %s changed state to: %s\n", device, params);
		*percent_warning = 0;
		goto out;
	}

	percent = 100 * stat.used / stat.max;
	if (percent >= *percent_warning) {
		syslog(LOG_WARNING, "Snapshot %s is now %i%% full.\n", device, percent);
		/* Print warning on the next multiple of WARNING_STEP. */
		*percent_warning = (percent / WARNING_STEP) * WARNING_STEP + WARNING_STEP;
	}
out:
	dmeventd_lvm2_unlock();
}

int register_device(const char *device,
		    const char *uuid __attribute((unused)),
		    int major __attribute((unused)),
		    int minor __attribute((unused)),
		    void **private)
{
	int *percent_warning = (int*)private;
	int r = dmeventd_lvm2_init();

	*percent_warning = WARNING_THRESH; /* Print warning if snapshot is full */

	syslog(LOG_INFO, "Monitoring snapshot %s\n", device);
	return r;
}

int unregister_device(const char *device,
		      const char *uuid __attribute((unused)),
		      int major __attribute((unused)),
		      int minor __attribute((unused)),
		      void **unused __attribute((unused)))
{
	syslog(LOG_INFO, "No longer monitoring snapshot %s\n",
	       device);
	dmeventd_lvm2_exit();
	return 1;
}
