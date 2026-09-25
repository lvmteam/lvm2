/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

static void _set_value_format_flags(struct dm_config_node *cn, uint32_t flags)
{
	while (cn) {
		if (cn->v) {
			struct dm_config_value *v;
			for (v = cn->v; v; v = v->next)
				dm_config_value_set_format_flags(v, v->format_flags | flags);
		}
		if (cn->child)
			_set_value_format_flags(cn->child, flags);
		cn = cn->sib;
	}
}

/*
 * Convert "section/key=value" to LVM config format and append to pool object.
 * Returns 1 for additive edit, 0 for removal (value is "-"), -1 on error.
 * Must only be called while a dm_pool_grow_object is in progress on mem.
 */
static int _edit_to_config_string(struct dm_pool *mem, const char *spec_str, int for_removal)
{
	char str[4096];
	char *eq, *slash, *p, *next;
	const char *value;
	char *parts[CFG_PATH_MAX_LEN];
	int nparts = 0, i;

	if (dm_snprintf(str, sizeof(str), "%s", spec_str) < 0) {
		log_error("Edit specification too long: '%s'", spec_str);
		return -1;
	}

	if (!(eq = strchr(str, '='))) {
		log_error("Invalid edit specification '%s' - expected 'Section/Field=Value'", spec_str);
		return -1;
	}

	*eq = '\0';
	value = eq + 1;

	if (!(slash = strrchr(str, '/'))) {
		log_error("Invalid edit specification '%s' - expected 'Section/Field=Value'", spec_str);
		return -1;
	}

	if (!strcmp(value, "-")) {
		if (!for_removal)
			return 0;
		value = "0";
	} else if (for_removal) {
		return 0;
	}

	/* Split path into section components + field */
	p = str;
	while (p && *p && nparts < CFG_PATH_MAX_LEN) {
		next = strchr(p, '/');
		if (next)
			*next = '\0';
		parts[nparts++] = p;
		p = next ? next + 1 : NULL;
	}

	if (nparts < 2) {
		log_error("Invalid edit specification '%s' - expected 'Section/Field=Value'", spec_str);
		return -1;
	}

	for (i = 0; i < nparts - 1; i++) {
		if (!dm_pool_grow_object(mem, parts[i], strlen(parts[i])) ||
		    !dm_pool_grow_object(mem, " {\n", 3))
			return -1;
	}

	if (!dm_pool_grow_object(mem, parts[nparts - 1], strlen(parts[nparts - 1])) ||
	    !dm_pool_grow_object(mem, "=", 1) ||
	    !dm_pool_grow_object(mem, value, strlen(value)) ||
	    !dm_pool_grow_object(mem, "\n", 1))
		return -1;

	for (i = 0; i < nparts - 1; i++) {
		if (!dm_pool_grow_object(mem, "}\n", 2))
			return -1;
	}

	return 1;
}

static int _check_local_section(const char *section)
{
	return (strcmp(section, "local") == 0 ||
	        strncmp(section, "local/", 6) == 0);
}

static int _get_vsn(struct cmd_context *cmd, uint16_t *version_int)
{
	const char *vsn;
	unsigned int major, minor, patchlevel;

	if (!(vsn = arg_str_value(cmd, atversion_ARG, NULL)) &&
	    !(vsn = arg_str_value(cmd, sinceversion_ARG, NULL)))
		vsn = LVM_VERSION;

	if (sscanf(vsn, "%u.%u.%u", &major, &minor, &patchlevel) != 3) {
		log_error("Incorrect version format.");
		return 0;
	}

	*version_int = vsn(major, minor, patchlevel);
	return 1;
}

static int _do_def_check(struct config_def_tree_spec *spec,
			 struct dm_config_tree *cft,
			 struct cft_check_handle **cft_check_handle)
{
	struct cft_check_handle *handle;

	if (!(handle = get_config_tree_check_handle(spec->cmd, cft)))
		return 0;

	handle->force_check = 1;
	handle->suppress_messages = 1;

