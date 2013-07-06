/*
 * Copyright (C) 2011-2013 Red Hat, Inc. All rights reserved.
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

/* First warning when thin is 80% full. */
#define WARNING_THRESH 80
/* Run a check every 5%. */
#define CHECK_STEP 5
/* Do not bother checking thins less than 50% full. */
#define CHECK_MINIMUM 50

#define UMOUNT_COMMAND "/bin/umount"

#define THIN_DEBUG 0

struct dso_state {
	struct dm_pool *mem;
	int metadata_percent_check;
	int data_percent_check;
	uint64_t known_metadata_size;
	uint64_t known_data_size;
	char cmd_str[1024];
};


/* TODO - move this mountinfo code into library to be reusable */
#ifdef linux
#  include "kdev_t.h"
#else
#  define MAJOR(x) major((x))
#  define MINOR(x) minor((x))
#  define MKDEV(x,y) makedev((x),(y))
#endif

/* Get dependencies for device, and try to find matching device */
static int _has_deps(const char *name, int tp_major, int tp_minor, int *dev_minor)
{
	struct dm_task *dmt;
	const struct dm_deps *deps;
	struct dm_info info;
	int major, minor;
	int r = 0;

	if (!(dmt = dm_task_create(DM_DEVICE_DEPS)))
		return 0;

	if (!dm_task_set_name(dmt, name))
		goto out;

	if (!dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &info))
		goto out;

	if (!(deps = dm_task_get_deps(dmt)))
		goto out;

	if (!info.exists || deps->count != 1)
		goto out;

	major = (int) MAJOR(deps->device[0]);
	minor = (int) MINOR(deps->device[0]);
	if ((major != tp_major) || (minor != tp_minor))
		goto out;

	*dev_minor = info.minor;

#if THIN_DEBUG
	{
		char dev_name[PATH_MAX];
		if (dm_device_get_name(major, minor, 0, dev_name, sizeof(dev_name)))
			syslog(LOG_DEBUG, "Found %s (%u:%u) depends on %s",
			       name, major, *dev_minor, dev_name);
	}
#endif
	r = 1;
out:
	dm_task_destroy(dmt);

	return r;
}

/* Get all active devices */
static int _find_all_devs(dm_bitset_t bs, int tp_major, int tp_minor)
{
	struct dm_task *dmt;
	struct dm_names *names;
	unsigned next = 0;
	int minor, r = 1;

	if (!(dmt = dm_task_create(DM_DEVICE_LIST)))
		return 0;

	if (!dm_task_run(dmt)) {
		r = 0;
		goto out;
	}

	if (!(names = dm_task_get_names(dmt))) {
		r = 0;
		goto out;
	}

	if (!names->dev)
		goto out;

	do {
		names = (struct dm_names *)((char *) names + next);
		if (_has_deps(names->name, tp_major, tp_minor, &minor))
			dm_bit_set(bs, minor);
		next = names->next;
	} while (next);

out:
	dm_task_destroy(dmt);

	return r;
}

