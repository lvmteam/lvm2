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
#include "import-export.h"
#include "lvm-string.h"

#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>


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

	/*
	 * An ordered list of previous backups.
	 * Each list entered against the vg name.
	 * Most recent first.
	 */
	struct hash_table *vg_backups;

	/*
	 * Scratch pool.  Contents of vg_backups
	 * come from here.
	 */
	struct pool *mem;
};

/*
 * A list of these is built up for each volume
 * group.  Ordered with the least recent at the
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

static struct list *_get_vgs(struct format_instance *fi)
{
	_unsupported("get_vgs");
	return NULL;
}

static struct list *_get_pvs(struct format_instance *fi)
{
	_unsupported("get_pvs");
	return NULL;
}

static struct physical_volume *_pv_read(struct format_instance *fi,
					const char *pv_name)
{
	_unsupported("pv_read");
	return NULL;
}

static int _pv_setup(struct format_instance *fi, struct physical_volume *pv,
	     struct volume_group *vg)
{
	_unsupported("pv_setup");
	return 0;
}

static int _pv_write(struct format_instance *fi, struct physical_volume *pv)
{
	_unsupported("pv_write");
	return 0;
}

static int _vg_setup(struct format_instance *fi, struct volume_group *vg)
{
	_unsupported("vg_setup");
	return 0;
}

static struct volume_group *_vg_read(struct format_instance *fi,
				     const char *vg_name)
{
	_unsupported("vg_read");
	return NULL;
}

static void _destroy(struct format_instance *fi)
{
	struct backup_c *bc = (struct backup_c *) fi->private;
	if (bc->vg_backups)
		hash_destroy(bc->vg_backups);
	pool_destroy(bc->mem);
}


/*
 * vg_write implementation starts here.
 */

/*
 * Extract vg name and version number from a filename
 */
static int _split_vg(const char *filename, char *vg, size_t vg_size,
		     uint32_t *index)
{
	int len, vg_len;
	char *dot, *underscore;

	len = strlen(filename);
	if (len < 7)
		return 0;

	dot = (char *) (filename + len - 3);
	if (strcmp(".vg", dot))
		return 0;

	if (!(underscore = rindex(filename, '_')))
		return 0;

	if (sscanf(underscore + 1, "%u", index) != 1)
		return 0;

	vg_len = underscore - filename;
	if (vg_len + 1 > vg_size)
		return 0;

	strncpy(vg, filename, vg_len);
	vg[vg_len] = '\0';

	return 1;
}

static void _insert_file(struct list *head, struct backup_file *b)
{
	struct list *bh;
	struct backup_file *bf;

	if (list_empty(head)) {
		list_add(head, &b->list);
		return;
	}

	/* index increases through list */
	list_iterate (bh, head) {
		bf = list_item(bh, struct backup_file);

		if (bf->index > b->index) {
			list_add(&bf->list, &b->list);
			return;
		}
	}

	list_add_h(&bf->list, &b->list);
}

static int _scan_vg(struct backup_c *bc, const char *file,
		    const char *vg_name, int index)
{
	struct backup_file *b;
	struct list *files;

	/*
	 * Do we need to create a new list of
	 * backup files for this vg ?
	 */
	if (!(files = hash_lookup(bc->vg_backups, vg_name))) {
		if (!(files = pool_alloc(bc->mem, sizeof(*files)))) {
			stack;
			return 0;
		}

		list_init(files);
		if (!hash_insert(bc->vg_backups, vg_name, files)) {
			log_err("Couldn't insert backup file "
				"into hash table.");
			return 0;
		}
	}

	/*
	 * Create a new backup file.
	 */
	if (!(b = pool_alloc(bc->mem, sizeof(*b)))) {
		log_err("Couldn't create new backup file.");
		return 0;
	}

	b->index = index;
	b->path = (char *)file;
	b->vg = (char *)vg_name;

	/*
	 * Insert it to the correct part of the
	 * list.
	 */
	_insert_file(files, b);

	return 1;
}

static char *_join(struct pool *mem, const char *dir, const char *name)
{
	if (!pool_begin_object(mem, 32) ||
	    !pool_grow_object(mem, dir, strlen(dir)) ||
	    !pool_grow_object(mem, "/", 1) ||
	    !pool_grow_object(mem, name, strlen(name)) ||
	    !pool_grow_object(mem, "\0", 1)) {
		stack;
		return NULL;
	}

	return pool_end_object(mem);
}

static int _scan_dir(struct backup_c *bc)
{
	int r = 0, i, count, index;
	char vg_name[64], *path;
	struct dirent **dirent;

	if ((count = scandir(bc->dir, &dirent, NULL, alphasort)) < 0) {
		log_err("Couldn't scan backup directory.");
		return 0;
	}

	for (i = 0; i < count; i++) {
		if ((dirent[i]->d_name[0] == '.') ||
		    !_split_vg(dirent[i]->d_name, vg_name,
			       sizeof(vg_name), &index))
			continue;

		if (!(path = _join(bc->mem, bc->dir, dirent[i]->d_name))) {
			stack;
			goto out;
		}

		_scan_vg(bc, path, vg_name, index);
	}
	r = 1;

 out:
	for (i = 0; i < count; i++)
		free(dirent[i]);
	free(dirent);

	return r;
}