	if (spec->type == CFG_DEF_TREE_DIFF || spec->type == CFG_DEF_TREE_FULL_DIFF) {
		if (!handle->check_diff)
			handle->skip_if_checked = 0;
		handle->check_diff = 1;
	} else {
		handle->skip_if_checked = 1;
		handle->check_diff = 0;
	}

	handle->ignoreunsupported = spec->ignoreunsupported;
	handle->ignoreadvanced = spec->ignoreadvanced;

	config_def_check(handle);
	*cft_check_handle = handle;

	return 1;
}

static int _merge_config_cascade(struct cmd_context *cmd, struct dm_config_tree *cft_cascaded,
				 struct dm_config_tree **cft_merged)
{
	if (!cft_cascaded)
		return 1;

	if (!*cft_merged && !(*cft_merged = config_open(CONFIG_MERGED_FILES, NULL, 0)))
		return_0;

	if (!_merge_config_cascade(cmd, cft_cascaded->cascade, cft_merged))
		return_0;

	return merge_config_tree(cmd, *cft_merged, cft_cascaded, CONFIG_MERGE_TYPE_RAW);
}

static int _config_validate(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	struct cft_check_handle *handle;

	if (!(handle = get_config_tree_check_handle(cmd, cft)))
		return 1; /* FIXME: Currently intentional, may hide real error! */

	handle->force_check = 1;
	handle->skip_if_checked = 1;
	handle->suppress_messages = 0;

	return config_def_check(handle);
}

static int _file_has_comments(const char *path)
{
	FILE *fp;
	char buf[512];

	if (!(fp = fopen(path, "r")))
		return 0;

	while (fgets(buf, sizeof(buf), fp)) {
		const char *p = buf;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#') {
			fclose(fp);
			return 1;
		}
	}

	fclose(fp);
	return 0;
}

static const char *_determine_config_file(struct cmd_context *cmd,
					   int has_local, int has_nonlocal)
{
	static char path[PATH_MAX];

	if (has_local && has_nonlocal) {
		log_error("Cannot mix local and non-local sections in a single edit operation.");
		return NULL;
	}

	if (has_local) {
		if (dm_snprintf(path, sizeof(path), "%s/lvmlocal.conf", cmd->system_dir) < 0)
			return NULL;
		return path;
	}

	if (dm_snprintf(path, sizeof(path), "%s/lvm.conf", cmd->system_dir) < 0)
		return NULL;
	return path;
}

static int _create_backup_path(struct cmd_context *cmd, const char *filepath, char *backup_path, size_t len)
{
	char backup_dir[PATH_MAX];
	const char *filename;
	time_t now;
	struct tm *tm;
	char timestamp[64];
	char candidate_path[PATH_MAX * 2];
	int counter;
	struct stat st;
	char sys_dir_prefix[PATH_MAX];
	size_t sys_dir_len;

	/* Only backup files in system_dir */
	if (dm_snprintf(sys_dir_prefix, sizeof(sys_dir_prefix), "%s/", cmd->system_dir) < 0)
		return 0;
	sys_dir_len = strlen(sys_dir_prefix);
	if (strncmp(filepath, sys_dir_prefix, sys_dir_len) != 0) {
		backup_path[0] = '\0';
		return 1; /* Not in system_dir, no backup needed */
	}

	/* Create backup directory */
	if (dm_snprintf(backup_dir, sizeof(backup_dir), "%s/old_conf", cmd->system_dir) < 0)
		return 0;
	if (mkdir(backup_dir, 0755) < 0 && errno != EEXIST) {
		log_sys_error("mkdir", backup_dir);
		return 0;
	}

	/* Get filename from path */
	filename = strrchr(filepath, '/');
	filename = filename ? filename + 1 : filepath;

	/* Create timestamp: YYYYMMDD-HHMMSS */
	now = time(NULL);
	tm = localtime(&now);
	strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm);

	/* Try filenames with counter from 00 to 99 to support multiple updates per second */
	for (counter = 0; counter < 100; counter++) {
		if (dm_snprintf(candidate_path, sizeof(candidate_path), "%s/%s-%s%02d",
				backup_dir, filename, timestamp, counter) < 0)
			return 0;

		/* Check if this filename already exists */
		if (stat(candidate_path, &st) != 0) {
			/* File doesn't exist, use this path */
			if (dm_snprintf(backup_path, len, "%s", candidate_path) < 0)
				return 0;
			return 1;
		}
	}

	/* All 100 slots for this second are taken - this is extremely unlikely */
	log_error("Failed to create backup path: too many backups in one second");
	return 0;
}

