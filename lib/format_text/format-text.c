/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "format-text.h"
#include "import-export.h"

#include "lvm-file.h"
#include "log.h"
#include "pool.h"
#include "config.h"
#include "hash.h"
#include "dbg_malloc.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <limits.h>

/*
 * NOTE: Currently there can be only one vg per file.
 */

static void _not_written(const char *cmd)
{
	log_err("The text format is lacking an implementation for '%s'", cmd);
}

static struct list *_get_vgs(struct format_instance *fi)
{
	_not_written("_get_vgs");
	return NULL;
}

static struct list *_get_pvs(struct format_instance *fi)
{
	_not_written("_get_vgs");
	return NULL;
}

static struct physical_volume *_pv_read(struct format_instance *fi,
					const char *pv_name)
{
	_not_written("_get_vgs");
	return NULL;
}

static int _pv_setup(struct format_instance *fi, struct physical_volume *pv,
		     struct volume_group *vg)
{
	_not_written("_get_vgs");
	return 0;
}

static int _pv_write(struct format_instance *fi, struct physical_volume *pv)
{
	_not_written("_get_vgs");
	return 0;
}

static int _vg_setup(struct format_instance *fi, struct volume_group *vg)
{
	_not_written("_get_vgs");
	return 0;
}

static struct volume_group *_vg_read(struct format_instance *fi,
				     const char *vg_name)
{
	char *file = (char *) fi->private;
	struct volume_group *vg;

	if (!(vg = text_vg_import(fi->cmd, file))) {
		stack;
		return NULL;
	}

	/*
	 * Currently you can only have a single volume group per
	 * text file (this restriction may remain).  We need to
	 * check that it contains the correct volume group.
	 */
	if (strcmp(vg_name, vg->name)) {
		pool_free(fi->cmd->mem, vg);
		log_err("'%s' does not contain volume group '%s'.",
			file, vg_name);
		return NULL;
	}

	return vg;
}

static int _vg_write(struct format_instance *fi, struct volume_group *vg)
{
	FILE *fp;
	int fd;
	char *slash;
	char *file = (char *) fi->private;
	char temp_file[PATH_MAX], temp_dir[PATH_MAX];

	slash = rindex(file, '/');

	if (slash == 0)
		strcpy(temp_dir, ".");
	else if (slash - file < PATH_MAX) {
		strncpy(temp_dir, file, slash - file);
		temp_dir[slash - file] = '\0';

	} else {
		log_error("Text format failed to determine directory.");
		return 0;
	}

        if (!create_temp_name(temp_dir, temp_file, sizeof(temp_file), &fd)) {
                log_err("Couldn't create temporary text file name.");
                return 0;
        }

	if (!(fp = fdopen(fd, "w"))) {
		log_sys_error("fdopen", temp_file);
		close(fd);
		return 0;
	}

	if (!text_vg_export(fp, vg)) {
		log_error("Failed to write metadata to %s.", temp_file);
		fclose(fp);
		return 0;
	}

	if (fclose(fp)) {
		log_sys_error("fclose", file);
		return 0;
	}

	if (rename(temp_file, file)) {
		log_error("%s: rename to %s failed: %s", temp_file, file,
			  strerror(errno));
		return 0;
	}

	return 1;
}

static void _destroy(struct format_instance *fi)
{
	dbg_free(fi->private);
	dbg_free(fi);
}

static struct format_handler _text_handler = {
	get_vgs: _get_vgs,
	get_pvs: _get_pvs,
	pv_read: _pv_read,
	pv_setup: _pv_setup,
	pv_write: _pv_write,
	vg_setup: _vg_setup,
	vg_read: _vg_read,
	vg_write: _vg_write,
	destroy: _destroy
};

struct format_instance *text_format_create(struct cmd_context *cmd,
					   const char *file)
{
	const char *no_alloc = "Couldn't allocate text format object.";

	struct format_instance *fi;
	char *path;

	if (!(fi = dbg_malloc(sizeof(*fi)))) {
		log_err(no_alloc);
		return NULL;
	}

	if (!(path = dbg_strdup(file))) {
		dbg_free(fi);
		log_err(no_alloc);
		return NULL;
	}

	fi->cmd = cmd;
	fi->ops = &_text_handler;
	fi->private = path;

	return fi;
}
