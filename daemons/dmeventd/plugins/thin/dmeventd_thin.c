/*
 * Copyright (C) 2011-2016 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib.h"	/* using here lvm log */
#include "dmeventd_lvm.h"
#include "libdevmapper-event.h"

#include <sys/wait.h>
#include <stdarg.h>
#include <pthread.h>

/* TODO - move this mountinfo code into library to be reusable */
#ifdef __linux__
#  include "kdev_t.h"
#else
#  define MAJOR(x) major((x))
#  define MINOR(x) minor((x))
#endif

/* First warning when thin data or metadata is 80% full. */
#define WARNING_THRESH	(DM_PERCENT_1 * 80)
/* Umount thin LVs when thin data or metadata LV is >=
 * and lvextend --use-policies has failed. */
#define UMOUNT_THRESH	(DM_PERCENT_1 * 95)
/* Run a check every 5%. */
#define CHECK_STEP	(DM_PERCENT_1 *  5)
/* Do not bother checking thin data or metadata is less than 50% full. */
#define CHECK_MINIMUM	(DM_PERCENT_1 * 50)

#define UMOUNT_COMMAND "/bin/umount"

#define MAX_FAILS	(10)

#define THIN_DEBUG 0

struct dso_state {
	struct dm_pool *mem;
	int metadata_percent_check;
	int metadata_percent;
	int data_percent_check;
	int data_percent;
	uint64_t known_metadata_size;
	uint64_t known_data_size;
	unsigned fails;
	char cmd_str[1024];
};

DM_EVENT_LOG_FN("thin")

#define UUID_PREFIX "LVM-"

/* Figure out device UUID has LVM- prefix and is OPEN */
static int _has_unmountable_prefix(int major, int minor)
{
	struct dm_task *dmt;
	struct dm_info info;
	const char *uuid;
	int r = 0;

	if (!(dmt = dm_task_create(DM_DEVICE_INFO)))
		return_0;

	if (!dm_task_set_major_minor(dmt, major, minor, 1))
		goto_out;

	if (!dm_task_no_flush(dmt))
		stack;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &info))
		goto out;

	if (!info.exists || !info.open_count)
		goto out; /* Not open -> not mounted */

	if (!(uuid = dm_task_get_uuid(dmt)))
		goto out;

	/* Check it's public mountable LV
	 * has prefix  LVM-  and UUID size is 68 chars */
	if (memcmp(uuid, UUID_PREFIX, sizeof(UUID_PREFIX) - 1) ||
	    strlen(uuid) != 68)
		goto out;

#if THIN_DEBUG
	log_debug("Found logical volume %s (%u:%u).", uuid, major, minor);
#endif
	r = 1;
out:
	dm_task_destroy(dmt);

	return r;
}

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

	if (!_has_unmountable_prefix(major, info.minor))
		goto out;