static int _move_to_backup(struct cmd_context *cmd, const char *source_path, const char *original_filepath)
{
	char backup_path[PATH_MAX * 2];
	char backup_dir[PATH_MAX];
	const char *filename;
	char file_prefix[PATH_MAX];

	unsigned int backup_limit;

	backup_limit = (unsigned int)find_config_tree_int(cmd, backup_config_file_backup_limit_CFG, NULL);
	if (!backup_limit)
		return 1;

	if (!_create_backup_path(cmd, original_filepath, backup_path, sizeof(backup_path)))
		return 0;

	if (!backup_path[0]) {
		unlink(source_path);
		return 1;
	}

	if (rename(source_path, backup_path) < 0) {
		log_sys_error("rename", source_path);
		return 0;
	}

	log_print("Previous config backed up to %s", backup_path);

	if (dm_snprintf(backup_dir, sizeof(backup_dir), "%s/old_conf", cmd->system_dir) < 0)
		return 0;

	filename = strrchr(original_filepath, '/');
	filename = filename ? filename + 1 : original_filepath;

	if (dm_snprintf(file_prefix, sizeof(file_prefix), "%s-", filename) < 0)
		return 0;

	backup_dir_cleanup(backup_dir, file_prefix, backup_limit, 0);

	return 1;
}

/*
 * Fallback used when RENAME_EXCHANGE is not available (either at build time or
 * at runtime): back up the existing destination before overwriting it.
 */
static int _rename_with_backup(struct cmd_context *cmd, const char *temp_path,
			       const char *dest_path)
{
	if (!_move_to_backup(cmd, dest_path, dest_path))
		log_warn("Failed to create backup of %s", dest_path);
	if (rename(temp_path, dest_path) < 0) {
		log_sys_error("rename", temp_path);
		unlink(temp_path);
		return 0;
	}
	return 1;
}

/*
 * Atomically write config to destination file using RENAME_EXCHANGE.
 * This ensures the file is never left in a partially written state.
 */
static int _atomic_write_config(struct cmd_context *cmd,
				 struct dm_config_tree *cft,
				 struct config_def_tree_spec *tree_spec,
				 const char *dest_path)
{
	char temp_path[PATH_MAX];
	char dir_path[PATH_MAX];
	char *last_slash;
	struct stat st;
	int exists;
	int ret;
	int fd;

	/* Check if destination exists */
	exists = (stat(dest_path, &st) == 0);

	/* Create temp file in same directory as destination */
	if (!_dm_strncpy(dir_path, dest_path, sizeof(dir_path)))
		return 0;
	last_slash = strrchr(dir_path, '/');
	if (last_slash)
		*last_slash = '\0';
	else
		dm_strncpy(dir_path, ".", sizeof(dir_path));

	if (dm_snprintf(temp_path, sizeof(temp_path), "%s/.lvm-config-XXXXXX", dir_path) < 0)
		return 0;

	fd = mkstemp(temp_path);
	if (fd < 0) {
		log_sys_error("mkstemp", temp_path);
		return 0;
	}
	close(fd);

	/* Write config to temp file */
	ret = config_write(cft, tree_spec, temp_path, 0, NULL);

	if (!ret) {
		log_error("Failed to write config to temp file %s", temp_path);
		unlink(temp_path);
		return 0;
	}

	fd = open(temp_path, O_RDONLY);
	if (fd >= 0) {
		if (fsync(fd))
			log_sys_error("fsync", temp_path);
		close(fd);
	}

	if (exists) {
#ifdef HAVE_RENAMEAT2
		/* Atomically exchange temp file with destination */
		if (renameat2(AT_FDCWD, temp_path, AT_FDCWD, dest_path, RENAME_EXCHANGE) < 0) {
			if (errno == ENOSYS || errno == EINVAL) {
				/* renameat2 not supported; back up dest before overwriting */
				log_warn("renameat2 not supported, using rename fallback");
				if (!_rename_with_backup(cmd, temp_path, dest_path))
					return 0;
			} else {
				log_sys_error("renameat2", temp_path);
				unlink(temp_path);
				return 0;
			}
		} else {
			/* Successfully exchanged - temp_path now contains old file */
			if (!_move_to_backup(cmd, temp_path, dest_path)) {
				log_warn("Failed to create backup, removing old file");
				unlink(temp_path);
			}
		}
#else
		/* renameat2 not available at build time; back up dest before overwriting */
		if (!_rename_with_backup(cmd, temp_path, dest_path))
			return 0;
#endif
	} else {
		/* Destination doesn't exist, just rename temp to dest */
		if (rename(temp_path, dest_path) < 0) {
			log_sys_error("rename", temp_path);
			unlink(temp_path);
			return 0;
		}
	}

	return 1;
}