static int _scan_backups(struct backup_c *bc)
{
	pool_empty(bc->mem);

	if (bc->vg_backups)
		hash_destroy(bc->vg_backups);

	if (!(bc->vg_backups = hash_create(128))) {
		log_err("Couldn't create hash table for scanning backups.");
		return 0;
	}

	if (!_scan_dir(bc)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Creates a temporary filename, and opens a
 * descriptor to the file.  Both the filename and
 * descriptor are needed so we can rename the file
 * after successfully writing it.
 * Grab NFS-supported exclusive fcntl discretionary lock.
 */
static int _create_temp_name(const char *dir, char *buffer, size_t len,
			     int *fd)
{
	int i, num;
	pid_t pid;
	char hostname[255];
	struct flock lock = {
		l_type: F_WRLCK,
		l_whence: 0,
		l_start: 0,
		l_len: 0
	};

	num = rand();
	pid = getpid();
	if (gethostname(hostname, sizeof(hostname)) < 0) {
		log_sys_error("gethostname", "");
		strcpy(hostname, "nohostname");
	}
	for (i = 0; i < 20; i++, num++) {
		if (lvm_snprintf(buffer, len, "%s/.lvm_%s_%d_%d", 
				 dir, hostname, pid, num) == -1) {
			log_err("Not enough space to build temporary file "
				"string.");
			return 0;
		}

		*fd = open(buffer, O_CREAT | O_EXCL | O_WRONLY | O_APPEND, 
			   S_IRUSR | S_IWUSR);
		if (*fd < 0)
			continue;

		if (!fcntl(*fd, F_SETLK, &lock))
			return 1;	

		close(*fd);
	}

	return 0;
}

/*
 * NFS-safe rename of a temporary file to a common name, designed to
 * avoid race conditions and not overwrite the destination if it exists.
 *
 * Try to create the new filename as a hard link to the original.
 * Check the link count of the original file to see if it worked.
 * (Assumes nothing else touches our temporary file!)
 * If it worked, unlink the old filename.
 */
static int _rename(const char *old, const char *new)
{
	struct stat buf;

	link(old, new);

	if (stat(old, &buf)) {
		log_sys_error("stat", old);
		return 0;
	}

	if (buf.st_nlink != 2) {
		log_error("%s: rename to %s failed", old, new);
		return 0;
	}

	if (unlink(old)) {
		log_sys_error("unlink", old);
		return 0;
	}

	return 1;
}

static int _vg_write(struct format_instance *fi, struct volume_group *vg)
{
	int r = 0, i, fd;
	unsigned int index = 0;
	struct backup_c *bc = (struct backup_c *) fi->private;
	struct backup_file *last;
	FILE *fp = NULL;
	char temp_file[PATH_MAX], backup_name[PATH_MAX];

	/*
	 * Build a format string for mkstemp.
	 */
	if (!_create_temp_name(bc->dir, temp_file, sizeof(temp_file), &fd)) {
		log_err("Couldn't generate template for backup name.");
		return 0;
	}

	if (!(fp = fdopen(fd, "w"))) {
		log_err("Couldn't create FILE object for backup.");
		close(fd);
		return 0;
	}

	if (!text_vg_export(fp, vg)) {
		stack;
		fclose(fp);
		return 0;
	}

	fclose(fp);

	/*
	 * Now we want to rename this file to <vg>_index.vg.
	 */
	if (!_scan_backups(bc)) {
		log_err("Couldn't scan the backup directory (%s).", bc->dir);
		goto out;
	}

	if ((last = (struct backup_file *) hash_lookup(bc->vg_backups,
						       vg->name))) {
		/* move to the last in the list */
		last = list_item(last->list.p, struct backup_file);
		index = last->index + 1;
	}

	for (i = 0; i < 10; i++) {
		if (lvm_snprintf(backup_name, sizeof(backup_name),
				 "%s/%s_%d.vg",
				 bc->dir, vg->name, index) < 0) {
			log_err("backup file name too long.");
			goto out;
		}

		if (_rename(temp_file, backup_name)) {
			r = 1;
			break;
		}

		index++;
	}

 out:
	return r;
}

void backup_expire(struct format_instance *fi)
{
	/* FIXME: finish */
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

struct format_instance *backup_format_create(struct cmd_context *cmd,
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

	if (!(bc->mem = pool_create(1024))) {
		stack;
		goto bad;
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
	fi->ops = &_backup_handler;
	fi->private = bc;

	return fi;

 bad:
	if (bc->mem)
		pool_destroy(bc->mem);

	pool_free(mem, bc);
	return NULL;
}
