/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "format-text.h"

#include "log.h"
#include "pool.h"
#include "config.h"
#include "hash.h"

/*
 * The format instance is given a directory path
 * upon creation.  Each file in this directory
 * whose name is of the form '(.*)_[0-9]*.vg' is a config
 * file (see lib/config.[hc]), which contains a
 * description of a single volume group.
 *
 * The prefix ($1 from the above regex) of the
 * config file gives the volume group name.
 *
 * Backup files that have expired will be removed.
 */

struct backup_c {
	uint32_t retain_days;
	uint32_t min_retains;

	char *dir;
};

/*
 * A list of these is built up for each volume
 * group.  Ordered with the most recent at the
 * head.
 */
struct backup_file {
	struct list list;

	char *path;
	char *vg;
	int index;
};

/*
 * This format is write only.
 */
static void _unsupported(const char *cmd)
{
	log_err("The backup format doesn't support '%s'", cmd);
}

struct list *get_vgs(struct format_instance *fi)
{
	_unsupported("get_vgs");
	return NULL;
}

struct list *get_pvs(struct format_instance *fi)
{
	_unsupported("get_pvs");
	return NULL;
}

struct physical_volume *pv_read(struct format_instance *fi,
				const char *pv_name)
{
	_unsupported("pv_read");
	return NULL;
}

int pv_setup(struct format_instance *fi, struct physical_volume *pv,
	     struct volume_group *vg)
{
	_unsupported("pv_setup");
	return 1;
}

int pv_write(struct format_instance *fi, struct physical_volume *pv)
{
	_unsupported("pv_write");
	return 1;
}

int vg_setup(struct format_instance *fi, struct volume_group *vg)
{
	_unsupported("vg_setup");
	return 1;
}

struct volume_group *vg_read(struct format_instance *fi, const char *vg_name)
{
	_unsupported("vg_read");
	return 1;
}

int vg_write(struct format_instance *fi, struct volume_group *vg)
{
	struct backup_c *bc = (struct backup_c *) fi->private;

}

void destroy(struct format_instance *fi)
{
	/*
	 * We don't need to do anything here since
	 * everything is allocated from the pool.
	 */
}

static int _split_vg(const char *filename, char *vg, size_t vg_size,
		     uint32_t *index)
{
	char buffer[64];
	int n;

	snprintf(buffer, sizeof(buffer), "\%%ds_\%u.vg%n", vg, index, &n);
	return (sscanf(filename, buffer, vg, index) == 2) &&
		(filename + n == '\0');
}

static void _insert_file(struct list *head, struct backup_file *b)
{
	struct list *bh;
	struct backup_file *bf;

	if (list_empty(head)) {
		list_add(head, &b->list);
		return;
	}

	list_iterate (bh, head) {
		bf = list_item(bh, struct backup_file);

		if (bf->index < b->index)
			break;
	}

	list_add_h(&bf->list, &b->list);
}

static int _scan_vg(struct pool *mem, struct hash_table *vgs, const char *file)
{
	struct backup_file *b;
	struct list *files;
	char vg_name[256];
	int index;

	/*
	 * Do we need to create a new list of
	 * backup files for this vg ?
	 */
	if (!(files = hash_lookup(vgs, vg_name))) {
		if (!(files = pool_alloc(mem, sizeof(*files)))) {
			stack;
			return 0;
		}

		list_init(files);
		if (!hash_insert(vgs, vg_name, files)) {
			log_err("Couldn't insert backup file "
				"into hash table.");
			return 0;
		}
	}

	/*
	 * Create a new backup file.
	 */
	if (!(b = pool_alloc(mem, sizeof(*b)))) {
		log_err("Couldn't create new backup file.");
		return 0;
	}

	/*
	 * Insert it to the correct part of the
	 * list.
	 */
	_insert_file(files, b);

	return 1;
}

static int _scan_dir(struct pool *mem, struct hash_table *vgs, const char *dir)
{
	int r = 0;

	if ((count = scandir(dir, &dirent, NULL, alphasort)) < 0) {
		log_err("Couldn't scan backup directory.");
		return NULL;
	}

	for (i = 0; i < count; i++) {
		if ((dirent[i]->d_name[0] == '.') ||
		    !_split_vg(dirent[i]->d_name))
			continue;

		if (!(path = _join(mem, dir, dirent[i]->d_name))) {
			stack;
			goto out;
		}

		_scan_vg(path);
	}
	r = 1;

 out:
	for (i = 0; i < count; i++)
		free(dirent[i]);
	free(dirent);

	return r;
}

struct hash_table *_scan_backups(const char *dir)
{
	int count;
	struct dirent **dirent;
	struct hash_table *h = NULL;

	if (!(h = hash_create(128))) {
		log_err("Couldn't create hash table for scanning backups.");
		return NULL;
	}

	if (!_scan_vgs(mem, h, dir)) {
		stack;
		hash_destroy(h);
		return NULL;
	}

	return h;
}

void backup_expire(struct format_instance *fi)
{
	struct backup_c *bc = (struct backup_c *) fi->private;
	struct hash_table *vgs;

	
}

static struct format_handler _backup_handler = {
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

struct format_instance *backup_format_create(struct cmd_context,
					     const char *dir,
					     uint32_t retain_days,
					     uint32_t min_retains)
{
	struct format_instance *fi;
	struct backup_c *bc = NULL;
	struct pool *mem = cmd->mem;

	if (!(bc = pool_zalloc(mem, sizeof(*bc)))) {
		stack;
		return NULL;
	}

	if (!(bc->dir = pool_strdup(mem, dir))) {
		stack;
		goto bad;
	}

	bc->retain_days = retain_days;
	bc->min_retains = min_retains;

	if (!(fi = pool_alloc(mem, sizeof(*fi)))) {
		stack;
		goto bad;
	}

	fi->cmd = cmd;
	fi->ops = _backup_handler;
	fi->private = bc;

	return fi;

 bad:
	pool_free(mem, bc);
	return NULL;
}