static int edit_args_to_config_add(struct cmd_context *cmd, struct dm_pool *edit_mem,
				   int *has_add, int *has_local, int *has_nonlocal,
				   const char **config_add_string)
{
	struct arg_value_group_list *group;
	const char *edit_str;
	int ret;

	if (!dm_pool_begin_object(edit_mem, 256))
		return_ECMD_FAILED;

	dm_list_iterate_items(group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(group->arg_values, edit_ARG))
			continue;

		edit_str = grouped_arg_str_value(group->arg_values, edit_ARG, NULL);
		if (!edit_str) {
			log_error("Failed to get edit specification");
			dm_pool_abandon_object(edit_mem);
			return EINVALID_CMD_LINE;
		}

		if (_check_local_section(edit_str))
			*has_local = 1;
		else
			*has_nonlocal = 1;

		ret = _edit_to_config_string(edit_mem, edit_str, 0);
		if (ret < 0) {
			dm_pool_abandon_object(edit_mem);
			return EINVALID_CMD_LINE;
		}
		if (ret > 0)
			*has_add = 1;
	}

	if (!dm_pool_grow_object(edit_mem, "\0", 1))
		goto fail;
	if (!(*config_add_string = dm_pool_end_object(edit_mem)))
		return_ECMD_FAILED;
	return 0;
fail:
	dm_pool_abandon_object(edit_mem);
	return EINVALID_CMD_LINE;
}

static int edit_args_to_config_remove(struct cmd_context *cmd, struct dm_pool *edit_mem,
				      int *has_remove, const char **config_remove_string)
{
	struct arg_value_group_list *group;
	const char *edit_str;
	int ret;

	if (!dm_pool_begin_object(edit_mem, 256))
		return_ECMD_FAILED;

	dm_list_iterate_items(group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(group->arg_values, edit_ARG))
			continue;

		edit_str = grouped_arg_str_value(group->arg_values, edit_ARG, NULL);
		if (!edit_str)
			continue;

		ret = _edit_to_config_string(edit_mem, edit_str, 1);
		if (ret < 0)
			goto fail;
		if (ret > 0)
			*has_remove = 1;
	}

	if (!dm_pool_grow_object(edit_mem, "\0", 1))
		goto fail;
	if (!(*config_remove_string = dm_pool_end_object(edit_mem)))
		return_ECMD_FAILED;
	return 0;
fail:
	dm_pool_abandon_object(edit_mem);
	return EINVALID_CMD_LINE;
}

