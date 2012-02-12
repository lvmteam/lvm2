/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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
#include "archiver.h"
#include "format-text.h"
#include "lvm-file.h"
#include "lvm-string.h"
#include "lvmcache.h"
#include "toolcontext.h"
#include "locking.h"

#include <unistd.h>

struct archive_params {
	int enabled;
	char *dir;
	unsigned int keep_days;
	unsigned int keep_number;
};

struct backup_params {
	int enabled;
	char *dir;
};

int archive_init(struct cmd_context *cmd, const char *dir,
		 unsigned int keep_days, unsigned int keep_min,
		 int enabled)
{
	archive_exit(cmd);

	if (!(cmd->archive_params = dm_pool_zalloc(cmd->libmem,
						sizeof(*cmd->archive_params)))) {
		log_error("archive_params alloc failed");
		return 0;
	}

	cmd->archive_params->dir = NULL;

	if (!*dir)
		return 1;

	if (!(cmd->archive_params->dir = dm_strdup(dir))) {
		log_error("Couldn't copy archive directory name.");
		return 0;
	}

	cmd->archive_params->keep_days = keep_days;
	cmd->archive_params->keep_number = keep_min;
	archive_enable(cmd, enabled);

	return 1;
}

void archive_exit(struct cmd_context *cmd)
{
	if (!cmd->archive_params)
		return;
	dm_free(cmd->archive_params->dir);
	memset(cmd->archive_params, 0, sizeof(*cmd->archive_params));
}

void archive_enable(struct cmd_context *cmd, int flag)
{
	cmd->archive_params->enabled = flag;
}

static char *_build_desc(struct dm_pool *mem, const char *line, int before)
{
	size_t len = strlen(line) + 32;
	char *buffer;

	if (!(buffer = dm_pool_alloc(mem, len))) {
		log_error("Failed to allocate desc.");
		return NULL;
	}

	if (dm_snprintf(buffer, len, "Created %s executing '%s'",
			before ? "*before*" : "*after*", line) < 0) {
		log_error("Failed to build desc.");
		return NULL;
	}

	return buffer;
}

static int __archive(struct volume_group *vg)
{
	char *desc;

	if (!(desc = _build_desc(vg->cmd->mem, vg->cmd->cmd_line, 1)))
		return_0;

	return archive_vg(vg, vg->cmd->archive_params->dir, desc,
			  vg->cmd->archive_params->keep_days,
			  vg->cmd->archive_params->keep_number);
}

int archive(struct volume_group *vg)
{
	if (!vg->cmd->archive_params->enabled || !vg->cmd->archive_params->dir)
		return 1;

	if (test_mode()) {
		log_verbose("Test mode: Skipping archiving of volume group.");
		return 1;
	}

	if (!dm_create_dir(vg->cmd->archive_params->dir))
		return 0;

	/* Trap a read-only file system */
	if ((access(vg->cmd->archive_params->dir, R_OK | W_OK | X_OK) == -1) &&
	     (errno == EROFS))
		return 0;

	log_verbose("Archiving volume group \"%s\" metadata (seqno %u).", vg->name,
		    vg->seqno);
	if (!__archive(vg)) {
		log_error("Volume group \"%s\" metadata archive failed.",
			  vg->name);
		return 0;
	}

	return 1;
}

int archive_display(struct cmd_context *cmd, const char *vg_name)
{
	int r1, r2;

	r1 = archive_list(cmd, cmd->archive_params->dir, vg_name);
	r2 = backup_list(cmd, cmd->backup_params->dir, vg_name);

	return r1 && r2;
}

int archive_display_file(struct cmd_context *cmd, const char *file)
{
	int r;

	r = archive_list_file(cmd, file);

	return r;
}

int backup_init(struct cmd_context *cmd, const char *dir,
		int enabled)
{
	backup_exit(cmd);

	if (!(cmd->backup_params = dm_pool_zalloc(cmd->libmem,
					       sizeof(*cmd->backup_params)))) {
		log_error("backup_params alloc failed");
		return 0;
	}

	cmd->backup_params->dir = NULL;
	if (!*dir)
		return 1;

	if (!(cmd->backup_params->dir = dm_strdup(dir))) {
		log_error("Couldn't copy backup directory name.");
		return 0;
	}
	backup_enable(cmd, enabled);

	return 1;
}

