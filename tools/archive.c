/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

static struct {
	int enabled;
	char *dir;
	unsigned int keep_days;
	unsigned int keep_number;

} _archive_params;

int archive_init(const char *dir, unsigned int keep_days, unsigned int keep_min)
{
	_archive_params.dir = NULL;

	if (!*dir)
		return 1;

	if (!create_dir(dir))
		return 0;

	if (!(_archive_params.dir = dbg_strdup(dir))) {
		log_error("Couldn't copy archive directory name.");
		return 0;
	}

	_archive_params.keep_days = keep_days;
	_archive_params.keep_number = keep_min;
	_archive_params.enabled = 1;
	return 1;
}

void archive_exit(void)
{
	if (_archive_params.dir)
		dbg_free(_archive_params.dir);
	memset(&_archive_params, 0, sizeof(_archive_params));
}

void archive_enable(int flag)
{
	_archive_params.enabled = flag;
}

static char *_build_desc(struct pool *mem, const char *line, int before)
{
	size_t len = strlen(line) + 32;
	char *buffer;

	if (!(buffer = pool_zalloc(mem, strlen(line) + 32))) {
		stack;
		return NULL;
	}

	if (snprintf(buffer, len,
		     "Created %s executing '%s'",
		     before ? "*before*" : "*after*", line) < 0) {
		stack;
		return NULL;
	}

	return buffer;
}

static int __archive(struct volume_group *vg)
{
	char *desc;

	if (!(desc = _build_desc(vg->cmd->mem, vg->cmd->cmd_line, 1))) {
		stack;
		return 0;
	}

	return archive_vg(vg, _archive_params.dir, desc,
			  _archive_params.keep_days,
			  _archive_params.keep_number);
}

int archive(struct volume_group *vg)
{
	if (!_archive_params.enabled || !_archive_params.dir)
		return 1;

	if (test_mode()) {
		log_verbose("Test mode: Skipping archiving of volume group.");
		return 1;
	}

	log_verbose("Archiving volume group \"%s\" metadata.", vg->name);
	if (!__archive(vg)) {
		log_error("Volume group \"%s\" metadata archive failed.",
			  vg->name);
		return 0;
	}

	return 1;
}

int archive_display(struct cmd_context *cmd, const char *vg_name)
{
	return archive_list(cmd, _archive_params.dir, vg_name);
}

static struct {
	int enabled;
	char *dir;

} _backup_params;

int backup_init(const char *dir)
{
	_backup_params.dir = NULL;
	if (!*dir)
		return 1;

	if (!create_dir(dir))
		return 0;

	if (!(_backup_params.dir = dbg_strdup(dir))) {
		log_error("Couldn't copy backup directory name.");
		return 0;
	}

	return 1;
}

void backup_exit(void)
{
	if (_backup_params.dir)
		dbg_free(_backup_params.dir);
	memset(&_backup_params, 0, sizeof(_backup_params));
}

void backup_enable(int flag)
{
	_backup_params.enabled = flag;
}

static int __backup(struct volume_group *vg)
{
	char name[PATH_MAX];
	char *desc;

	if (!(desc = _build_desc(vg->cmd->mem, vg->cmd->cmd_line, 0))) {
		stack;
		return 0;
	}

	if (lvm_snprintf(name, sizeof(name), "%s/%s",
			 _backup_params.dir, vg->name) < 0) {
		log_error("Failed to generate volume group metadata backup "
			  "filename.");
		return 0;
	}

	log_verbose("Creating volume group backup \"%s\"", name);

	return backup_to_file(name, desc, vg);
}

int backup(struct volume_group *vg)
{
	if (!_backup_params.enabled || !_backup_params.dir) {
		log_print("WARNING: This metadata update is NOT backed up");
		return 1;
	}

	if (test_mode()) {
		log_verbose("Test mode: Skipping volume group backup.");
		return 1;
	}

	if (!__backup(vg)) {
		log_error("Backup of volume group %s metadata failed.",
			  vg->name);
		return 0;
	}

	return 1;
}

int backup_remove(const char *vg_name)
{
	char path[PATH_MAX];

	if (lvm_snprintf(path, sizeof(path), "%s/%s",
			 _backup_params.dir, vg_name) < 0) {
		log_err("Failed to generate backup filename (for removal).");
		return 0;
	}

	/*
	 * Let this fail silently.
	 */
	unlink(path);
	return 1;
}