int editconfig_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	const char *file = arg_str_value(cmd, file_ARG, NULL);
	const char *input_file = arg_str_value(cmd, input_ARG, NULL);
	const char *output_file = arg_str_value(cmd, output_ARG, NULL);
	struct config_def_tree_spec tree_spec = {0};
	struct dm_config_tree *cft = NULL;
	struct dm_config_tree *edits_add_cft = NULL;
	struct dm_config_tree *edits_remove_cft = NULL;
	struct cft_check_handle *cft_check_handle = NULL;
	const char *config_file;
	const char *config_add_string;
	const char *config_remove_string;
	struct dm_config_tree *long_cft = NULL;
	struct dm_pool *edit_mem;
	int has_local = 0, has_nonlocal = 0;
	int has_add = 0, has_remove = 0;
	int use_atomic_write = 0;
	char lvm_conf_path[PATH_MAX];
	char lvmlocal_conf_path[PATH_MAX];
	int ret;

	tree_spec.cmd = cmd;

	if (!(edit_mem = dm_pool_create("edit specs", 1024)))
		return_ECMD_FAILED;

	ret = edit_args_to_config_add(cmd, edit_mem, &has_add, &has_local, &has_nonlocal, &config_add_string);
	if (ret)
		goto out;

	ret = edit_args_to_config_remove(cmd, edit_mem, &has_remove, &config_remove_string);
	if (ret)
		goto out;

	ret = ECMD_FAILED;

	if (input_file)
		config_file = input_file;
	else if (file)
		config_file = file;
	else {
		if (!(config_file = _determine_config_file(cmd, has_local, has_nonlocal))) {
			ret = EINVALID_CMD_LINE;
			goto out;
		}
	}

	if (access(config_file, F_OK) == 0) {
		log_verbose("Reading existing config from %s", config_file);
		if (!(cft = config_file_open_and_read(config_file, CONFIG_FILE, cmd))) {
			log_error("Failed to read config file %s", config_file);
			goto out;
		}
	} else {
		log_verbose("Creating new config file %s", config_file);
		if (!(cft = dm_config_create())) {
			log_error("Failed to create config tree");
			goto out;
		}
	}

	/*
	 * Merge edit trees into cft. merge_config_tree moves nodes from
	 * source into destination, so edits_add_cft and edits_remove_cft
	 * must not be destroyed until cft is no longer needed.
	 */
	if (has_add) {
		if (!(edits_add_cft = dm_config_from_string(config_add_string))) {
			log_error("Failed to parse edit specifications");
			goto out;
		}

		if (!merge_config_tree(cmd, cft, edits_add_cft, CONFIG_MERGE_TYPE_RAW)) {
			log_error("Failed to apply configuration edits");
			goto out;
		}
	}

	if (!config_set_source(cft, CONFIG_FILE))
		goto out;

	if (has_remove) {
		if (!(edits_remove_cft = dm_config_from_string(config_remove_string))) {
			log_error("Failed to parse removal specifications");
			goto out;
		}
		if (!merge_config_tree(cmd, cft, edits_remove_cft, CONFIG_MERGE_TYPE_REMOVE)) {
			log_error("Failed to apply configuration removals");
			goto out;
		}
	}

	if (dm_snprintf(lvm_conf_path, sizeof(lvm_conf_path), "%s/lvm.conf", cmd->system_dir) < 0)
		goto out;
	if (dm_snprintf(lvmlocal_conf_path, sizeof(lvmlocal_conf_path), "%s/lvmlocal.conf", cmd->system_dir) < 0)
		goto out;

	if (output_file) {
		file = output_file;
		if (strcmp(output_file, lvm_conf_path) == 0 ||
		    strcmp(output_file, lvmlocal_conf_path) == 0)
			use_atomic_write = 1;
	} else if (!input_file) {
		file = config_file;
		use_atomic_write = 1;
	} else {
		file = NULL;
	}

	if (arg_is_set(cmd, withspaces_ARG)) {
		tree_spec.withspaces = 1;
		_set_value_format_flags(cft->root, DM_CONFIG_VALUE_FMT_COMMON_EXTRA_SPACES);
	}

	if (file && !arg_is_set(cmd, withcomments_ARG) && _file_has_comments(file))
		tree_spec.withcomments = 1;

	if (arg_is_set(cmd, withcomments_ARG) || tree_spec.withcomments) {
		tree_spec.withcomments = 1;
		tree_spec.type = CFG_DEF_TREE_FULL_DIFF;
		tree_spec.current_cft = cft;

		if (!_do_def_check(&tree_spec, cft, &cft_check_handle))
			goto out;
		tree_spec.check_status = cft_check_handle->status;

		if (!(long_cft = config_def_create_tree(&tree_spec))) {
			log_error("Failed to create output config tree");
			goto out;
		}
	}

	if (use_atomic_write) {
		if (!_atomic_write_config(cmd, long_cft ? long_cft : cft, &tree_spec, file)) {
			log_error("Failed to write config to %s", file);
			goto out;
		}
	} else {
		if (!config_write(long_cft ? long_cft : cft, &tree_spec, file, 0, NULL)) {
			log_error("Failed to write config%s%s", file ? " to " : "", file ? file : "");
			goto out;
		}
	}

	if (file)
		log_print("Configuration written to %s", file);
	ret = ECMD_PROCESSED;