void backup_exit(struct cmd_context *cmd)
{
	if (!cmd->backup_params)
		return;
	dm_free(cmd->backup_params->dir);
	memset(cmd->backup_params, 0, sizeof(*cmd->backup_params));
}

void backup_enable(struct cmd_context *cmd, int flag)
{
	cmd->backup_params->enabled = flag;
}

static int __backup(struct volume_group *vg)
{
	char name[PATH_MAX];
	char *desc;

	if (!(desc = _build_desc(vg->cmd->mem, vg->cmd->cmd_line, 0)))
		return_0;

	if (dm_snprintf(name, sizeof(name), "%s/%s",
			 vg->cmd->backup_params->dir, vg->name) < 0) {
		log_error("Failed to generate volume group metadata backup "
			  "filename.");
		return 0;
	}

	return backup_to_file(name, desc, vg);
}

int backup_locally(struct volume_group *vg)
{
	if (!vg->cmd->backup_params->enabled || !vg->cmd->backup_params->dir) {
		log_warn("WARNING: This metadata update is NOT backed up");
		return 1;
	}

	if (test_mode()) {
		log_verbose("Test mode: Skipping backup of volume group.");
		return 1;
	}

	if (!dm_create_dir(vg->cmd->backup_params->dir))
		return 0;

	/* Trap a read-only file system */
	if ((access(vg->cmd->backup_params->dir, R_OK | W_OK | X_OK) == -1) &&
	    (errno == EROFS))
		return 0;

	if (!__backup(vg)) {
		log_error("Backup of volume group %s metadata failed.",
			  vg->name);
		return 0;
	}

	return 1;
}

int backup(struct volume_group *vg)
{
	if (vg_is_clustered(vg))
		remote_backup_metadata(vg);

	return backup_locally(vg);
}

int backup_remove(struct cmd_context *cmd, const char *vg_name)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s",
			 cmd->backup_params->dir, vg_name) < 0) {
		log_error("Failed to generate backup filename (for removal).");
		return 0;
	}

	/*
	 * Let this fail silently.
	 */
	if (unlink(path))
		log_sys_debug("unlink", path);

	return 1;
}

struct volume_group *backup_read_vg(struct cmd_context *cmd,
				    const char *vg_name, const char *file)
{
	struct volume_group *vg = NULL;
	struct format_instance *tf;
	struct format_instance_ctx fic;
	struct text_context tc = {.path_live = file,
				  .path_edit = NULL,
				  .desc = cmd->cmd_line};
	struct metadata_area *mda;

	fic.type = FMT_INSTANCE_PRIVATE_MDAS;
	fic.context.private = &tc;
	if (!(tf = cmd->fmt_backup->ops->create_instance(cmd->fmt_backup, &fic))) {
		log_error("Couldn't create text format object.");
		return NULL;
	}

	dm_list_iterate_items(mda, &tf->metadata_areas_in_use) {
		if (!(vg = mda->ops->vg_read(tf, vg_name, mda)))
			stack;
		break;
	}

	if (!vg)
		tf->fmt->ops->destroy_instance(tf);

	return vg;
}

/* ORPHAN and VG locks held before calling this */
int backup_restore_vg(struct cmd_context *cmd, struct volume_group *vg)
{
	struct pv_list *pvl;
	struct format_instance *fid;
	struct format_instance_ctx fic;
	uint32_t tmp;

	/*
	 * FIXME: Check that the PVs referenced in the backup are
	 * not members of other existing VGs.
	 */

	/* Attempt to write out using currently active format */
	fic.type = FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = vg->name;
	fic.context.vg_ref.vg_id = NULL;
	if (!(fid = cmd->fmt->ops->create_instance(cmd->fmt, &fic))) {
		log_error("Failed to allocate format instance");
		return 0;
	}
	vg_set_fid(vg, fid);

	/*
	 * Setting vg->old_name to a blank value will explicitly
	 * disable any attempt to check VG name in existing metadata.
	*/
	vg->old_name = dm_pool_strdup(vg->vgmem, "");

	/* Add any metadata areas on the PVs */
	dm_list_iterate_items(pvl, &vg->pvs) {
		tmp = vg->extent_size;
		vg->extent_size = 0;
		if (!vg->fid->fmt->ops->pv_setup(vg->fid->fmt, pvl->pv, vg)) {
			log_error("Format-specific setup for %s failed",
				  pv_dev_name(pvl->pv));
			return 0;
		}
		vg->extent_size = tmp;
	}

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	return 1;
}

