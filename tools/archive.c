/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "archive.h"
#include "dbg_malloc.h"

static struct {
	int enabled;
	char *dir;
	unsigned int keep_days;
	unsigned int keep_number;

} _archive_params;

int archive_init(const char *dir,
		 unsigned int keep_days, unsigned int keep_min)
{
	if (!(_archive_params.dir = dbg_strdup(dir))) {
		log_err("Couldn't create copy of archive dir.");
		return 0;
	}

	_archive_params.keep_days = keep_days;
	_archive_params.keep_number = keep_number;
	_archive_params.enabled = 1;
	return 1;
}

void archive_exit(void)
{
	dbg_free(_archive_params.dir);
	memset(&_archive_params, 0, sizeof(_archive_params));
}

void archive_enable(int flag)
{
	_archive_params.enabled = flag;
}

static int __archive(struct volume_group *vg)
{
	int r;
	struct format_instance *archiver;

	if (!(archiver = backup_format_create(vg->cmd,
					      _archive_params.dir,
					      _archive_params.keep_days,
					      _archive_params.keep_number))) {
		log_error("Couldn't create archiver object.");
		return 0;
	}

	if (!(r = archiver->ops->vg_write(archiver, vg)))
		stack;

	archiver->ops->destroy(archiver);
	return r;
}

int archive(struct volume_group *vg)
{
	if (!_archive_params.enabled)
		return 1;

	if (test_mode()) {
		log_print("Test mode: Skipping archiving of volume group.");
		return 1;
	}

	log_print("Creating archive of volume group '%s' ...", vg->name);
	if (!__archive(vg)) {
		log_error("Archiving failed.");
		return 0;
	}

	return 1;
}



static struct {
	int enabled;
	char *dir;

} _backup_params;

int backup_init(const char *dir)
{
	if (!(_backup_params.dir = dbg_strdup(dir))) {
		log_err("Couldn't create copy of backup dir.");
		return 0;
	}

	return 1;
}

void backup_exit(void)
{
	dbg_free(_backup_params.dir);
	memset(&backup_params, 0, sizeof(_backup_params));
}

void backup_enable(int flag)
{
	_backup_params.enabled = flag;
}

static int __backup(struct volume_group *vg)
{
	int r;
	struct format_instance *tf;
	char name[PATH_MAX];

	if (lvm_snprintf(name, sizeof(name), "%s/%s",
			 _backup_params.dir, vg->name) < 0) {
		log_err("Couldn't generate backup filename for volume group.");
		return 0;
	}

	if (!(tf = text_format_create(vg->cmd, name))) {
		log_error("Couldn't create backup object.");
		return 0;
	}

	if (!(r = tf->ops->vg_write(tf, vg)))
		stack;

	tf->ops->destroy(tf);
	return r;
}

int backup(struct volume_group *vg)
{
	if (!_backup_params.enabled)
		return 1;

	if (test_mode()) {
		log_print("Test mode: Skipping volume group backup.");
		return 1;
	}

	log_print("Creating backup of volume group '%s' ...", vg->name);

	if (!__backup(vg)) {
		log_error("Backup failed.");
		return 0;
	}

	return 1;
}
