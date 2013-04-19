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
#include "lvm-file.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <ctype.h>

struct config_file {
	time_t timestamp;
	off_t st_size;
	char *filename;
	int exists;
	int keep_open;
	struct device *dev;
};

static char _cfg_path[CFG_PATH_MAX_LEN];

/*
 * Map each ID to respective definition of the configuration item.
 */
static struct cfg_def_item _cfg_def_items[CFG_COUNT + 1] = {
#define cfg_section(id, name, parent, flags, since_version, comment) {id, parent, name, CFG_TYPE_SECTION, {0}, flags, since_version, comment},
#define cfg(id, name, parent, flags, type, default_value, since_version, comment) {id, parent, name, type, {.v_##type = default_value}, flags, since_version, comment},
#define cfg_array(id, name, parent, flags, types, default_value, since_version, comment) {id, parent, name, CFG_TYPE_ARRAY | types, {.v_CFG_TYPE_STRING = default_value}, flags, since_version, comment},
#include "config_settings.h"
#undef cfg_section
#undef cfg
#undef cfg_array
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
		if (!dev_close(cf->dev))
			stack;

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
		return 0;
	}

	cmd->cft = dm_config_insert_cascaded_tree(cft_new, cmd->cft);

	return 1;
}

int config_file_read_fd(struct dm_config_tree *cft, struct device *dev,
			off_t offset, size_t size, off_t offset2, size_t size2,
			checksum_fn_t checksum_fn, uint32_t checksum)
{
	char *fb, *fe;
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
		if (munmap(fb - mmap_offset, size + mmap_offset)) {
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
		if (!dev_close(cf->dev))
			stack;
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

#define cfg_def_get_item_p(id) (&_cfg_def_items[id])
#define cfg_def_get_default_value(item,type) item->default_value.v_##type
#define cfg_def_get_path(item) (_cfg_def_make_path(_cfg_path,CFG_PATH_MAX_LEN,item->id,item),_cfg_path)

static int _cfg_def_make_path(char *buf, size_t buf_size, int id, cfg_def_item_t *item)
{
	int parent_id = item->parent;
	int count, n;

	if (id == parent_id)
		return 0;

	count = _cfg_def_make_path(buf, buf_size, parent_id, cfg_def_get_item_p(parent_id));
	if ((n = dm_snprintf(buf + count, buf_size - count, "%s%s",
			     count ? "/" : "",
			     item->flags & CFG_NAME_VARIABLE ? "#" : item->name)) < 0) {
		log_error(INTERNAL_ERROR "_cfg_def_make_path: supplied buffer too small for %s/%s",
					  cfg_def_get_item_p(parent_id)->name, item->name);
		buf[0] = '\0';
		return 0;
	}

	return count + n;
}

int config_def_get_path(char *buf, size_t buf_size, int id)
{
	return _cfg_def_make_path(buf, buf_size, id, cfg_def_get_item_p(id));
}

static void _get_type_name(char *buf, size_t buf_size, cfg_def_type_t type)
{
	buf[0] = '\0';

	if (type & CFG_TYPE_ARRAY) {
		if (type & ~CFG_TYPE_ARRAY)
			strncat(buf, " array with values of type:", buf_size);
		else {
			strncat(buf, " array", buf_size);
			return;
		}
	}

	if (type & CFG_TYPE_SECTION)
		strncat(buf, " section", buf_size);
	if (type & CFG_TYPE_BOOL)
		strncat(buf, " boolean", buf_size);
	if (type & CFG_TYPE_INT)
		strncat(buf, " integer", buf_size);
	if (type & CFG_TYPE_FLOAT)
		strncat(buf, " float", buf_size);
	if (type & CFG_TYPE_STRING)
		strncat(buf, " string", buf_size);
}

static void _log_type_error(const char *path, cfg_def_type_t actual,
			    cfg_def_type_t expected, int suppress_messages)
{
	static char actual_type_name[64];
	static char expected_type_name[64];

	_get_type_name(actual_type_name, sizeof(actual_type_name), actual);
	_get_type_name(expected_type_name, sizeof(expected_type_name), expected);

	log_warn_suppress(suppress_messages, "Configuration setting \"%s\" has invalid type. "
					     "Found%s, expected%s.", path,
					     actual_type_name, expected_type_name);
}

static int _config_def_check_node_single_value(const char *rp, const struct dm_config_value *v,
					       const cfg_def_item_t *def, int suppress_messages)
{
	/* Check empty array first if present. */
	if (v->type == DM_CFG_EMPTY_ARRAY) {
		if (!(def->type & CFG_TYPE_ARRAY)) {
			_log_type_error(rp, CFG_TYPE_ARRAY, def->type, suppress_messages);
			return 0;
		}
		if (!(def->flags & CFG_ALLOW_EMPTY)) {
			log_warn_suppress(suppress_messages,
				"Configuration setting \"%s\" invalid. Empty value not allowed.", rp);
			return 0;
		}
		return 1;
	}

	switch (v->type) {
		case DM_CFG_INT:
			if (!(def->type & CFG_TYPE_INT) && !(def->type & CFG_TYPE_BOOL)) {
				_log_type_error(rp, CFG_TYPE_INT, def->type, suppress_messages);
				return 0;
			}
			break;
		case DM_CFG_FLOAT:
			if (!(def->type & CFG_TYPE_FLOAT)) {
				_log_type_error(rp, CFG_TYPE_FLOAT, def->type, suppress_messages);
				return 0;
			}
			break;
		case DM_CFG_STRING:
			if (def->type & CFG_TYPE_BOOL) {
				if (!dm_config_value_is_bool(v)) {
					log_warn_suppress(suppress_messages,
						"Configuration setting \"%s\" invalid. "
						"Found string value \"%s\", "
						"expected boolean value: 0/1, \"y/n\", "
						"\"yes/no\", \"on/off\", "
						"\"true/false\".", rp, v->v.str);
					return 0;
				}
			} else if  (!(def->type & CFG_TYPE_STRING)) {
				_log_type_error(rp, CFG_TYPE_STRING, def->type, suppress_messages);
				return 0;
			}
			break;
		default: ;
	}

	return 1;
}

static int _config_def_check_node_value(const char *rp, const struct dm_config_value *v,
					const cfg_def_item_t *def, int suppress_messages)
{
	if (!v) {
		if (def->type != CFG_TYPE_SECTION) {
			_log_type_error(rp, CFG_TYPE_SECTION, def->type, suppress_messages);
			return 0;
		}
		return 1;
	}

	if (v->next) {
		if (!(def->type & CFG_TYPE_ARRAY)) {
			_log_type_error(rp, CFG_TYPE_ARRAY, def->type, suppress_messages);
			return 0;
		}
	}

	do {
		if (!_config_def_check_node_single_value(rp, v, def, suppress_messages))
			return 0;
		v = v->next;
	} while (v);

	return 1;
}

static int _config_def_check_node(const char *vp, char *pvp, char *rp, char *prp,
				  size_t buf_size, struct dm_config_node *cn,
				  struct dm_hash_table *ht, int suppress_messages)
{
	cfg_def_item_t *def;
	int sep = vp != pvp; /* don't use '/' separator for top-level node */

	if (dm_snprintf(pvp, buf_size, "%s%s", sep ? "/" : "", cn->key) < 0 ||
	    dm_snprintf(prp, buf_size, "%s%s", sep ? "/" : "", cn->key) < 0) {
		log_error("Failed to construct path for configuration node %s.", cn->key);
		return 0;
	}


	if (!(def = (cfg_def_item_t *) dm_hash_lookup(ht, vp))) {
		/* If the node is not a section but a setting, fail now. */
		if (cn->v) {
			log_warn_suppress(suppress_messages,
				"Configuration setting \"%s\" unknown.", rp);
			cn->id = -1;
			return 0;
		}

		/* If the node is a section, try if the section name is variable. */
		/* Modify virtual path vp in situ and replace the key name with a '#'. */
		/* The real path without '#' is still stored in rp variable. */
		pvp[sep] = '#', pvp[sep + 1] = '\0';
		if (!(def = (cfg_def_item_t *) dm_hash_lookup(ht, vp))) {
			log_warn_suppress(suppress_messages,
				"Configuration section \"%s\" unknown.", rp);
			cn->id = -1;
			return 0;
		}
	}

	def->flags |= CFG_USED;
	cn->id = def->id;

	if (!_config_def_check_node_value(rp, cn->v, def, suppress_messages))
	return 0;

	def->flags |= CFG_VALID;
	return 1;
}

static int _config_def_check_tree(const char *vp, char *pvp, char *rp, char *prp,
				  size_t buf_size, struct dm_config_node *root,
				  struct dm_hash_table *ht, int suppress_messages)
{
	struct dm_config_node *cn;
	int valid, r = 1;
	size_t len;

	for (cn = root->child; cn; cn = cn->sib) {
		if ((valid = _config_def_check_node(vp, pvp, rp, prp, buf_size,
					cn, ht, suppress_messages)) && !cn->v) {
			len = strlen(rp);
			valid = _config_def_check_tree(vp, pvp + strlen(pvp),
					rp, prp + len, buf_size - len, cn, ht,
					suppress_messages);
		}
		if (!valid)
			r = 0;
	}

	return r;
}

int config_def_check(struct cmd_context *cmd, int force, int skip, int suppress_messages)
{
	cfg_def_item_t *def;
	struct dm_config_node *cn;
	char *vp = _cfg_path, rp[CFG_PATH_MAX_LEN];
	size_t rplen;
	int id, r = 1;

	/*
	 * vp = virtual path, it might contain substitutes for variable parts
	 * 	of the path, used while working with the hash
	 * rp = real path, the real path of the config element as found in the
	 *      configuration, used for message output
	 */

	/*
	 * If the check has already been done and 'skip' is set,
	 * skip the actual check and use last result if available.
	 * If not available, we must do the check. The global status
	 * is stored in root node.
	 */
	def = cfg_def_get_item_p(root_CFG_SECTION);
	if (skip && (def->flags & CFG_USED))
		return def->flags & CFG_VALID;

	/* Clear 'used' and 'valid' status flags. */
	for (id = 0; id < CFG_COUNT; id++) {
		def = cfg_def_get_item_p(id);
		def->flags &= ~(CFG_USED | CFG_VALID);
	}

	if (!force && !find_config_tree_bool(cmd, config_checks_CFG)) {
		if (cmd->cft_def_hash) {
			dm_hash_destroy(cmd->cft_def_hash);
			cmd->cft_def_hash = NULL;
		}
		return 1;
	}

	/*
	 * Create a hash of all possible configuration
	 * sections and settings with full path as a key.
	 * If section name is variable, use '#' as a substitute.
	 */
	if (!cmd->cft_def_hash) {
		if (!(cmd->cft_def_hash = dm_hash_create(64))) {
			log_error("Failed to create configuration definition hash.");
			r = 0; goto out;
		}
		for (id = 1; id < CFG_COUNT; id++) {
			def = cfg_def_get_item_p(id);
			if (!cfg_def_get_path(def)) {
				dm_hash_destroy(cmd->cft_def_hash);
				cmd->cft_def_hash = NULL;
				r = 0; goto out;
			}
			if (!dm_hash_insert(cmd->cft_def_hash, vp, def)) {
				log_error("Failed to insert configuration to hash.");
				r = 0;
				goto out;
			}
		}
	}

	cfg_def_get_item_p(root_CFG_SECTION)->flags |= CFG_USED;

	/*
	 * Allow only sections as top-level elements.
	 * Iterate top-level sections and dive deeper.
	 * If any of subsequent checks fails, the whole check fails.
	 */
	for (cn = cmd->cft->root; cn; cn = cn->sib) {
		if (!cn->v) {
			/* top level node: vp=vp, rp=rp */
			if (!_config_def_check_node(vp, vp, rp, rp,
						    CFG_PATH_MAX_LEN,
						    cn, cmd->cft_def_hash,
						    suppress_messages)) {
				r = 0; continue;
			}
			rplen = strlen(rp);
			if (!_config_def_check_tree(vp, vp + strlen(vp),
						    rp, rp + rplen,
						    CFG_PATH_MAX_LEN - rplen,
						    cn, cmd->cft_def_hash,
						    suppress_messages))
				r = 0;
		} else {
			log_error_suppress(suppress_messages,
				"Configuration setting \"%s\" invalid. "
				"It's not part of any section.", cn->key);
			r = 0;
		}
	}
out:
	if (r) {
		cfg_def_get_item_p(root_CFG_SECTION)->flags |= CFG_VALID;
		def->flags |= CFG_VALID;
	} else {
		cfg_def_get_item_p(root_CFG_SECTION)->flags &= ~CFG_VALID;
		def->flags &= ~CFG_VALID;
	}
	return r;
}

const struct dm_config_node *find_config_tree_node(struct cmd_context *cmd, int id)
{
	return dm_config_tree_find_node(cmd->cft, cfg_def_get_path(cfg_def_get_item_p(id)));
}

const char *find_config_tree_str(struct cmd_context *cmd, int id)
{
	cfg_def_item_t *item = cfg_def_get_item_p(id);
	const char *path = cfg_def_get_path(item);

	if (item->type != CFG_TYPE_STRING)
		log_error(INTERNAL_ERROR "%s cfg tree element not declared as string.", path);

	return dm_config_tree_find_str(cmd->cft, path, cfg_def_get_default_value(item, CFG_TYPE_STRING));
}

const char *find_config_tree_str_allow_empty(struct cmd_context *cmd, int id)
{
	cfg_def_item_t *item = cfg_def_get_item_p(id);
	const char *path = cfg_def_get_path(item);

	if (item->type != CFG_TYPE_STRING)
		log_error(INTERNAL_ERROR "%s cfg tree element not declared as string.", path);
	if (!(item->flags & CFG_ALLOW_EMPTY))
		log_error(INTERNAL_ERROR "%s cfg tree element not declared to allow empty values.", path);

	return dm_config_tree_find_str_allow_empty(cmd->cft, path, cfg_def_get_default_value(item, CFG_TYPE_STRING));
}

int find_config_tree_int(struct cmd_context *cmd, int id)
{
	cfg_def_item_t *item = cfg_def_get_item_p(id);
	const char *path = cfg_def_get_path(item);

	if (item->type != CFG_TYPE_INT)
		log_error(INTERNAL_ERROR "%s cfg tree element not declared as integer.", path);

	return dm_config_tree_find_int(cmd->cft, path, cfg_def_get_default_value(item, CFG_TYPE_INT));
}

int64_t find_config_tree_int64(struct cmd_context *cmd, int id)
{
	cfg_def_item_t *item = cfg_def_get_item_p(id);
	const char *path = cfg_def_get_path(item);

	if (item->type != CFG_TYPE_INT)
		log_error(INTERNAL_ERROR "%s cfg tree element not declared as integer.", path);

	return dm_config_tree_find_int64(cmd->cft, path, cfg_def_get_default_value(item, CFG_TYPE_INT));
}

float find_config_tree_float(struct cmd_context *cmd, int id)
{
	cfg_def_item_t *item = cfg_def_get_item_p(id);
	const char *path = cfg_def_get_path(item);

	if (item->type != CFG_TYPE_FLOAT)
		log_error(INTERNAL_ERROR "%s cfg tree element not declared as float.", path);

	return dm_config_tree_find_float(cmd->cft, path, cfg_def_get_default_value(item, CFG_TYPE_FLOAT));
}

int find_config_tree_bool(struct cmd_context *cmd, int id)
{
	cfg_def_item_t *item = cfg_def_get_item_p(id);
	const char *path = cfg_def_get_path(item);

	if (item->type != CFG_TYPE_BOOL)
		log_error(INTERNAL_ERROR "%s cfg tree element not declared as boolean.", path);

	return dm_config_tree_find_bool(cmd->cft, path, cfg_def_get_default_value(item, CFG_TYPE_BOOL));
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

struct out_baton {
	FILE *fp;
	int withcomment;
	int withversion;
};

static int _out_prefix_fn(const struct dm_config_node *cn, const char *line, void *baton)
{
	struct out_baton *out = baton;
	struct cfg_def_item *cfg_def;
	char version[9]; /* 8+1 chars for max version of 7.15.511 */
	const char *path;
	const char *node_type_name = cn->v ? "option" : "section";

	if (cn->id < 0)
		return 1;

	if (!cn->id) {
		log_error(INTERNAL_ERROR "Configuration node %s has invalid id.", cn->key);
		return 0;
	}

	cfg_def = cfg_def_get_item_p(cn->id);

	if (out->withcomment) {
		path = cfg_def_get_path(cfg_def);
		fprintf(out->fp, "%s# Configuration %s %s.\n", line, node_type_name, path);

		if (cfg_def->comment)
			fprintf(out->fp, "%s# %s\n", line, cfg_def->comment);

		if (cfg_def->flags & CFG_ADVANCED)
			fprintf(out->fp, "%s# This configuration %s is advanced.\n", line, node_type_name);

		if (cfg_def->flags & CFG_UNSUPPORTED)
			fprintf(out->fp, "%s# This configuration %s is not officially supported.\n", line, node_type_name);
	}

	if (out->withversion) {
		if (dm_snprintf(version, 9, "%u.%u.%u",
				(cfg_def->since_version & 0xE000) >> 13,
				(cfg_def->since_version & 0x1E00) >> 9,
				(cfg_def->since_version & 0x1FF)) == -1) {
			log_error("_out_prefix_fn: couldn't create version string");
			return 0;
		}
		fprintf(out->fp, "%s# Since version %s.\n", line, version);
	}

	return 1;
}

static int _out_line_fn(const struct dm_config_node *cn, const char *line, void *baton)
{
	struct out_baton *out = baton;
	fprintf(out->fp, "%s\n", line);
	return 1;
}

static int _out_suffix_fn(const struct dm_config_node *cn, const char *line, void *baton)
{
	return 1;
}

int config_write(struct dm_config_tree *cft,
		 int withcomment, int withversion,
		 const char *file, int argc, char **argv)
{
	struct out_baton baton = {0, 0, 0};
	const struct dm_config_node *cn;
	const struct dm_config_node_out_spec out_spec = {.prefix_fn = _out_prefix_fn,
							 .line_fn = _out_line_fn,
							 .suffix_fn = _out_suffix_fn};
	int r = 1;

	baton.withcomment = withcomment;
	baton.withversion = withversion;

	if (!file) {
		baton.fp = stdout;
		file = "stdout";
	} else if (!(baton.fp = fopen(file, "w"))) {
		log_sys_error("open", file);
		return 0;
	}

	log_verbose("Dumping configuration to %s", file);
	if (!argc) {
		if (!dm_config_write_node_out(cft->root, &out_spec, &baton)) {
			log_error("Failure while writing to %s", file);
			r = 0;
		}
	} else while (argc--) {
		if ((cn = dm_config_find_node(cft->root, *argv))) {
			if (!dm_config_write_one_node_out(cn, &out_spec, &baton)) {
				log_error("Failure while writing to %s", file);
				r = 0;
			}
		} else {
			log_error("Configuration node %s not found", *argv);
			r = 0;
		}
		argv++;
	}

	if (baton.fp && dm_fclose(baton.fp)) {
		stack;
		r = 0;
	}

	return r;
}

static struct dm_config_value *_get_def_array_values(struct dm_config_tree *cft,
						     cfg_def_item_t *def)
{
	char *enc_value, *token, *p, *r;
	struct dm_config_value *array = NULL, *v = NULL, *oldv = NULL;

	if (!def->default_value.v_CFG_TYPE_STRING) {
		if (!(array = dm_config_create_value(cft))) {
			log_error("Failed to create default empty array for %s.", def->name);
			return NULL;
		}
		array->type = DM_CFG_EMPTY_ARRAY;
		return array;
	}

	if (!(p = token = enc_value = dm_strdup(def->default_value.v_CFG_TYPE_STRING))) {
		log_error("_get_def_array_values: dm_strdup failed");
		return NULL;
	}
	/* Proper value always starts with '#'. */
	if (token[0] != '#')
		goto bad;

	while (token) {
		/* Move to type identifier. Error on no char. */
		token++;
		if (!token[0])
			goto bad;

		/* Move to the actual value and decode any "##" into "#". */
		p = token + 1;
		while ((p = strchr(p, '#')) && p[1] == '#') {
			memmove(p, p + 1, strlen(p));
			p++;
		}
		/* Separate the value out of the whole string. */
		if (p)
			p[0] = '\0';

		if (!(v = dm_config_create_value(cft))) {
			log_error("Failed to create default config array value for %s.", def->name);
			dm_free(enc_value);
			return NULL;
		}
		if (oldv)
			oldv->next = v;
		if (!array)
			array = v;

		switch (toupper(token[0])) {
			case 'I':
			case 'B':
				v->v.i = strtoll(token + 1, &r, 10);
				if (*r)
					goto bad;
				v->type = DM_CFG_INT;
				break;
			case 'F':
				v->v.f = strtod(token + 1, &r);
				if (*r)
					goto bad;
				v->type = DM_CFG_FLOAT;
				break;
			case 'S':
				if (!(r = dm_pool_alloc(cft->mem, strlen(token + 1)))) {
					dm_free(enc_value);
					log_error("Failed to duplicate token for default "
						  "array value of %s.", def->name);
					return NULL;
				}
				memcpy(r, token + 1, strlen(token + 1));
				v->v.str = r;
				v->type = DM_CFG_STRING;
				break;
			default:
				goto bad;
		}

		oldv = v;
		token = p;
	}

	dm_free(enc_value);
	return array;
bad:
	log_error(INTERNAL_ERROR "Default array value malformed for \"%s\", "
		  "value: \"%s\", token: \"%s\".", def->name,
		  def->default_value.v_CFG_TYPE_STRING, token);
	dm_free(enc_value);
	return NULL;
}

static struct dm_config_node *_add_def_node(struct dm_config_tree *cft,
					    struct config_def_tree_spec *spec,
					    struct dm_config_node *parent,
					    struct dm_config_node *relay,
					    cfg_def_item_t *def)
{
	struct dm_config_node *cn;
	const char *str;

	if (!(cn = dm_config_create_node(cft, def->name))) {
		log_error("Failed to create default config setting node.");
		return NULL;
	}

	if (!(def->type & CFG_TYPE_SECTION) && (!(cn->v = dm_config_create_value(cft)))) {
		log_error("Failed to create default config setting node value.");
		return NULL;
	}

	cn->id = def->id;

	if (!(def->type & CFG_TYPE_ARRAY)) {
		switch (def->type) {
			case CFG_TYPE_SECTION:
				cn->v = NULL;
				break;
			case CFG_TYPE_BOOL:
				cn->v->type = DM_CFG_INT;
				cn->v->v.i = cfg_def_get_default_value(def, CFG_TYPE_BOOL);
				break;
			case CFG_TYPE_INT:
				cn->v->type = DM_CFG_INT;
				cn->v->v.i = cfg_def_get_default_value(def, CFG_TYPE_INT);
				break;
			case CFG_TYPE_FLOAT:
				cn->v->type = DM_CFG_FLOAT;
				cn->v->v.f = cfg_def_get_default_value(def, CFG_TYPE_FLOAT);
				break;
			case CFG_TYPE_STRING:
				cn->v->type = DM_CFG_STRING;
				if (!(str = cfg_def_get_default_value(def, CFG_TYPE_STRING)))
					str = "";
				cn->v->v.str = str;
				break;
			default:
				log_error(INTERNAL_ERROR "_add_def_node: unknown type");
				return NULL;
				break;
		}
	} else
		cn->v = _get_def_array_values(cft, def);

	cn->child = NULL;
	if (parent) {
		cn->parent = parent;
		if (!parent->child)
			parent->child = cn;
	} else
		cn->parent = cn;

	if (relay)
		relay->sib = cn;

	return cn;
}

static int _should_skip_def_node(struct config_def_tree_spec *spec, int section_id, cfg_def_item_t *def)
{
	if ((def->parent != section_id) ||
	    (spec->ignoreadvanced && def->flags & CFG_ADVANCED) ||
	    (spec->ignoreunsupported && def->flags & CFG_UNSUPPORTED))
		return 1;

	switch (spec->type) {
		case CFG_DEF_TREE_MISSING:
			if ((def->flags & CFG_USED) ||
			    (def->flags & CFG_NAME_VARIABLE) ||
			    (def->since_version > spec->version))
				return 1;
			break;
		case CFG_DEF_TREE_NEW:
			if (def->since_version != spec->version)
				return 1;
			break;
		default:
			if (def->since_version > spec->version)
				return 1;
			break;
	}

	return 0;
}

static struct dm_config_node *_add_def_section_subtree(struct dm_config_tree *cft,
						       struct config_def_tree_spec *spec,
						       struct dm_config_node *parent,
						       struct dm_config_node *relay,
						       int section_id)
{
	struct dm_config_node *cn = NULL, *relay_sub = NULL, *tmp;
	cfg_def_item_t *def;
	int id;

	for (id = 0; id < CFG_COUNT; id++) {
		def = cfg_def_get_item_p(id);
		if (_should_skip_def_node(spec, section_id, def))
			continue;

		if (!cn && !(cn = _add_def_node(cft, spec, parent, relay, cfg_def_get_item_p(section_id))))
				goto bad;

		if ((tmp = def->type == CFG_TYPE_SECTION ? _add_def_section_subtree(cft, spec, cn, relay_sub, id)
							 : _add_def_node(cft, spec, cn, relay_sub, def)))
			relay_sub = tmp;
	}

	return cn;
bad:
	log_error("Failed to create default config section node.");
	return NULL;
}

struct dm_config_tree *config_def_create_tree(struct config_def_tree_spec *spec)
{
	struct dm_config_tree *cft;
	struct dm_config_node *root = NULL, *relay = NULL, *tmp;
	int id;

	if (!(cft = dm_config_create())) {
		log_error("Failed to create default config tree.");
		return NULL;
	}

	for (id = root_CFG_SECTION + 1; id < CFG_COUNT; id++) {
		if (cfg_def_get_item_p(id)->parent != root_CFG_SECTION)
			continue;

		if ((tmp = _add_def_section_subtree(cft, spec, root, relay, id))) {
			relay = tmp;
			if (!root)
				root = relay;
		}
	}

	cft->root = root;
	return cft;
}
