/*
 * Copyright (C) 2007-2015 Red Hat, Inc. All rights reserved.
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
#include "dmeventd_lvm.h"
#include "libdevmapper-event.h"

#include <sys/wait.h>
#include <stdarg.h>

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
	char cmd_lvextend[512];
};

DM_EVENT_LOG_FN("snap")

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
                log_sys_error("exec", cmd);
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
	log_debug("Extending snapshot via %s.", cmd);
	return dmeventd_lvm2_run_with_lock(cmd);
}

static void _umount(const char *device, int major, int minor)
{
	FILE *mounts;
	char buffer[4096];
	char *words[3];
	struct stat st;
	const char procmounts[] = "/proc/mounts";

	if (!(mounts = fopen(procmounts, "r"))) {
		log_sys_error("fopen", procmounts);
		log_error("Not umounting %s.", device);
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
			log_error("Unmounting invalid snapshot %s from %s.", device, words[1]);
			if (!_run(UMOUNT_COMMAND, "-fl", words[1], NULL))
				log_error("Failed to umount snapshot %s from %s: %s.",
					  device, words[1], strerror(errno));
		}
	}

	if (fclose(mounts))
		log_sys_error("close", procmounts);
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask event __attribute__((unused)),
		   void **user)
{
	struct dso_state *state = *user;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	struct dm_status_snapshot *status = NULL;
	const char *device = dm_task_get_name(dmt);
	int percent;

	/* No longer monitoring, waiting for remove */
	if (!state->percent_check)
		return;

	dm_get_next_target(dmt, next, &start, &length, &target_type, &params);
	if (!target_type || strcmp(target_type, "snapshot")) {
		log_error("Target %s is not snapshot.", target_type);
		return;
	}

	if (!dm_get_status_snapshot(state->mem, params, &status))
		return;

	if (status->invalid || status->overflow) {
		struct dm_info info;
		log_error("Snapshot %s is lost.", device);
		if (dm_task_get_info(dmt, &info)) {
			_umount(device, info.major, info.minor);
			goto_out;
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
	if (status->invalid || status->overflow || !status->total_sectors) {
		log_error("Snapshot %s changed state to: %s.", device, params);
		state->percent_check = 0;
		goto out;
	}

	percent = (int) (100 * status->used_sectors / status->total_sectors);
	if (percent >= state->percent_check) {
		/* Usage has raised more than CHECK_STEP since the last
		   time. Run actions. */
		state->percent_check = (percent / CHECK_STEP) * CHECK_STEP + CHECK_STEP;

		if (percent >= WARNING_THRESH) /* Print a warning to syslog. */
			log_warn("WARNING: Snapshot %s is now %i%% full.", device, percent);

		/* Try to extend the snapshot, in accord with user-set policies */
		if (!_extend(state->cmd_lvextend))
			log_error("Failed to extend snapshot %s.", device);
	}
out:
	dm_pool_free(state->mem, status);
}

int register_device(const char *device,
		    const char *uuid __attribute__((unused)),
		    int major __attribute__((unused)),
		    int minor __attribute__((unused)),
		    void **user)
{
	struct dso_state *state;

	if (dmeventd_lvm2_init_with_pool("snapshot_state", state))
		goto_bad;

	if (!dmeventd_lvm2_command(state->mem, state->cmd_lvextend,
				   sizeof(state->cmd_lvextend),
				   "lvextend --use-policies", device)) {
		dmeventd_lvm2_exit_with_pool(state);
		goto_bad;
	}

	state->percent_check = CHECK_MINIMUM;
	*user = state;

	log_info("Monitoring snapshot %s.", device);

	return 1;
bad:
	log_error("Failed to monitor snapshot %s.", device);

	return 0;
}

int unregister_device(const char *device,
		      const char *uuid __attribute__((unused)),
		      int major __attribute__((unused)),
		      int minor __attribute__((unused)),
		      void **user)
{
	struct dso_state *state = *user;

	dmeventd_lvm2_exit_with_pool(state);
	log_info("No longer monitoring snapshot %s.", device);

	return 1;
}