#if THIN_DEBUG
	{
		char dev_name[PATH_MAX];
		if (dm_device_get_name(major, minor, 0, dev_name, sizeof(dev_name)))
			log_debug("Found %s (%u:%u) depends on %s.",
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

struct mountinfo_s {
	const char *device;
	struct dm_info info;
	dm_bitset_t minors; /* Bitset for active thin pool minors */
};

static int _umount_device(char *buffer, unsigned major, unsigned minor,
			  char *target, void *cb_data)
{
	struct mountinfo_s *data = cb_data;
	char *words[10];

	if ((major == data->info.major) && dm_bit(data->minors, minor)) {
		if (dm_split_words(buffer, DM_ARRAY_SIZE(words), 0, words) < DM_ARRAY_SIZE(words))
			words[9] = NULL; /* just don't show device name */
		log_info("Unmounting thin %s (%d:%d) of thin pool %s (%u:%u) from mount point \"%s\".",
			 words[9] ? : "", major, minor, data->device,
			 data->info.major, data->info.minor,
			 target);
		if (!_run(UMOUNT_COMMAND, "-fl", target, NULL))
			log_error("Failed to lazy umount thin %s (%d:%d) from %s: %s.",
				  words[9], major, minor, target, strerror(errno));
	}

	return 1;
}

/*
 * Find all thin pool LV users and try to umount them.
 * TODO: work with read-only thin pool support
 */
static void _umount(struct dm_task *dmt)
{
	/* TODO: Convert to use hash to reduce memory usage */
	static const size_t MINORS = (1U << 20); /* 20 bit */
	struct mountinfo_s data = { NULL };

	if (!dm_task_get_info(dmt, &data.info))
		return;

	data.device = dm_task_get_name(dmt);

	if (!(data.minors = dm_bitset_create(NULL, MINORS))) {
		log_error("Failed to allocate bitset. Not unmounting %s.", data.device);
		goto out;
	}

	if (!_find_all_devs(data.minors, data.info.major, data.info.minor)) {
		log_error("Failed to detect mounted volumes for %s.", data.device);
		goto out;
	}

	if (!dm_mountinfo_read(_umount_device, &data)) {
		log_error("Could not parse mountinfo file.");
		goto out;
	}

out:
	if (data.minors)
		dm_bitset_destroy(data.minors);
}

static int _use_policy(struct dm_task *dmt, struct dso_state *state)
{
#if THIN_DEBUG
	log_debug("dmeventd executes: %s.", state->cmd_str);
#endif
	if (!dmeventd_lvm2_run_with_lock(state->cmd_str)) {
		log_error("Failed to extend thin pool %s.",
			  dm_task_get_name(dmt));
		state->fails++;
		return 0;
	}

	state->fails = 0;
	return 1;
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask event __attribute__((unused)),
		   void **user)
{
	const char *device = dm_task_get_name(dmt);
	struct dso_state *state = *user;
	struct dm_status_thin_pool *tps = NULL;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	int needs_policy = 0;
	int needs_umount = 0;
	struct dm_task *new_dmt = NULL;

#if THIN_DEBUG
	log_debug("Watch for tp-data:%.2f%%  tp-metadata:%.2f%%.",
		  dm_percent_to_float(state->data_percent_check),
		  dm_percent_to_float(state->metadata_percent_check));
#endif

	if (event & DM_EVENT_DEVICE_ERROR) {
		/* Error -> no need to check and do instant resize */
		state->data_percent = state->metadata_percent = 0;
		if (_use_policy(dmt, state))
			goto out;

		stack;

		/*
		 * Rather update oldish status
		 * since after 'command' processing
		 * percentage info could have changed a lot.
		 * If we would get above UMOUNT_THRESH
		 * we would wait for next sigalarm.
		 */
		if (!(new_dmt = dm_task_create(DM_DEVICE_STATUS)))
			goto_out;

		if (!dm_task_set_uuid(new_dmt, dm_task_get_uuid(dmt)))
			goto_out;

		/* Non-blocking status read */
		if (!dm_task_no_flush(new_dmt))
			log_warn("WARNING: Can't set no_flush for dm status.");

		if (!dm_task_run(new_dmt))
			goto_out;

		dmt = new_dmt;
	}

	dm_get_next_target(dmt, next, &start, &length, &target_type, &params);

	if (!target_type || (strcmp(target_type, "thin-pool") != 0)) {
		log_error("Invalid target type.");
		goto out;
	}

	if (!dm_get_status_thin_pool(state->mem, params, &tps)) {
		log_error("Failed to parse status.");
		needs_umount = 1;
		goto out;
	}

#if THIN_DEBUG
	log_debug("Thin pool status " FMTu64 "/" FMTu64 "  "
		  FMTu64 "/" FMTu64 ".",
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

	state->metadata_percent = dm_make_percent(tps->used_metadata_blocks, tps->total_metadata_blocks);
	if (state->metadata_percent >= state->metadata_percent_check) {
		/*
		 * Usage has raised more than CHECK_STEP since the last
		 * time. Run actions.
		 */
		state->metadata_percent_check = (state->metadata_percent / CHECK_STEP) * CHECK_STEP + CHECK_STEP;

		/* FIXME: extension of metadata needs to be written! */
		if (state->metadata_percent >= WARNING_THRESH)
			log_warn("WARNING: Thin pool %s metadata is now %.2f%% full.",
				 device, dm_percent_to_float(state->metadata_percent));
		needs_policy = 1;

		if (state->metadata_percent >= UMOUNT_THRESH)
			needs_umount = 1;
	}

	state->data_percent = dm_make_percent(tps->used_data_blocks, tps->total_data_blocks);
	if (state->data_percent >= state->data_percent_check) {
		/*
		 * Usage has raised more than CHECK_STEP since
		 * the last time. Run actions.
		 */
		state->data_percent_check = (state->data_percent / CHECK_STEP) * CHECK_STEP + CHECK_STEP;

		if (state->data_percent >= WARNING_THRESH)
			log_warn("WARNING: Thin pool %s data is now %.2f%% full.",
				 device, dm_percent_to_float(state->data_percent));
		needs_policy = 1;

		if (state->data_percent >= UMOUNT_THRESH)
			needs_umount = 1;
	}

	if (needs_policy &&
	    _use_policy(dmt, state))
		needs_umount = 0; /* No umount when command was successful */
out:
	if (needs_umount) {
		_umount(dmt);
		/* Until something changes, do not retry any more actions */
		state->data_percent_check = state->metadata_percent_check = (DM_PERCENT_1 * 101);
	}

	if (tps)
		dm_pool_free(state->mem, tps);

	if (state->fails >= MAX_FAILS) {
		log_warn("WARNING: Dropping monitoring of %s. "
			 "lvm2 command fails too often (%u times in row).",
			 device, state->fails);
		pthread_kill(pthread_self(), SIGALRM);
	}

	if (new_dmt)
		dm_task_destroy(new_dmt);
}

int register_device(const char *device,
		    const char *uuid __attribute__((unused)),
		    int major __attribute__((unused)),
		    int minor __attribute__((unused)),
		    void **user)
{
	struct dso_state *state;

	if (!dmeventd_lvm2_init_with_pool("thin_pool_state", state))
		goto_bad;

	if (!dmeventd_lvm2_command(state->mem, state->cmd_str,
				   sizeof(state->cmd_str),
				   "lvextend --use-policies",
				   device)) {
		dmeventd_lvm2_exit_with_pool(state);
		goto_bad;
	}

	state->metadata_percent_check = CHECK_MINIMUM;
	state->data_percent_check = CHECK_MINIMUM;
	*user = state;

	log_info("Monitoring thin pool %s.", device);

	return 1;
bad:
	log_error("Failed to monitor thin pool %s.", device);

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
	log_info("No longer monitoring thin pool %s.", device);

	return 1;
}