out:
	dm_pool_destroy(edit_mem);
	if (long_cft)
		dm_config_destroy(long_cft);
	if (edits_add_cft)
		dm_config_destroy(edits_add_cft);
	if (edits_remove_cft)
		dm_config_destroy(edits_remove_cft);
	if (cft)
		dm_config_destroy(cft);
	return ret;
}

int dumpconfig(struct cmd_context *cmd, int argc, char **argv)
{
	const char *file = arg_str_value(cmd, file_ARG, NULL);
	const char *type = arg_str_value(cmd, configtype_ARG, arg_is_set(cmd, list_ARG) ? "list" : "current");
	struct config_def_tree_spec tree_spec = {0};
	struct dm_config_tree *cft = NULL;
	struct cft_check_handle *cft_check_handle = NULL;
	struct profile *profile = NULL;
	int r = ECMD_PROCESSED;

	tree_spec.cmd = cmd;

	if (arg_is_set(cmd, configtype_ARG) && arg_is_set(cmd, validate_ARG)) {
		log_error("Only one of --type and --validate permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, atversion_ARG)) {
		if (arg_is_set(cmd, sinceversion_ARG)) {
			log_error("Only one of --atversion and --sinceversion permitted.");
			return EINVALID_CMD_LINE;
		}

		if (!arg_is_set(cmd, configtype_ARG) && !arg_is_set(cmd, list_ARG)) {
			log_error("--atversion requires --type or --list");
			return EINVALID_CMD_LINE;
		}
	} else if (arg_is_set(cmd, sinceversion_ARG)) {
		if (!arg_is_set(cmd, configtype_ARG) || strcmp(type, "new")) {
			log_error("--sinceversion requires --type new");
			return EINVALID_CMD_LINE;
		}
	}

	if (arg_is_set(cmd, ignoreadvanced_ARG))
		tree_spec.ignoreadvanced = 1;

	if (arg_is_set(cmd, ignoreunsupported_ARG)) {
		if (arg_is_set(cmd, showunsupported_ARG)) {
			log_error("Only one of --ignoreunsupported and --showunsupported permitted.");
			return EINVALID_CMD_LINE;
		}
		tree_spec.ignoreunsupported = 1;
	} else if (arg_is_set(cmd, showunsupported_ARG)) {
		tree_spec.ignoreunsupported = 0;
	} else if (strcmp(type, "current") && strcmp(type, "diff")) {
		/*
		 * By default hide unsupported settings
		 * for all display types except "current"
		 * and "diff".
		 */
		tree_spec.ignoreunsupported = 1;
	}

	if (strcmp(type, "current") && strcmp(type, "diff")) {
		/*
		 * By default hide deprecated settings
		 * for all display types except "current"
		 * and "diff" unless --showdeprecated is set.
		 *
		 * N.B. Deprecated settings are visible if
		 * --atversion is used with a version that
		 * is lower than the version in which the
		 * setting was deprecated.
		 */
		if (!arg_is_set(cmd, showdeprecated_ARG))
			tree_spec.ignoredeprecated = 1;
	}

	if (arg_is_set(cmd, ignorelocal_ARG))
		tree_spec.ignorelocal = 1;

	if (!strcmp(type, "current") || !strcmp(type, "full")) {
		if (arg_is_set(cmd, atversion_ARG)) {
			log_error("--atversion has no effect with --type %s", type);
			return EINVALID_CMD_LINE;
		}

		if ((arg_is_set(cmd, ignoreunsupported_ARG) ||
		    arg_is_set(cmd, ignoreadvanced_ARG)) &&
		    !strcmp(type, "current")) {
			/* FIXME: allow these even for --type current */
			log_error("--ignoreadvanced and --ignoreunsupported has "
				  "no effect with --type current");
			return EINVALID_CMD_LINE;
		}
	} else if (arg_is_set(cmd, mergedconfig_ARG)) {
		log_error("--mergedconfig has no effect without --type current or --type full");
		return EINVALID_CMD_LINE;
	}

	if (!_get_vsn(cmd, &tree_spec.version))
		return EINVALID_CMD_LINE;

	/*
	 * The profile specified by --profile cmd arg is like --commandprofile,
	 * but it is used just for dumping the profile content and not for
	 * application.
	 */
	if (arg_is_set(cmd, profile_ARG) &&
	    (!(profile = add_profile(cmd, arg_str_value(cmd, profile_ARG, NULL), CONFIG_PROFILE_COMMAND)) ||
	    !override_config_tree_from_profile(cmd, profile))) {
		log_error("Failed to load profile %s.", arg_str_value(cmd, profile_ARG, NULL));
		return ECMD_FAILED;
	}

	/*
	 * Set the 'cft' to work with based on whether we need the plain
	 * config tree or merged config tree cascade if --mergedconfig is used.
	 */
	if ((arg_is_set(cmd, mergedconfig_ARG) || !strcmp(type, "full") || !strcmp(type, "diff")) && cmd->cft->cascade) {
		if (!_merge_config_cascade(cmd, cmd->cft, &cft)) {
			log_error("Failed to merge configuration.");
			r = ECMD_FAILED;
			goto out;
		}
	} else
		cft = cmd->cft;
	tree_spec.current_cft = cft;

	if (arg_is_set(cmd, validate_ARG)) {
		if (_config_validate(cmd, cft)) {
			log_print("LVM configuration valid.");
			goto out;
		} else {
			log_error("LVM configuration invalid.");
			r = ECMD_FAILED;
			goto out;
		}
	}

	if (!strcmp(type, "list")) {
		tree_spec.listmode = 1;
		tree_spec.type = CFG_DEF_TREE_LIST;
		/* list type does not require status check */
	} else if (!strcmp(type, "full")) {
		tree_spec.type = CFG_DEF_TREE_FULL;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	} else if (!strcmp(type, "current")) {
		tree_spec.type = CFG_DEF_TREE_CURRENT;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "missing")) {
		tree_spec.type = CFG_DEF_TREE_MISSING;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "default")) {
		tree_spec.type = CFG_DEF_TREE_DEFAULT;
		/* default type does not require check status */
	}
	else if (!strcmp(type, "diff")) {
		tree_spec.type = CFG_DEF_TREE_DIFF;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "new")) {
		tree_spec.type = arg_is_set(cmd, sinceversion_ARG) ? CFG_DEF_TREE_NEW_SINCE
								  : CFG_DEF_TREE_NEW;
		/* new type does not require check status */
	}
	else if (!strcmp(type, "profilable")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE;
		/* profilable type does not require check status */
	}
	else if (!strcmp(type, "profilable-command")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE_CMD;
		/* profilable-command type does not require check status */
	}
	else if (!strcmp(type, "profilable-metadata")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE_MDA;
		/* profilable-metadata  type does not require check status */
	}
	else {
		log_error("Incorrect type of configuration specified. "
			  "Expected one of: current, default, diff, full, list, missing, "
			  "new, profilable, profilable-command, profilable-metadata.");
		r = EINVALID_CMD_LINE;
		goto out;
	}

	if (arg_is_set(cmd, withsummary_ARG))
		tree_spec.withsummary = 1;

	/*
	 *  This is for backwards compatibility with the original prehistoric
	 *  'dumpcofig --list' when the --typeconfig was not supported yet.
	 */
	if (arg_is_set(cmd, list_ARG) && !arg_is_set(cmd, configtype_ARG))
		tree_spec.withsummary = 1;

	if (arg_is_set(cmd, withcomments_ARG))
		tree_spec.withcomments = 1;

	if (arg_is_set(cmd, unconfigured_ARG))
		tree_spec.unconfigured = 1;

	if (arg_is_set(cmd, withversions_ARG))
		tree_spec.withversions = 1;

	if (arg_is_set(cmd, withgeneralpreamble_ARG))
		tree_spec.withgeneralpreamble = 1;

	if (arg_is_set(cmd, withlocalpreamble_ARG))
		tree_spec.withlocalpreamble = 1;

	if (arg_is_set(cmd, withspaces_ARG))
		tree_spec.withspaces = 1;

	if (arg_is_set(cmd, valuesonly_ARG))
		tree_spec.valuesonly = 1;

	if (arg_is_set(cmd, list_ARG))
		tree_spec.listmode = 1;

	if (tree_spec.listmode) {
		if (arg_is_set(cmd, withcomments_ARG)) {
			log_error("--withcomments has no effect with --type list or --list");
			r = EINVALID_CMD_LINE;
			goto out;
		}
		if (arg_is_set(cmd, withlocalpreamble_ARG)) {
			log_error("--withlocalpreamble has no effect with --type list or --list");
			r = EINVALID_CMD_LINE;
			goto out;
		}
		if (arg_is_set(cmd, withgeneralpreamble_ARG)) {
			log_error("--withgeneralpreamble has no effect with --type list or --list");
			r = EINVALID_CMD_LINE;
			goto out;
		}
		if (arg_is_set(cmd, valuesonly_ARG)) {
			log_error("--valuesonly has no effect with --type list or --list.");
			r = EINVALID_CMD_LINE;
			goto out;
		}
	}

	if (cft_check_handle)
		tree_spec.check_status = cft_check_handle->status;

	if ((tree_spec.type != CFG_DEF_TREE_CURRENT) &&
	    (tree_spec.type != CFG_DEF_TREE_DIFF) &&
	    !(cft = config_def_create_tree(&tree_spec))) {
		r = ECMD_FAILED;
		goto_out;
	}

	if (tree_spec.withspaces)
		_set_value_format_flags(cft->root, DM_CONFIG_VALUE_FMT_COMMON_EXTRA_SPACES);

	if (!config_write(cft, &tree_spec, file, argc, argv)) {
		stack;
		r = ECMD_FAILED;
	}
out:
	if (tree_spec.current_cft && (tree_spec.current_cft != cft) &&
	    (tree_spec.current_cft != cmd->cft))
		/*
		 * This happens in case of CFG_DEF_TREE_FULL where we
		 * have merged explicitly defined config trees and also
		 * we have used default tree.
		 */
		dm_config_destroy(tree_spec.current_cft);

	if (cft && (cft != cmd->cft))
		dm_config_destroy(cft);
	else if (profile)
		remove_config_tree_by_source(cmd, CONFIG_PROFILE_COMMAND);

	/*
	 * The cmd->cft (the "current" tree) is destroyed
	 * together with cmd context destroy...
	 */

	return r;
}

int config(struct cmd_context *cmd, int argc, char **argv)
{
	return dumpconfig(cmd, argc, argv);
}

int lvmconfig(struct cmd_context *cmd, int argc, char **argv)
{
	return dumpconfig(cmd, argc, argv);
}
