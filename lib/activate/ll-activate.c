/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "ll-activate.h"
#include "log.h"

struct dm_task *setup_dm_task(const char *name, int task)
{
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task))) {
		stack;
		return NULL;
	}

	dm_task_set_name(dmt, name);

	return dmt;
}

int device_info(const char *name, struct dm_info *info)
{
	int r = 0;
	struct dm_task *dmt;

	log_very_verbose("Getting device info for %s", name);
	if (!(dmt = setup_dm_task(name, DM_DEVICE_INFO))) {
		stack;
		return 0;
	}

	if (!dm_task_run(dmt)) {
		stack;
		goto out;
	}

	if (!dm_task_get_info(dmt, info)) {
		stack;
		goto out;
	}
	r = 1;

 out:
	dm_task_destroy(dmt);
	return r;
}

/*
 * The next three functions return -1 on error.
 */
int device_active(const char *name)
{
	struct dm_info info;

	if (!device_info(name, &info)) {
		stack;
		return -1;
	}

	log_very_verbose("%s is%s active", name, info.exists ? "" : " not");
	return info.exists;
}

int device_suspended(const char *name)
{
	struct dm_info info;

	if (!device_info(name, &info)) {
		stack;
		return -1;
	}

	log_very_verbose("%s is%s suspended", name,
			 info.suspended ? "" : " not");
	return info.suspended;
}

int device_open_count(const char *name)
{
	struct dm_info info;

	if (!device_info(name, &info)) {
		stack;
		return -1;
	}

	log_very_verbose("%s is open %d time(s)", name, info.open_count);
	return info.open_count;
}


int device_deactivate(const char *name)
{
	int r;
	struct dm_task *dmt;

	log_very_verbose("Deactivating '%s'.", name);

	if (!(dmt = setup_dm_task(name, DM_DEVICE_REMOVE))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		stack;

	dm_task_destroy(dmt);

	return r;
}

static int _suspend(const char *name, int sus)
{
	int r;
	struct dm_task *dmt;
	int task = sus ? DM_DEVICE_SUSPEND : DM_DEVICE_RESUME;

	log_very_verbose("%s %s", sus ? "Suspending" : "Resuming", name);
	if (!(dmt = setup_dm_task(name, task))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_err("Couldn't %s device '%s'", sus ? "suspend" : "resume",
			name);

	dm_task_destroy(dmt);
	return r;
}

int device_suspend(const char *name)
{
	return _suspend(name, 1);
}

int device_resume(const char *name)
{
	return _suspend(name, 0);
}
