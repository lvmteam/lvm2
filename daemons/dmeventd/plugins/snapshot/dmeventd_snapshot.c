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

#include <sys/wait.h>
#include <syslog.h> /* FIXME Replace syslog with multilog */
/* FIXME Missing openlog? */

/* First warning when snapshot is 80% full. */
#define WARNING_THRESH 80
/* Run a check every 5%. */
#define CHECK_STEP 5
/* Do not bother checking snapshots less than 50% full. */
#define CHECK_MINIMUM 50

#define UMOUNT_COMMAND "/bin/umount"

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

static int _run(const char *cmd, ...)
{
        va_list ap;
        int argc = 1; /* for argv[0], i.e. cmd */
        int i = 0;
        const char **argv;
        pid_t pid = fork();
        int status;

        if (pid == 0) { /* child */
                va_start(ap, cmd);
                while (va_arg(ap, const char *))
                        ++ argc;
                va_end(ap);

                /* + 1 for the terminating NULL */
                argv = alloca(sizeof(const char *) * (argc + 1));

                argv[0] = cmd;
                va_start(ap, cmd);
                while ((argv[++i] = va_arg(ap, const char *)));
                va_end(ap);

                execvp(cmd, (char **)argv);
                syslog(LOG_ERR, "Failed to execute %s: %s.\n", cmd, strerror(errno));
                exit(127);
        }

        if (pid > 0) { /* parent */
                if (waitpid(pid, &status, 0) != pid)
                        return 0; /* waitpid failed */
                if (!WIFEXITED(status) || WEXITSTATUS(status))
                        return 0; /* the child failed */
        }

        if (pid < 0)
                return 0; /* fork failed */

        return 1; /* all good */
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
	if (dm_snprintf(cmd_str, sizeof(cmd_str),
			"lvextend --use-policies %s/%s", vg, lv) < 0) {
		syslog(LOG_ERR, "Unable to form LVM command: Device name too long.");
		return 0;
	}

	r = dmeventd_lvm2_run(cmd_str);
	syslog(LOG_INFO, "Extension of snapshot %s/%s %s.", vg, lv,
	       (r == ECMD_PROCESSED) ? "finished successfully" : "failed");
	return r == ECMD_PROCESSED;
}

static void _umount(const char *device, int major, int minor)
{
	FILE *mounts;
	char buffer[4096];
	char *words[3];
	struct stat st;

	if (!(mounts = fopen("/proc/mounts", "r"))) {
		syslog(LOG_ERR, "Could not read /proc/mounts. Not umounting %s.\n", device);
		return;
	}

	while (!feof(mounts)) {
		/* read a line of /proc/mounts */
		if (!fgets(buffer, sizeof(buffer), mounts))
			break; /* eof, likely */

		/* words[0] is the mount point and words[1] is the device path */
		dm_split_words(buffer, 3, 0, words);

		/* find the major/minor of the device */
		if (stat(words[0], &st))
			continue; /* can't stat, skip this one */

		if (S_ISBLK(st.st_mode) &&
		    major(st.st_rdev) == major &&
		    minor(st.st_rdev) == minor) {
			syslog(LOG_ERR, "Unmounting invalid snapshot %s from %s.", device, words[1]);
                        if (!_run(UMOUNT_COMMAND, "-fl", words[1], NULL))
                                syslog(LOG_ERR, "Failed to umount snapshot %s from %s: %s.",
                                       device, words[1], strerror(errno));
		}
	}

	if (fclose(mounts))
		syslog(LOG_ERR, "Failed to close /proc/mounts.\n");
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

	if (status.invalid) {
		struct dm_info info;
		if (dm_task_get_info(dmt, &info)) {
			dmeventd_lvm2_unlock();
			_umount(device, info.major, info.minor);
                        return;
		} /* else; too bad, but this is best-effort thing... */
	}

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
