/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
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

#include "config.h"
#include "crc.h"
#include "device.h"
#include "str_list.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "lvm-file.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <assert.h>

struct config_file {
	time_t timestamp;
	off_t st_size;
	char *filename;
	int exists;
	int keep_open;
	struct device *dev;
};

/*
 * public interface
 */
struct dm_config_tree *config_file_open(const char *filename, int keep_open)
{
	struct dm_config_tree *cft = dm_config_create();
	struct config_file *cf;
	if (!cft)
		return NULL;

	cf = dm_pool_zalloc(cft->mem, sizeof(struct config_file));
	if (!cf) goto fail;

	cf->timestamp = 0;
	cf->exists = 0;
	cf->keep_open = keep_open;
	dm_config_set_custom(cft, cf);

	if (filename &&
	    !(cf->filename = dm_pool_strdup(cft->mem, filename))) {
		log_error("Failed to duplicate filename.");
		goto fail;
	}

	return cft;
fail:
	dm_config_destroy(cft);
	return NULL;
}

/*
 * Doesn't populate filename if the file is empty.
 */
int config_file_check(struct dm_config_tree *cft, const char **filename, struct stat *info)
{
	struct config_file *cf = dm_config_get_custom(cft);
	struct stat _info;

	if (!info)
		info = &_info;

	if (stat(cf->filename, info)) {
		log_sys_error("stat", cf->filename);
		cf->exists = 0;
		return 0;
	}

	if (!S_ISREG(info->st_mode)) {
		log_error("%s is not a regular file", cf->filename);
		cf->exists = 0;
		return 0;
	}

	cf->exists = 1;
	cf->timestamp = info->st_ctime;
	cf->st_size = info->st_size;

	if (info->st_size == 0)
		log_verbose("%s is empty", cf->filename);
	else if (filename)
		*filename = cf->filename;

	return 1;
}

/*
 * Return 1 if config files ought to be reloaded
 */
int config_file_changed(struct dm_config_tree *cft)
{
	struct config_file *cf = dm_config_get_custom(cft);
	struct stat info;

	if (!cf->filename)
		return 0;

	if (stat(cf->filename, &info) == -1) {
		/* Ignore a deleted config file: still use original data */
		if (errno == ENOENT) {
			if (!cf->exists)
				return 0;
			log_very_verbose("Config file %s has disappeared!",
					 cf->filename);
			goto reload;
		}
		log_sys_error("stat", cf->filename);
		log_error("Failed to reload configuration files");
		return 0;
	}

	if (!S_ISREG(info.st_mode)) {
		log_error("Configuration file %s is not a regular file",
			  cf->filename);
		goto reload;
	}

	/* Unchanged? */
	if (cf->timestamp == info.st_ctime && cf->st_size == info.st_size)
		return 0;

      reload:
	log_verbose("Detected config file change to %s", cf->filename);
	return 1;
}

void config_file_destroy(struct dm_config_tree *cft)
{
	struct config_file *cf = dm_config_get_custom(cft);

	if (cf && cf->dev)
		dev_close(cf->dev);

	dm_config_destroy(cft);
}

/*
 * Returns config tree if it was removed.
 */
struct dm_config_tree *remove_overridden_config_tree(struct cmd_context *cmd)
{
	struct dm_config_tree *old_cft = cmd->cft;
	struct dm_config_tree *cft = dm_config_remove_cascaded_tree(cmd->cft);

	if (!cft)
		return NULL;

	cmd->cft = cft;

	return old_cft;
}

int override_config_tree_from_string(struct cmd_context *cmd,
				     const char *config_settings)
{
	struct dm_config_tree *cft_new;

	if (!(cft_new = dm_config_from_string(config_settings))) {
		log_error("Failed to set overridden configuration entries.");
		return 1;
	}

	cmd->cft = dm_config_insert_cascaded_tree(cft_new, cmd->cft);

	return 0;
}