/* ORPHAN and VG locks held before calling this */
int backup_restore_from_file(struct cmd_context *cmd, const char *vg_name,
			     const char *file)
{
	struct volume_group *vg;
	int missing_pvs, r = 0;
	const struct lv_list *lvl;

	/*
	 * Read in the volume group from the text file.
	 */
	if (!(vg = backup_read_vg(cmd, vg_name, file)))
		return_0;

	/* FIXME: Restore support is missing for now */
	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_is_thin_type(lvl->lv)) {
			log_error("Cannot restore Volume Group %s with "
				  "thin logical volumes. "
				  "(not yet supported).", vg->name);
			r = 0;
			goto out;
		}

	missing_pvs = vg_missing_pv_count(vg);
	if (missing_pvs == 0)
		r = backup_restore_vg(cmd, vg);
	else
		log_error("Cannot restore Volume Group %s with %i PVs "
			  "marked as missing.", vg->name, missing_pvs);

out:
	release_vg(vg);
	return r;
}

int backup_restore(struct cmd_context *cmd, const char *vg_name)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s",
			 cmd->backup_params->dir, vg_name) < 0) {
		log_error("Failed to generate backup filename (for restore).");
		return 0;
	}

	return backup_restore_from_file(cmd, vg_name, path);
}

int backup_to_file(const char *file, const char *desc, struct volume_group *vg)
{
	int r = 0;
	struct format_instance *tf;
	struct format_instance_ctx fic;
	struct text_context tc = {.path_live = file,
				  .path_edit = NULL,
				  .desc = desc};
	struct metadata_area *mda;
	struct cmd_context *cmd;

	cmd = vg->cmd;

	log_verbose("Creating volume group backup \"%s\" (seqno %u).", file, vg->seqno);

	fic.type = FMT_INSTANCE_PRIVATE_MDAS;
	fic.context.private = &tc;
	if (!(tf = cmd->fmt_backup->ops->create_instance(cmd->fmt_backup, &fic))) {
		log_error("Couldn't create backup object.");
		return 0;
	}

	if (!dm_list_size(&tf->metadata_areas_in_use)) {
		log_error(INTERNAL_ERROR "No in use metadata areas to write.");
		tf->fmt->ops->destroy_instance(tf);
		return 0;
	}

	/* Write and commit the metadata area */
	dm_list_iterate_items(mda, &tf->metadata_areas_in_use) {
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

/*
 * Update backup (and archive) if they're out-of-date or don't exist.
 */
void check_current_backup(struct volume_group *vg)
{
	char path[PATH_MAX];
	struct volume_group *vg_backup;
	int old_suppress;

	if (vg_is_exported(vg))
		return;

	if (dm_snprintf(path, sizeof(path), "%s/%s",
			 vg->cmd->backup_params->dir, vg->name) < 0) {
		log_debug("Failed to generate backup filename.");
		return;
	}

	old_suppress = log_suppress(1);
	/* Up-to-date backup exists? */
	if ((vg_backup = backup_read_vg(vg->cmd, vg->name, path)) &&
	    (vg->seqno == vg_backup->seqno) &&
	    (id_equal(&vg->id, &vg_backup->id))) {
		log_suppress(old_suppress);
		release_vg(vg_backup);
		return;
	}
	log_suppress(old_suppress);

	if (vg_backup) {
		if (!archive(vg_backup))
			stack;
		release_vg(vg_backup);
	}
	if (!archive(vg))
		stack;
	if (!backup_locally(vg))
		stack;
}
