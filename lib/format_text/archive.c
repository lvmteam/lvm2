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
#include "lvm-file.h"
#include "toolcontext.h"

#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <time.h>

/*
 * The format instance is given a directory path upon creation.
 * Each file in this directory whose name is of the form
 * '(.*)_[0-9]*.vg' is a config file (see lib/config.[hc]), which
 * contains a description of a single volume group.
 *
 * The prefix ($1 from the above regex) of the config file gives
 * the volume group name.
 *
 * Backup files that have expired will be removed.
 */

/*
 * A list of these is built up for our volume group.  Ordered
 * with the least recent at the head.
 */
struct archive_file {
	struct list list;

	char *path;
	int index;
};

/*
 * Extract vg name and version number from a filename.
 */
static int _split_vg(const char *filename, char *vg, size_t vg_size,
		     uint32_t * index)
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

static void _insert_file(struct list *head, struct archive_file *b)
{
	struct list *bh;
	struct archive_file *bf;

	if (list_empty(head)) {
		list_add(head, &b->list);
		return;
	}

	/* index increases through list */
	list_iterate(bh, head) {
		bf = list_item(bh, struct archive_file);

		if (bf->index > b->index) {
			list_add(&bf->list, &b->list);
			return;
		}
	}

	list_add_h(&bf->list, &b->list);
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

/*
 * Returns a list of archive_files.
 */
static struct list *_scan_archive(struct pool *mem,
				  const char *vg, const char *dir)
{
	int i, count, index;
	char vg_name[64], *path;
	struct dirent **dirent;
	struct archive_file *af;
	struct list *results;

	if (!(results = pool_alloc(mem, sizeof(*results)))) {
		stack;
		return NULL;
	}

	list_init(results);

	if ((count = scandir(dir, &dirent, NULL, alphasort)) < 0) {
		log_err("Couldn't scan archive directory.");
		return 0;
	}

	for (i = 0; i < count; i++) {
		/* ignore dot files */
		if (dirent[i]->d_name[0] == '.')
			continue;

		/* check the name is the correct format */
		if (!_split_vg(dirent[i]->d_name, vg_name,
			       sizeof(vg_name), &index))
			continue;

		/* is it the vg we're interested in ? */
		if (strcmp(vg, vg_name))
			continue;

		if (!(path = _join(mem, dir, dirent[i]->d_name))) {
			stack;
			goto out;
		}

		/*
		 * Create a new archive_file.
		 */
		if (!(af = pool_alloc(mem, sizeof(*af)))) {
			log_err("Couldn't create new archive file.");
			results = NULL;
			goto out;
		}

		af->index = index;
		af->path = path;

		/*
		 * Insert it to the correct part of the list.
		 */
		_insert_file(results, af);
	}

      out:
	for (i = 0; i < count; i++)
		free(dirent[i]);
	free(dirent);

	return results;
}

int archive_vg(struct volume_group *vg,
	       const char *dir, const char *desc,
	       uint32_t retain_days, uint32_t min_archive)
{
	int i, fd;
	unsigned int index = 0;
	struct archive_file *last;
	FILE *fp = NULL;
	char temp_file[PATH_MAX], archive_name[PATH_MAX];
	struct list *archives;

	/*
	 * Write the vg out to a temporary file.
	 */
	if (!create_temp_name(dir, temp_file, sizeof(temp_file), &fd)) {
		log_err("Couldn't create temporary archive name.");
		return 0;
	}

	if (!(fp = fdopen(fd, "w"))) {
		log_err("Couldn't create FILE object for archive.");
		close(fd);
		return 0;
	}

	if (!text_vg_export(fp, vg, desc)) {
		stack;
		fclose(fp);
		return 0;
	}

	fclose(fp);

	/*
	 * Now we want to rename this file to <vg>_index.vg.
	 */
	if (!(archives = _scan_archive(vg->cmd->mem, vg->name, dir))) {
		log_err("Couldn't scan the archive directory (%s).", dir);
		return 0;
	}

	if (list_empty(archives))
		index = 0;

	else {
		last = list_item(archives->p, struct archive_file);
		index = last->index + 1;
	}

	for (i = 0; i < 10; i++) {
		if (lvm_snprintf(archive_name, sizeof(archive_name),
				 "%s/%s_%05d.vg", dir, vg->name, index) < 0) {
			log_err("archive file name too long.");
			return 0;
		}

		if (lvm_rename(temp_file, archive_name))
			break;

		index++;
	}

	return 1;
}

static void _display_archive(struct cmd_context *cmd, struct uuid_map *um,
			     struct archive_file *af)
{
	struct volume_group *vg = NULL;
	struct format_instance *tf;
	time_t when;
	char *desc;
	void *context;

	log_print("path:\t\t%s", af->path);

	if (!(context = create_text_context(cmd->fmtt, af->path, NULL)) ||
	    !(tf = cmd->fmtt->ops->create_instance(cmd->fmtt, NULL, context))) {
		log_error("Couldn't create text instance object.");
		return;
	}

	/*
	 * Read the archive file to ensure that it is valid, and
	 * retrieve the archive time and description.
	 */
	/* FIXME Use variation on _vg_read */
	if (!(vg = text_vg_import(tf, af->path, um, &when, &desc))) {
		log_print("Unable to read archive file.");
		tf->fmt->ops->destroy_instance(tf);
		return;
	}

	log_print("description:\t%s", desc ? desc : "<No description>");
	log_print("time:\t\t%s", ctime(&when));

	pool_free(cmd->mem, vg);
	tf->fmt->ops->destroy_instance(tf);
}

int archive_list(struct cmd_context *cmd, struct uuid_map *um,
		 const char *dir, const char *vg)
{
	struct list *archives, *ah;
	struct archive_file *af;

	if (!(archives = _scan_archive(cmd->mem, vg, dir))) {
		log_err("Couldn't scan the archive directory (%s).", dir);
		return 0;
	}

	if (list_empty(archives))
		log_print("No archives found.");

	list_iterate(ah, archives) {
		af = list_item(ah, struct archive_file);

		_display_archive(cmd, um, af);
		log_print(" ");
	}

	pool_free(cmd->mem, archives);

	return 1;
}