static int _extend(struct dso_state *state)
{
#if THIN_DEBUG
	syslog(LOG_INFO, "dmeventd executes: %s.\n", state->cmd_str);
#endif
	return (dmeventd_lvm2_run(state->cmd_str) == LVM2_COMMAND_SUCCEEDED);
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
			++argc;
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

struct mountinfo_s {
	struct dm_info info;
	dm_bitset_t minors; /* Bitset for active thin pool minors */
	const char *device;
};

static int _umount_device(char *buffer, unsigned major, unsigned minor,
			  char *target, void *cb_data)
{
	struct mountinfo_s *data = cb_data;

	if ((major == data->info.major) && dm_bit(data->minors, minor)) {
		syslog(LOG_INFO, "Unmounting thin volume %s from %s.\n",
		       data->device, target);
		if (!_run(UMOUNT_COMMAND, "-fl", target, NULL))
			syslog(LOG_ERR, "Failed to umount thin %s from %s: %s.\n",
			       data->device, target, strerror(errno));
	}

	return 1;
}

/*
 * Find all thin pool users and try to umount them.
 * TODO: work with read-only thin pool support
 */
static void _umount(struct dm_task *dmt, const char *device)
{
	static const size_t MINORS = 4096;
	struct mountinfo_s data = {
		.device = device,
	};

	if (!dm_task_get_info(dmt, &data.info))
		return;

	dmeventd_lvm2_unlock();

	if (!(data.minors = dm_bitset_create(NULL, MINORS))) {
		syslog(LOG_ERR, "Failed to allocate bitset. Not unmounting %s.\n", device);
		goto out;
	}

	if (!_find_all_devs(data.minors, data.info.major, data.info.minor)) {
		syslog(LOG_ERR, "Failed to detect mounted volumes for %s.\n", device);
		goto out;
	}

	if (!dm_mountinfo_read(_umount_device, &data)) {
		syslog(LOG_ERR, "Could not parse mountinfo file.\n");
		goto out;
	}

out:
	if (data.minors)
		dm_bitset_destroy(data.minors);
	dmeventd_lvm2_lock();
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask event __attribute__((unused)),
		   void **private)
{
	const char *device = dm_task_get_name(dmt);
	int percent;
	struct dso_state *state = *private;
	struct dm_status_thin_pool *tps = NULL;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;

#if 0
	/* No longer monitoring, waiting for remove */
	if (!state->meta_percent_check && !state->data_percent_check)
		return;
#endif
	dmeventd_lvm2_lock();

	dm_get_next_target(dmt, next, &start, &length, &target_type, &params);

	if (!target_type || (strcmp(target_type, "thin-pool") != 0)) {
		syslog(LOG_ERR, "Invalid target type.\n");
		goto out;
	}

	if (!dm_get_status_thin_pool(state->mem, params, &tps)) {
		syslog(LOG_ERR, "Failed to parse status.\n");
		_umount(dmt, device);
		goto out;
	}

#if THIN_DEBUG
	syslog(LOG_INFO, "%p: Got status %" PRIu64 " / %" PRIu64
	       " %" PRIu64  " / %" PRIu64 ".\n", state,
	       tps->used_metadata_blocks, tps->total_metadata_blocks,
	       tps->used_data_blocks, tps->total_data_blocks);
#endif

	/* Thin pool size had changed. Clear the threshold. */
	if (state->known_metadata_size != tps->total_metadata_blocks) {
		state->metadata_percent_check = CHECK_MINIMUM;
		state->known_metadata_size = tps->total_metadata_blocks;
	}

	if (state->known_data_size != tps->total_data_blocks) {
		state->data_percent_check = CHECK_MINIMUM;
		state->known_data_size = tps->total_data_blocks;
	}

	percent = 100 * tps->used_metadata_blocks / tps->total_metadata_blocks;
	if (percent >= state->metadata_percent_check) {
		/*
		 * Usage has raised more than CHECK_STEP since the last
		 * time. Run actions.
		 */
		state->metadata_percent_check = (percent / CHECK_STEP) * CHECK_STEP + CHECK_STEP;

		/* FIXME: extension of metadata needs to be written! */
		if (percent >= WARNING_THRESH) /* Print a warning to syslog. */
			syslog(LOG_WARNING, "Thin metadata %s is now %i%% full.\n",
			       device, percent);
		 /* Try to extend the metadata, in accord with user-set policies */
		if (!_extend(state)) {
			syslog(LOG_ERR, "Failed to extend thin metadata %s.\n",
			       device);
			_umount(dmt, device);
		}
		/* FIXME: hmm READ-ONLY switch should happen in error path */
	}

	percent = 100 * tps->used_data_blocks / tps->total_data_blocks;
	if (percent >= state->data_percent_check) {
		/*
		 * Usage has raised more than CHECK_STEP since
		 * the last time. Run actions.
		 */
		state->data_percent_check = (percent / CHECK_STEP) * CHECK_STEP + CHECK_STEP;

		if (percent >= WARNING_THRESH) /* Print a warning to syslog. */
			syslog(LOG_WARNING, "Thin %s is now %i%% full.\n", device, percent);
		/* Try to extend the thin data, in accord with user-set policies */
		if (!_extend(state)) {
			syslog(LOG_ERR, "Failed to extend thin %s.\n", device);
			state->data_percent_check = 0;
			_umount(dmt, device);
		}
		/* FIXME: hmm READ-ONLY switch should happen in error path */
	}
out:
	if (tps)
		dm_pool_free(state->mem, tps);

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
		goto bad;

	if (!(statemem = dm_pool_create("thin_pool_state", 2048)) ||
	    !(state = dm_pool_zalloc(statemem, sizeof(*state))) ||
	    !dmeventd_lvm2_command(statemem, state->cmd_str,
				   sizeof(state->cmd_str),
				   "lvextend --use-policies",
				   device)) {
		if (statemem)
			dm_pool_destroy(statemem);
		dmeventd_lvm2_exit();
		goto bad;
	}

	state->mem = statemem;
	state->metadata_percent_check = CHECK_MINIMUM;
	state->data_percent_check = CHECK_MINIMUM;
	*private = state;

	syslog(LOG_INFO, "Monitoring thin %s.\n", device);

	return 1;
bad:
	syslog(LOG_ERR, "Failed to monitor thin %s.\n", device);

	return 0;
}

int unregister_device(const char *device,
		      const char *uuid __attribute__((unused)),
		      int major __attribute__((unused)),
		      int minor __attribute__((unused)),
		      void **private)
{
	struct dso_state *state = *private;

	syslog(LOG_INFO, "No longer monitoring thin %s.\n", device);
	dm_pool_destroy(state->mem);
	dmeventd_lvm2_exit();

	return 1;
}