int config_file_read_fd(struct dm_config_tree *cft, struct device *dev,
			off_t offset, size_t size, off_t offset2, size_t size2,
			checksum_fn_t checksum_fn, uint32_t checksum)
{
	const char *fb, *fe;
	int r = 0;
	int use_mmap = 1;
	off_t mmap_offset = 0;
	char *buf = NULL;

	/* Only use mmap with regular files */
	if (!(dev->flags & DEV_REGULAR) || size2)
		use_mmap = 0;

	if (use_mmap) {
		mmap_offset = offset % lvm_getpagesize();
		/* memory map the file */
		fb = mmap((caddr_t) 0, size + mmap_offset, PROT_READ,
			  MAP_PRIVATE, dev_fd(dev), offset - mmap_offset);
		if (fb == (caddr_t) (-1)) {
			log_sys_error("mmap", dev_name(dev));
			goto out;
		}
		fb = fb + mmap_offset;
	} else {
		if (!(buf = dm_malloc(size + size2))) {
			log_error("Failed to allocate circular buffer.");
			return 0;
		}
		if (!dev_read_circular(dev, (uint64_t) offset, size,
				       (uint64_t) offset2, size2, buf)) {
			goto out;
		}
		fb = buf;
	}

	if (checksum_fn && checksum !=
	    (checksum_fn(checksum_fn(INITIAL_CRC, (const uint8_t *)fb, size),
			 (const uint8_t *)(fb + size), size2))) {
		log_error("%s: Checksum error", dev_name(dev));
		goto out;
	}

	fe = fb + size + size2;
	if (!dm_config_parse(cft, fb, fe))
		goto_out;

	r = 1;

      out:
	if (!use_mmap)
		dm_free(buf);
	else {
		/* unmap the file */
		if (munmap((char *) (fb - mmap_offset), size + mmap_offset)) {
			log_sys_error("munmap", dev_name(dev));
			r = 0;
		}
	}

	return r;
}

int config_file_read(struct dm_config_tree *cft)
{
	const char *filename = NULL;
	struct config_file *cf = dm_config_get_custom(cft);
	struct stat info;
	int r;

	if (!config_file_check(cft, &filename, &info))
		return_0;

	/* Nothing to do.  E.g. empty file. */
	if (!filename)
		return 1;

	if (!cf->dev) {
		if (!(cf->dev = dev_create_file(filename, NULL, NULL, 1)))
			return_0;

		if (!dev_open_readonly_buffered(cf->dev))
			return_0;
	}

	r = config_file_read_fd(cft, cf->dev, 0, (size_t) info.st_size, 0, 0,
				(checksum_fn_t) NULL, 0);

	if (!cf->keep_open) {
		dev_close(cf->dev);
		cf->dev = NULL;
	}

	return r;
}

time_t config_file_timestamp(struct dm_config_tree *cft)
{
	struct config_file *cf = dm_config_get_custom(cft);
	assert(cf);
	return cf->timestamp;
}

const struct dm_config_node *find_config_tree_node(struct cmd_context *cmd,
						   const char *path)
{
	return dm_config_tree_find_node(cmd->cft, path);
}

const char *find_config_tree_str(struct cmd_context *cmd,
				 const char *path, const char *fail)
{
	return dm_config_tree_find_str(cmd->cft, path, fail);
}

const char *find_config_tree_str_allow_empty(struct cmd_context *cmd,
					     const char *path, const char *fail)
{
	return dm_config_tree_find_str_allow_empty(cmd->cft, path, fail);
}

int find_config_tree_int(struct cmd_context *cmd, const char *path,
			 int fail)
{
	return dm_config_tree_find_int(cmd->cft, path, fail);
}

int64_t find_config_tree_int64(struct cmd_context *cmd, const char *path, int64_t fail)
{
	return dm_config_tree_find_int64(cmd->cft, path, fail);
}

float find_config_tree_float(struct cmd_context *cmd, const char *path,
			     float fail)
{
	return dm_config_tree_find_float(cmd->cft, path, fail);
}

int find_config_tree_bool(struct cmd_context *cmd, const char *path, int fail)
{
	return dm_config_tree_find_bool(cmd->cft, path, fail);
}

/* Insert cn2 after cn1 */
static void _insert_config_node(struct dm_config_node **cn1,
				struct dm_config_node *cn2)
{
	if (!*cn1) {
		*cn1 = cn2;
		cn2->sib = NULL;
	} else {
		cn2->sib = (*cn1)->sib;
		(*cn1)->sib = cn2;
	}
}

