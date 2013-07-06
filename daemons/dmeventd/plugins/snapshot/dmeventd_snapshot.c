/*
 * Copyright (C) 2007-2011 Red Hat, Inc. All rights reserved.
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
#include "libdevmapper-event.h"
#include "dmeventd_lvm.h"

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

struct dso_state {
	struct dm_pool *mem;
	int percent_check;
	uint64_t known_size;
	char cmd_str[1024];
};

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

static int _extend(const char *cmd)
{
	return dmeventd_lvm2_run(cmd) == LVM2_COMMAND_SUCCEEDED;
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
		if (dm_split_words(buffer, 3, 0, words) < 2)
			continue;

		/* find the major/minor of the device */
		if (stat(words[0], &st))
			continue; /* can't stat, skip this one */

		if (S_ISBLK(st.st_mode) &&
		    major(st.st_rdev) == major &&
		    minor(st.st_rdev) == minor) {
			syslog(LOG_ERR, "Unmounting invalid snapshot %s from %s.\n", device, words[1]);
                        if (!_run(UMOUNT_COMMAND, "-fl", words[1], NULL))
                                syslog(LOG_ERR, "Failed to umount snapshot %s from %s: %s.\n",
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
	struct dm_status_snapshot *status = NULL;
	const char *device = dm_task_get_name(dmt);
	int percent;
	struct dso_state *state = *private;

	/* No longer monitoring, waiting for remove */
	if (!state->percent_check)
		return;

	dmeventd_lvm2_lock();

	dm_get_next_target(dmt, next, &start, &length, &target_type, &params);
	if (!target_type)
		goto out;

	if (!dm_get_status_snapshot(state->mem, params, &status))
		goto out;

	if (status->invalid) {
		struct dm_info info;
		if (dm_task_get_info(dmt, &info)) {
			dmeventd_lvm2_unlock();
			_umount(device, info.major, info.minor);
			return;
		} /* else; too bad, but this is best-effort thing... */
	}

	/* Snapshot size had changed. Clear the threshold. */
	if (state->known_size != status->total_sectors) {
		state->percent_check = CHECK_MINIMUM;
		state->known_size = status->total_sectors;
	}

	/*
	 * If the snapshot has been invalidated or we failed to parse
	 * the status string. Report the full status string to syslog.
	 */
	if (status->invalid || !status->total_sectors) {
		syslog(LOG_ERR, "Snapshot %s changed state to: %s\n", device, params);
		state->percent_check = 0;
		goto out;
	}

	percent = (int) (100 * status->used_sectors / status->total_sectors);
	if (percent >= state->percent_check) {
		/* Usage has raised more than CHECK_STEP since the last
		   time. Run actions. */
		state->percent_check = (percent / CHECK_STEP) * CHECK_STEP + CHECK_STEP;

		if (percent >= WARNING_THRESH) /* Print a warning to syslog. */
			syslog(LOG_WARNING, "Snapshot %s is now %i%% full.\n", device, percent);
		/* Try to extend the snapshot, in accord with user-set policies */
		if (!_extend(state->cmd_str))
			syslog(LOG_ERR, "Failed to extend snapshot %s.\n", device);
	}

out:
	if (status)
		dm_pool_free(state->mem, status);
	dmeventd_lvm2_unlock();
}

int register_device(const char *device,
		    const char *uuid __attribute__((unused)),
		    int major __attribute__((unused)),
		    int minor __attribute__((unused)),
		    void **private)
{
	struct dm_pool *statemem = NULL;
	struct dso_state *state;

	if (!dmeventd_lvm2_init())
		goto out;

	if (!(statemem = dm_pool_create("snapshot_state", 512)) ||
	    !(state = dm_pool_zalloc(statemem, sizeof(*state))))
		goto bad;

	if (!dmeventd_lvm2_command(statemem, state->cmd_str,
				   sizeof(state->cmd_str),
				   "lvextend --use-policies", device))
		goto bad;

	state->mem = statemem;
	state->percent_check = CHECK_MINIMUM;
	*private = state;

	syslog(LOG_INFO, "Monitoring snapshot %s\n", device);

	return 1;
bad:
	if (statemem)
		dm_pool_destroy(statemem);
	dmeventd_lvm2_exit();
out:
	syslog(LOG_ERR, "Failed to monitor snapshot %s.\n", device);

	return 0;
}

int unregister_device(const char *device,
		      const char *uuid __attribute__((unused)),
		      int major __attribute__((unused)),
		      int minor __attribute__((unused)),
		      void **private)
{
	struct dso_state *state = *private;

	syslog(LOG_INFO, "No longer monitoring snapshot %s\n", device);
	dm_pool_destroy(state->mem);
	dmeventd_lvm2_exit();

	return 1;
}
