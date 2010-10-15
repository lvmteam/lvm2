/*
 * Copyright (C) 2007-2010 Red Hat, Inc. All rights reserved.
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

#include <syslog.h> /* FIXME Replace syslog with multilog */
/* FIXME Missing openlog? */

/* First warning when snapshot is 80% full. */
#define WARNING_THRESH 80
/* Run a check every 5%. */
#define CHECK_STEP 5
/* Do not bother checking snapshots less than 50% full. */
#define CHECK_MINIMUM 50

struct snap_status {
	int invalid;
	int used;
	int max;
};

/* FIXME possibly reconcile this with target_percent when we gain
   access to regular LVM library here. */
static void _parse_snapshot_params(char *params, struct snap_status *status)
{
	char *p;
	/*
	 * xx/xx	-- fractions used/max
	 * Invalid	-- snapshot invalidated
	 * Unknown	-- status unknown
	 */
	status->used = status->max = 0;

	if (!strncmp(params, "Invalid", 7)) {
		status->invalid = 1;
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

	status->used = atoi(params);
	status->max = atoi(p);
}

static int _extend(const char *device)
{
	char *vg = NULL, *lv = NULL, *layer = NULL;
	char cmd_str[1024];
	int r = 0;

	if (!dm_split_lvm_name(dmeventd_lvm2_pool(), device, &vg, &lv, &layer)) {
		syslog(LOG_ERR, "Unable to determine VG name from %s.", device);
		return 0;
	}
	if (sizeof(cmd_str) <= snprintf(cmd_str, sizeof(cmd_str),
					"lvextend --use-policies %s/%s", vg, lv)) {
		syslog(LOG_ERR, "Unable to form LVM command: Device name too long.");
		return 0;
	}

	r = dmeventd_lvm2_run(cmd_str);
	syslog(LOG_INFO, "Extension of snapshot %s/%s %s.", vg, lv,
	       (r == ECMD_PROCESSED) ? "finished successfully" : "failed");
	return r == ECMD_PROCESSED;
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask event __attribute__((unused)),
		   void **private)
{
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	struct snap_status status = { 0 };
	const char *device = dm_task_get_name(dmt);
	int percent, *percent_check = (int*)private;

	/* No longer monitoring, waiting for remove */
	if (!*percent_check)
		return;

	dmeventd_lvm2_lock();

	dm_get_next_target(dmt, next, &start, &length, &target_type, &params);
	if (!target_type)
		goto out;

	_parse_snapshot_params(params, &status);

	/*
	 * If the snapshot has been invalidated or we failed to parse
	 * the status string. Report the full status string to syslog.
	 */
	if (status.invalid || !status.max) {
		syslog(LOG_ERR, "Snapshot %s changed state to: %s\n", device, params);
		*percent_check = 0;
		goto out;
	}

	percent = 100 * status.used / status.max;
	if (percent >= *percent_check) {
		/* Usage has raised more than CHECK_STEP since the last
		   time. Run actions. */
		*percent_check = (percent / CHECK_STEP) * CHECK_STEP + CHECK_STEP;
		if (percent >= WARNING_THRESH) /* Print a warning to syslog. */
			syslog(LOG_WARNING, "Snapshot %s is now %i%% full.\n", device, percent);
		/* Try to extend the snapshot, in accord with user-set policies */
		if (!_extend(device))
			syslog(LOG_ERR, "Failed to extend snapshot %s.", device);
	}
out:
	dmeventd_lvm2_unlock();
}

int register_device(const char *device,
		    const char *uuid __attribute__((unused)),
		    int major __attribute__((unused)),
		    int minor __attribute__((unused)),
		    void **private)
{
	int *percent_check = (int*)private;
	int r = dmeventd_lvm2_init();

	*percent_check = CHECK_MINIMUM;

	syslog(LOG_INFO, "Monitoring snapshot %s\n", device);
	return r;
}

int unregister_device(const char *device,
		      const char *uuid __attribute__((unused)),
		      int major __attribute__((unused)),
		      int minor __attribute__((unused)),
		      void **unused __attribute__((unused)))
{
	syslog(LOG_INFO, "No longer monitoring snapshot %s\n",
	       device);
	dmeventd_lvm2_exit();
	return 1;
}