/*
 * Merge section cn2 into section cn1 (which has the same name)
 * overwriting any existing cn1 nodes with matching names.
 */
static void _merge_section(struct dm_config_node *cn1, struct dm_config_node *cn2)
{
	struct dm_config_node *cn, *nextn, *oldn;
	struct dm_config_value *cv;

	for (cn = cn2->child; cn; cn = nextn) {
		nextn = cn->sib;

		/* Skip "tags" */
		if (!strcmp(cn->key, "tags"))
			continue;

		/* Subsection? */
		if (!cn->v)
			/* Ignore - we don't have any of these yet */
			continue;
		/* Not already present? */
		if (!(oldn = dm_config_find_node(cn1->child, cn->key))) {
			_insert_config_node(&cn1->child, cn);
			continue;
		}
		/* Merge certain value lists */
		if ((!strcmp(cn1->key, "activation") &&
		     !strcmp(cn->key, "volume_list")) ||
		    (!strcmp(cn1->key, "devices") &&
		     (!strcmp(cn->key, "filter") || !strcmp(cn->key, "types")))) {
			cv = cn->v;
			while (cv->next)
				cv = cv->next;
			cv->next = oldn->v;
		}

		/* Replace values */
		oldn->v = cn->v;
	}
}

static int _match_host_tags(struct dm_list *tags, const struct dm_config_node *tn)
{
	const struct dm_config_value *tv;
	const char *str;

	for (tv = tn->v; tv; tv = tv->next) {
		if (tv->type != DM_CFG_STRING)
			continue;
		str = tv->v.str;
		if (*str == '@')
			str++;
		if (!*str)
			continue;
		if (str_list_match_item(tags, str))
			return 1;
	}

	return 0;
}

/* Destructively merge a new config tree into an existing one */
int merge_config_tree(struct cmd_context *cmd, struct dm_config_tree *cft,
		      struct dm_config_tree *newdata)
{
	struct dm_config_node *root = cft->root;
	struct dm_config_node *cn, *nextn, *oldn, *cn2;
	const struct dm_config_node *tn;

	for (cn = newdata->root; cn; cn = nextn) {
		nextn = cn->sib;
		/* Ignore tags section */
		if (!strcmp(cn->key, "tags"))
			continue;
		/* If there's a tags node, skip if host tags don't match */
		if ((tn = dm_config_find_node(cn->child, "tags"))) {
			if (!_match_host_tags(&cmd->tags, tn))
				continue;
		}
		if (!(oldn = dm_config_find_node(root, cn->key))) {
			_insert_config_node(&cft->root, cn);
			/* Remove any "tags" nodes */
			for (cn2 = cn->child; cn2; cn2 = cn2->sib) {
				if (!strcmp(cn2->key, "tags")) {
					cn->child = cn2->sib;
					continue;
				}
				if (cn2->sib && !strcmp(cn2->sib->key, "tags")) {
					cn2->sib = cn2->sib->sib;
					continue;
				}
			}
			continue;
		}
		_merge_section(oldn, cn);
	}

	return 1;
}

static int _putline_fn(const char *line, void *baton) {
	FILE *fp = baton;
	fprintf(fp, "%s\n", line);
	return 1;
};

int config_write(struct dm_config_tree *cft, const char *file,
		 int argc, char **argv)
{
	const struct dm_config_node *cn;
	int r = 1;
	FILE *fp = NULL;

	if (!file) {
		fp = stdout;
		file = "stdout";
	} else if (!(fp = fopen(file, "w"))) {
		log_sys_error("open", file);
		return 0;
	}

	log_verbose("Dumping configuration to %s", file);
	if (!argc) {
		if (!dm_config_write_node(cft->root, _putline_fn, fp)) {
			log_error("Failure while writing to %s", file);
			r = 0;
		}
	} else while (argc--) {
		if ((cn = dm_config_find_node(cft->root, *argv))) {
			if (!dm_config_write_node(cn, _putline_fn, fp)) {
				log_error("Failure while writing to %s", file);
				r = 0;
			}
		} else {
			log_error("Configuration node %s not found", *argv);
			r = 0;
		}
		argv++;
	}

	if (fp && dm_fclose(fp)) {
		stack;
		r = 0;
	}

	return r;
}