struct volume_group *backup_read_vg(struct cmd_context *cmd,
				    const char *vg_name, const char *file)
{
	struct volume_group *vg = NULL;
	struct format_instance *tf;
	struct list *mdah;
	struct metadata_area *mda;
	void *context;

	if (!(context = create_text_context(cmd, file,
					    cmd->cmd_line)) ||
	    !(tf = cmd->fmt_backup->ops->create_instance(cmd->fmt_backup, NULL,
							 context))) {
		log_error("Couldn't create text format object.");
		return NULL;
	}

	list_iterate(mdah, &tf->metadata_areas) {
		mda = list_item(mdah, struct metadata_area);
		if (!(vg = mda->ops->vg_read(tf, vg_name, mda)))
			stack;
		break;
	}

	tf->fmt->ops->destroy_instance(tf);
	return vg;
}

/* ORPHAN and VG locks held before calling this */
int backup_restore_vg(struct cmd_context *cmd, struct volume_group *vg)
{
	struct list *pvh;
	struct physical_volume *pv;
	struct cache_info *info;

	/*
	 * FIXME: Check that the PVs referenced in the backup are
	 * not members of other existing VGs.
	 */

	/* Attempt to write out using currently active format */
	if (!(vg->fid = cmd->fmt->ops->create_instance(cmd->fmt, vg->name,
						       NULL))) {
		log_error("Failed to allocate format instance");
		return 0;
	}

	/* Add any metadata areas on the PVs */
	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;
		if (!(info = info_from_pvid(pv->dev->pvid))) {
			log_error("PV %s missing from cache",
				  dev_name(pv->dev));
			return 0;
		}
		if (cmd->fmt != info->fmt) {
			log_error("PV %s is a different format (%s)",
				  dev_name(pv->dev), info->fmt->name);
			return 0;
		}
		if (!vg->fid->fmt->ops->
		    pv_setup(vg->fid->fmt, __UINT64_C(0), 0, 0, 0,
			     __UINT64_C(0), &vg->fid->metadata_areas, pv, vg)) {
			log_error("Format-specific setup for %s failed",
				  dev_name(pv->dev));
			return 0;
		}
	}

	if (!vg_write(vg)) {
		stack;
		return 0;
	}

	return 1;
}

/* ORPHAN and VG locks held before calling this */
int backup_restore_from_file(struct cmd_context *cmd, const char *vg_name,
			     const char *file)
{
	struct volume_group *vg;

	/*
	 * Read in the volume group from the text file.
	 */
	if (!(vg = backup_read_vg(cmd, vg_name, file))) {
		stack;
		return 0;
	}

	return backup_restore_vg(cmd, vg);
}

int backup_restore(struct cmd_context *cmd, const char *vg_name)
{
	char path[PATH_MAX];

	if (lvm_snprintf(path, sizeof(path), "%s/%s",
			 _backup_params.dir, vg_name) < 0) {
		log_err("Failed to generate backup filename (for restore).");
		return 0;
	}

	return backup_restore_from_file(cmd, vg_name, path);
}

int backup_to_file(const char *file, const char *desc, struct volume_group *vg)
{
	int r = 0;
	struct format_instance *tf;
	struct list *mdah;
	struct metadata_area *mda;
	void *context;
	struct cmd_context *cmd;

	cmd = vg->cmd;

	if (!(context = create_text_context(cmd, file, desc)) ||
	    !(tf = cmd->fmt_backup->ops->create_instance(cmd->fmt_backup, NULL,
							 context))) {
		log_error("Couldn't create backup object.");
		return 0;
	}

	/* Write and commit the metadata area */
	list_iterate(mdah, &tf->metadata_areas) {
		mda = list_item(mdah, struct metadata_area);
		if (!(r = mda->ops->vg_write(tf, vg, mda))) {
			stack;
			continue;
		}
		if (mda->ops->vg_commit &&
		    !(r = mda->ops->vg_commit(tf, vg, mda))) {
			stack;
		}
	}

	tf->fmt->ops->destroy_instance(tf);
	return r;
}
