/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
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
#include "toolcontext.h"
#include "metadata.h"
#include "defaults.h"
#include "lvm-string.h"
#include "activate.h"
#include "filter.h"
#include "label.h"
#include "lvm-file.h"
#include "format-text.h"
#include "display.h"
#include "memlock.h"
#include "str_list.h"
#include "segtype.h"
#include "lvmcache.h"
#include "lvmetad.h"
#include "archiver.h"
#include "lvmpolld-client.h"

#ifdef HAVE_LIBDL
#include "sharedlib.h"
#endif

#ifdef LVM1_INTERNAL
#include "format1.h"
#endif

#ifdef POOL_INTERNAL
#include "format_pool.h"
#endif

#include <locale.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>

#ifdef __linux__
#  include <malloc.h>
#endif

static const size_t linebuffer_size = 4096;

/*
 * Copy the input string, removing invalid characters.
 */
const char *system_id_from_string(struct cmd_context *cmd, const char *str)
{
	char *system_id;

	if (!str || !*str) {
		log_warn("WARNING: Empty system ID supplied.");
		return "";
	}

	if (!(system_id = dm_pool_zalloc(cmd->libmem, strlen(str) + 1))) {
		log_warn("WARNING: Failed to allocate system ID.");
		return NULL;
	}

	copy_systemid_chars(str, system_id);

	if (!*system_id) {
		log_warn("WARNING: Invalid system ID format: %s", str);
		return NULL;
	}

	if (!strncmp(system_id, "localhost", 9)) {
		log_warn("WARNING: system ID may not begin with the string \"localhost\".");
		return NULL;
	}

	return system_id;
}

static const char *_read_system_id_from_file(struct cmd_context *cmd, const char *file)
{
	char *line = NULL;
	size_t line_size;
	char *start, *end;
	const char *system_id = NULL;
	FILE *fp;

	if (!file || !strlen(file) || !file[0])
		return_NULL;

	if (!(fp = fopen(file, "r"))) {
		log_warn("WARNING: %s: fopen failed: %s", file, strerror(errno));
		return NULL;
	}

	while (getline(&line, &line_size, fp) > 0) {
		start = line;

		/* Ignore leading whitespace */
		while (*start && isspace(*start))
			start++;

		/* Ignore rest of line after # */
		if (!*start || *start == '#')
			continue;

		if (system_id && *system_id) {
			log_warn("WARNING: Ignoring extra line(s) in system ID file %s.", file);
			break;
		}

		/* Remove any comments from end of line */
		for (end = start; *end; end++)
			if (*end == '#') {
				*end = '\0';
				break;
			}

		system_id = system_id_from_string(cmd, start);
	}

	free(line);

	if (fclose(fp))
		stack;

	return system_id;
}

static const char *_system_id_from_source(struct cmd_context *cmd, const char *source)
{
	char filebuf[PATH_MAX];
	const char *file;
	const char *etc_str;
	const char *str;
	const char *system_id = NULL;

	if (!strcasecmp(source, "uname")) {
		if (cmd->hostname)
			system_id = system_id_from_string(cmd, cmd->hostname);
		goto out;
	}

	/* lvm.conf and lvmlocal.conf are merged into one config tree */
	if (!strcasecmp(source, "lvmlocal")) {
		if ((str = find_config_tree_str(cmd, local_system_id_CFG, NULL)))
			system_id = system_id_from_string(cmd, str);
		goto out;
	}

	if (!strcasecmp(source, "machineid") || !strcasecmp(source, "machine-id")) {
		etc_str = find_config_tree_str(cmd, global_etc_CFG, NULL);
		if (dm_snprintf(filebuf, sizeof(filebuf), "%s/machine-id", etc_str) != -1)
			system_id = _read_system_id_from_file(cmd, filebuf);
		goto out;
	}

	if (!strcasecmp(source, "file")) {
		file = find_config_tree_str(cmd, global_system_id_file_CFG, NULL);
		system_id = _read_system_id_from_file(cmd, file);
		goto out;
	}

	log_warn("WARNING: Unrecognised system_id_source \"%s\".", source);

out:
	return system_id;
}

static int _get_env_vars(struct cmd_context *cmd)
{
	const char *e;

	/* Set to "" to avoid using any system directory */
	if ((e = getenv("LVM_SYSTEM_DIR"))) {
		if (dm_snprintf(cmd->system_dir, sizeof(cmd->system_dir),
				 "%s", e) < 0) {
			log_error("LVM_SYSTEM_DIR environment variable "
				  "is too long.");
			return 0;
		}
	}

	return 1;
}

static void _get_sysfs_dir(struct cmd_context *cmd, char *buf, size_t buf_size)
{
	static char proc_mounts[PATH_MAX];
	static char *split[4], buffer[PATH_MAX + 16];
	FILE *fp;
	char *sys_mnt = NULL;

	*buf = '\0';

	if (!*cmd->proc_dir) {
		log_debug("No proc filesystem found: skipping sysfs detection");
		return;
	}

	if (dm_snprintf(proc_mounts, sizeof(proc_mounts),
			 "%s/mounts", cmd->proc_dir) < 0) {
		log_error("Failed to create /proc/mounts string for sysfs detection");
		return;
	}

	if (!(fp = fopen(proc_mounts, "r"))) {
		log_sys_error("_get_sysfs_dir fopen", proc_mounts);
		return;
	}

	while (fgets(buffer, sizeof(buffer), fp)) {
		if (dm_split_words(buffer, 4, 0, split) == 4 &&
		    !strcmp(split[2], "sysfs")) {
			sys_mnt = split[1];
			break;
		}
	}

	if (fclose(fp))
		log_sys_error("fclose", proc_mounts);

	if (!sys_mnt) {
		log_error("Failed to find sysfs mount point");
		return;
	}

	strncpy(buf, sys_mnt, buf_size);
}

static int _parse_debug_classes(struct cmd_context *cmd)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	int debug_classes = 0;

	if (!(cn = find_config_tree_node(cmd, log_debug_classes_CFG, NULL)))
		return DEFAULT_LOGGED_DEBUG_CLASSES;

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_verbose("log/debug_classes contains a value "
				    "which is not a string.  Ignoring.");
			continue;
		}

		if (!strcasecmp(cv->v.str, "all"))
			return -1;

		if (!strcasecmp(cv->v.str, "memory"))
			debug_classes |= LOG_CLASS_MEM;
		else if (!strcasecmp(cv->v.str, "devices"))
			debug_classes |= LOG_CLASS_DEVS;
		else if (!strcasecmp(cv->v.str, "activation"))
			debug_classes |= LOG_CLASS_ACTIVATION;
		else if (!strcasecmp(cv->v.str, "allocation"))
			debug_classes |= LOG_CLASS_ALLOC;
		else if (!strcasecmp(cv->v.str, "lvmetad"))
			debug_classes |= LOG_CLASS_LVMETAD;
		else if (!strcasecmp(cv->v.str, "metadata"))
			debug_classes |= LOG_CLASS_METADATA;
		else if (!strcasecmp(cv->v.str, "cache"))
			debug_classes |= LOG_CLASS_CACHE;
		else if (!strcasecmp(cv->v.str, "locking"))
			debug_classes |= LOG_CLASS_LOCKING;
		else if (!strcasecmp(cv->v.str, "lvmpolld"))
			debug_classes |= LOG_CLASS_LVMPOLLD;
		else
			log_verbose("Unrecognised value for log/debug_classes: %s", cv->v.str);
	}

	return debug_classes;
}

static void _init_logging(struct cmd_context *cmd)
{
	int append = 1;
	time_t t;

	const char *log_file;
	char timebuf[26];

	/* Syslog */
	cmd->default_settings.syslog = find_config_tree_bool(cmd, log_syslog_CFG, NULL);
	if (cmd->default_settings.syslog != 1)
		fin_syslog();

	if (cmd->default_settings.syslog > 1)
		init_syslog(cmd->default_settings.syslog);

	/* Debug level for log file output */
	cmd->default_settings.debug = find_config_tree_int(cmd, log_level_CFG, NULL);
	init_debug(cmd->default_settings.debug);

	/*
	 * Suppress all non-essential stdout?
	 * -qq can override the default of 0 to 1 later.
	 * Once set to 1, there is no facility to change it back to 0.
	 */
	cmd->default_settings.silent = silent_mode() ? :
	    find_config_tree_bool(cmd, log_silent_CFG, NULL);
	init_silent(cmd->default_settings.silent);

	/* Verbose level for tty output */
	cmd->default_settings.verbose = find_config_tree_bool(cmd, log_verbose_CFG, NULL);
	init_verbose(cmd->default_settings.verbose + VERBOSE_BASE_LEVEL);

	/* Log message formatting */
	init_indent(find_config_tree_bool(cmd, log_indent_CFG, NULL));
	init_abort_on_internal_errors(find_config_tree_bool(cmd, global_abort_on_internal_errors_CFG, NULL));

	cmd->default_settings.msg_prefix = find_config_tree_str_allow_empty(cmd, log_prefix_CFG, NULL);
	init_msg_prefix(cmd->default_settings.msg_prefix);

	cmd->default_settings.cmd_name = find_config_tree_bool(cmd, log_command_names_CFG, NULL);
	init_cmd_name(cmd->default_settings.cmd_name);

	/* Test mode */
	cmd->default_settings.test =
	    find_config_tree_bool(cmd, global_test_CFG, NULL);
	init_test(cmd->default_settings.test);

	/* Settings for logging to file */
	if (find_config_tree_bool(cmd, log_overwrite_CFG, NULL))
		append = 0;

	log_file = find_config_tree_str(cmd, log_file_CFG, NULL);

	if (log_file) {
		release_log_memory();
		fin_log();
		init_log_file(log_file, append);
	}

	log_file = find_config_tree_str(cmd, log_activate_file_CFG, NULL);
	if (log_file)
		init_log_direct(log_file, append);

	init_log_while_suspended(find_config_tree_bool(cmd, log_activation_CFG, NULL));

	cmd->default_settings.debug_classes = _parse_debug_classes(cmd);
	log_debug("Setting log debug classes to %d", cmd->default_settings.debug_classes);
	init_debug_classes_logged(cmd->default_settings.debug_classes);

	t = time(NULL);
	ctime_r(&t, &timebuf[0]);
	timebuf[24] = '\0';
	log_verbose("Logging initialised at %s", timebuf);

	/* Tell device-mapper about our logging */
#ifdef DEVMAPPER_SUPPORT
	dm_log_with_errno_init(print_log);
#endif
	reset_log_duplicated();
	reset_lvm_errno(1);
}

static int _check_disable_udev(const char *msg) {
	if (getenv("DM_DISABLE_UDEV")) {
		log_very_verbose("DM_DISABLE_UDEV environment variable set. "
				 "Overriding configuration to use "
				 "udev_rules=0, udev_sync=0, verify_udev_operations=1.");
		if (udev_is_running())
			log_warn("Udev is running and DM_DISABLE_UDEV environment variable is set. "
				 "Bypassing udev, LVM will %s.", msg);

		return 1;
	}

	return 0;
}

static int _check_config_by_source(struct cmd_context *cmd, config_source_t source)
{
	struct dm_config_tree *cft;
	struct cft_check_handle *handle;

	if (!(cft = get_config_tree_by_source(cmd, source)) ||
	    !(handle = get_config_tree_check_handle(cmd, cft)))
		return 1;

	return config_def_check(handle);
}

static int _check_config(struct cmd_context *cmd)
{
	int abort_on_error;

	if (!find_config_tree_bool(cmd, config_checks_CFG, NULL))
		return 1;

	abort_on_error = find_config_tree_bool(cmd, config_abort_on_errors_CFG, NULL);

	if ((!_check_config_by_source(cmd, CONFIG_STRING) ||
	    !_check_config_by_source(cmd, CONFIG_MERGED_FILES) ||
	    !_check_config_by_source(cmd, CONFIG_FILE)) &&
	    abort_on_error) {
		log_error("LVM_ configuration invalid.");
		return 0;
	}

	return 1;
}

int process_profilable_config(struct cmd_context *cmd)
{
	if (!(cmd->default_settings.unit_factor =
	      dm_units_to_factor(find_config_tree_str(cmd, global_units_CFG, NULL),
				 &cmd->default_settings.unit_type, 1, NULL))) {
		log_error("Invalid units specification");
		return 0;
	}

	cmd->si_unit_consistency = find_config_tree_bool(cmd, global_si_unit_consistency_CFG, NULL);
	cmd->report_binary_values_as_numeric = find_config_tree_bool(cmd, report_binary_values_as_numeric_CFG, NULL);
	cmd->default_settings.suffix = find_config_tree_bool(cmd, global_suffix_CFG, NULL);
	cmd->report_list_item_separator = find_config_tree_str(cmd, report_list_item_separator_CFG, NULL);

	return 1;
}

static int _init_system_id(struct cmd_context *cmd)
{
	const char *source, *system_id;
	int local_set = 0;

	cmd->system_id = NULL;
	cmd->unknown_system_id = 0;

	system_id = find_config_tree_str_allow_empty(cmd, local_system_id_CFG, NULL);
	if (system_id && *system_id)
		local_set = 1;

	source = find_config_tree_str(cmd, global_system_id_source_CFG, NULL);
	if (!source)
		source = "none";

	/* Defining local system_id but not using it is probably a config mistake. */
	if (local_set && strcmp(source, "lvmlocal"))
		log_warn("WARNING: local/system_id is set, so should global/system_id_source be \"lvmlocal\" not \"%s\"?", source);

	if (!strcmp(source, "none"))
		return 1;

	if ((system_id = _system_id_from_source(cmd, source)) && *system_id) {
		cmd->system_id = system_id;
		return 1;
	}

	/*
	 * The source failed to resolve a system_id.  In this case allow
	 * VGs with no system_id to be accessed, but not VGs with a system_id.
	 */
	log_warn("WARNING: No system ID found from system_id_source %s.", source);
	cmd->unknown_system_id = 1;

	return 1;
}

static int _process_config(struct cmd_context *cmd)
{
	mode_t old_umask;
	const char *dev_ext_info_src;
	const char *read_ahead;
	struct stat st;
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	int64_t pv_min_kb;
	const char *lvmetad_socket;
	const char *lvmpolld_socket;
	int udev_disabled = 0;
	char sysfs_dir[PATH_MAX];

	if (!_check_config(cmd))
		return_0;

	/* umask */
	cmd->default_settings.umask = find_config_tree_int(cmd, global_umask_CFG, NULL);

	if ((old_umask = umask((mode_t) cmd->default_settings.umask)) !=
	    (mode_t) cmd->default_settings.umask)
		log_verbose("Set umask from %04o to %04o",
                            old_umask, cmd->default_settings.umask);

	/* dev dir */
	if (dm_snprintf(cmd->dev_dir, sizeof(cmd->dev_dir), "%s/",
			 find_config_tree_str(cmd, devices_dir_CFG, NULL)) < 0) {
		log_error("Device directory given in config file too long");
		return 0;
	}
#ifdef DEVMAPPER_SUPPORT
	dm_set_dev_dir(cmd->dev_dir);

	if (!dm_set_uuid_prefix("LVM-"))
		return_0;
#endif

	dev_ext_info_src = find_config_tree_str(cmd, devices_external_device_info_source_CFG, NULL);
	if (!strcmp(dev_ext_info_src, "none"))
		init_external_device_info_source(DEV_EXT_NONE);
	else if (!strcmp(dev_ext_info_src, "udev"))
		init_external_device_info_source(DEV_EXT_UDEV);
	else {
		log_error("Invalid external device info source specification.");
		return 0;
	}

	/* proc dir */
	if (dm_snprintf(cmd->proc_dir, sizeof(cmd->proc_dir), "%s",
			 find_config_tree_str(cmd, global_proc_CFG, NULL)) < 0) {
		log_error("Device directory given in config file too long");
		return 0;
	}

	if (*cmd->proc_dir && !dir_exists(cmd->proc_dir)) {
		log_warn("WARNING: proc dir %s not found - some checks will be bypassed",
			 cmd->proc_dir);
		cmd->proc_dir[0] = '\0';
	}

	_get_sysfs_dir(cmd, sysfs_dir, sizeof(sysfs_dir));
	dm_set_sysfs_dir(sysfs_dir);

	/* activation? */
	cmd->default_settings.activation = find_config_tree_bool(cmd, global_activation_CFG, NULL);
	set_activation(cmd->default_settings.activation, 0);

	cmd->auto_set_activation_skip = find_config_tree_bool(cmd, activation_auto_set_activation_skip_CFG, NULL);

	read_ahead = find_config_tree_str(cmd, activation_readahead_CFG, NULL);
	if (!strcasecmp(read_ahead, "auto"))
		cmd->default_settings.read_ahead = DM_READ_AHEAD_AUTO;
	else if (!strcasecmp(read_ahead, "none"))
		cmd->default_settings.read_ahead = DM_READ_AHEAD_NONE;
	else {
		log_error("Invalid readahead specification");
		return 0;
	}

	/*
	 * If udev is disabled using DM_DISABLE_UDEV environment
	 * variable, override existing config and hardcode these:
	 *   - udev_rules = 0
	 *   - udev_sync = 0
	 *   - udev_fallback = 1
	 */
	udev_disabled = _check_disable_udev("manage logical volume symlinks in device directory");

	cmd->default_settings.udev_rules = udev_disabled ? 0 :
		find_config_tree_bool(cmd, activation_udev_rules_CFG, NULL);

	cmd->default_settings.udev_sync = udev_disabled ? 0 :
		find_config_tree_bool(cmd, activation_udev_sync_CFG, NULL);

	/*
	 * Set udev_fallback lazily on first use since it requires
	 * checking DM driver version which is an extra ioctl!
	 * This also prevents unnecessary use of mapper/control.
	 * If udev is disabled globally, set fallback mode immediately.
	 */
	cmd->default_settings.udev_fallback = udev_disabled ? 1 : -1;

	init_retry_deactivation(find_config_tree_bool(cmd, activation_retry_deactivation_CFG, NULL));

	init_activation_checks(find_config_tree_bool(cmd, activation_checks_CFG, NULL));

	cmd->use_linear_target = find_config_tree_bool(cmd, activation_use_linear_target_CFG, NULL);

	cmd->stripe_filler = find_config_tree_str(cmd, activation_missing_stripe_filler_CFG, NULL);

	/* FIXME Missing error code checks from the stats, not log_warn?, notify if setting overridden, delay message/check till it is actually used (eg consider if lvm shell - file could appear later after this check)? */
	if (!strcmp(cmd->stripe_filler, "/dev/ioerror") &&
	    stat(cmd->stripe_filler, &st))
		cmd->stripe_filler = "error";

	if (strcmp(cmd->stripe_filler, "error")) {
		if (stat(cmd->stripe_filler, &st)) {
			log_warn("WARNING: activation/missing_stripe_filler = \"%s\" "
				 "is invalid,", cmd->stripe_filler);
			log_warn("         stat failed: %s", strerror(errno));
			log_warn("Falling back to \"error\" missing_stripe_filler.");
			cmd->stripe_filler = "error";
		} else if (!S_ISBLK(st.st_mode)) {
			log_warn("WARNING: activation/missing_stripe_filler = \"%s\" "
				 "is not a block device.", cmd->stripe_filler);
			log_warn("Falling back to \"error\" missing_stripe_filler.");
			cmd->stripe_filler = "error";
		}
	}

	if ((cn = find_config_tree_node(cmd, activation_mlock_filter_CFG, NULL)))
		for (cv = cn->v; cv; cv = cv->next) 
			if ((cv->type != DM_CFG_STRING) || !cv->v.str[0]) 
				log_error("Ignoring invalid activation/mlock_filter entry in config file");

	cmd->metadata_read_only = find_config_tree_bool(cmd, global_metadata_read_only_CFG, NULL);

	pv_min_kb = find_config_tree_int64(cmd, devices_pv_min_size_CFG, NULL);
	if (pv_min_kb < PV_MIN_SIZE_KB) {
		log_warn("Ignoring too small pv_min_size %" PRId64 "KB, using default %dKB.",
			 pv_min_kb, PV_MIN_SIZE_KB);
		pv_min_kb = PV_MIN_SIZE_KB;
	}
	/* LVM stores sizes internally in units of 512-byte sectors. */
	init_pv_min_size((uint64_t)pv_min_kb * (1024 >> SECTOR_SHIFT));

	if (!process_profilable_config(cmd))
		return_0;

	init_detect_internal_vg_cache_corruption
		(find_config_tree_bool(cmd, global_detect_internal_vg_cache_corruption_CFG, NULL));

	lvmetad_disconnect();
	lvmpolld_disconnect();

	lvmetad_socket = getenv("LVM_LVMETAD_SOCKET");
	if (!lvmetad_socket)
		lvmetad_socket = DEFAULT_RUN_DIR "/lvmetad.socket";

	/* TODO?
		lvmetad_socket = find_config_tree_str(cmd, "lvmetad/socket_path",
						      DEFAULT_RUN_DIR "/lvmetad.socket");
	*/
	lvmetad_set_socket(lvmetad_socket);
	cn = find_config_tree_node(cmd, devices_global_filter_CFG, NULL);
	lvmetad_set_token(cn ? cn->v : NULL);

	if (find_config_tree_int(cmd, global_locking_type_CFG, NULL) == 3 &&
	    find_config_tree_bool(cmd, global_use_lvmetad_CFG, NULL)) {
		log_warn("WARNING: configuration setting use_lvmetad overridden to 0 due to locking_type 3. "
			 "Clustered environment not supported by lvmetad yet.");
		lvmetad_set_active(NULL, 0);
	} else
		lvmetad_set_active(NULL, find_config_tree_bool(cmd, global_use_lvmetad_CFG, NULL));

	lvmetad_init(cmd);

	if (!_init_system_id(cmd))
		return_0;

	lvmpolld_socket = getenv("LVM_LVMPOLLD_SOCKET");
	if (!lvmpolld_socket)
		lvmpolld_socket = DEFAULT_RUN_DIR "/lvmpolld.socket";
	lvmpolld_set_socket(lvmpolld_socket);

	lvmpolld_set_active(find_config_tree_bool(cmd, global_use_lvmpolld_CFG, NULL));

	return 1;
}

static int _set_tag(struct cmd_context *cmd, const char *tag)
{
	log_very_verbose("Setting host tag: %s", dm_pool_strdup(cmd->libmem, tag));

	if (!str_list_add(cmd->libmem, &cmd->tags, tag)) {
		log_error("_set_tag: str_list_add %s failed", tag);
		return 0;
	}

	return 1;
}

static int _check_host_filters(struct cmd_context *cmd, const struct dm_config_node *hn,
			       int *passes)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;

	*passes = 1;

	for (cn = hn; cn; cn = cn->sib) {
		if (!cn->v)
			continue;
		if (!strcmp(cn->key, "host_list")) {
			*passes = 0;
			if (cn->v->type == DM_CFG_EMPTY_ARRAY)
				continue;
			for (cv = cn->v; cv; cv = cv->next) {
				if (cv->type != DM_CFG_STRING) {
					log_error("Invalid hostname string "
						  "for tag %s", cn->key);
					return 0;
				}
				if (!strcmp(cv->v.str, cmd->hostname)) {
					*passes = 1;
					return 1;
				}
			}
		}
		if (!strcmp(cn->key, "host_filter")) {
			log_error("host_filter not supported yet");
			return 0;
		}
	}

	return 1;
}

static int _init_tags(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	const struct dm_config_node *tn, *cn;
	const char *tag;
	int passes;

	/* Access tags section directly */
	if (!(tn = find_config_node(cmd, cft, tags_CFG_SECTION)) || !tn->child)
		return 1;

	/* NB hosttags 0 when already 1 intentionally does not delete the tag */
	if (!cmd->hosttags && find_config_bool(cmd, cft, tags_hosttags_CFG)) {
		/* FIXME Strip out invalid chars: only A-Za-z0-9_+.- */
		if (!_set_tag(cmd, cmd->hostname))
			return_0;
		cmd->hosttags = 1;
	}

	for (cn = tn->child; cn; cn = cn->sib) {
		if (cn->v)
			continue;
		tag = cn->key;
		if (*tag == '@')
			tag++;
		if (!validate_name(tag)) {
			log_error("Invalid tag in config file: %s", cn->key);
			return 0;
		}
		if (cn->child) {
			passes = 0;
			if (!_check_host_filters(cmd, cn->child, &passes))
				return_0;
			if (!passes)
				continue;
		}
		if (!_set_tag(cmd, tag))
			return_0;
	}

	return 1;
}

static int _load_config_file(struct cmd_context *cmd, const char *tag, int local)
{
	static char config_file[PATH_MAX] = "";
	const char *filler = "";
	struct config_tree_list *cfl;

	if (*tag)
		filler = "_";
	else if (local) {
		filler = "";
		tag = "local";
	}

	if (dm_snprintf(config_file, sizeof(config_file), "%s/lvm%s%s.conf",
			 cmd->system_dir, filler, tag) < 0) {
		log_error("LVM_SYSTEM_DIR or tag was too long");
		return 0;
	}

	if (!(cfl = dm_pool_alloc(cmd->libmem, sizeof(*cfl)))) {
		log_error("config_tree_list allocation failed");
		return 0;
	}

	if (!(cfl->cft = config_file_open_and_read(config_file, CONFIG_FILE, cmd)))
		return_0;

	dm_list_add(&cmd->config_files, &cfl->list);

	if (*tag) {
		if (!_init_tags(cmd, cfl->cft))
			return_0;
	} else
		/* Use temporary copy of lvm.conf while loading other files */
		cmd->cft = cfl->cft;

	return 1;
}

/*
 * Find and read lvm.conf.
 */
static int _init_lvm_conf(struct cmd_context *cmd)
{
	/* No config file if LVM_SYSTEM_DIR is empty */
	if (!*cmd->system_dir) {
		if (!(cmd->cft = config_open(CONFIG_FILE, NULL, 0))) {
			log_error("Failed to create config tree");
			return 0;
		}
		return 1;
	}

	if (!_load_config_file(cmd, "", 0))
		return_0;

	return 1;
}

/* Read any additional config files */
static int _init_tag_configs(struct cmd_context *cmd)
{
	struct dm_str_list *sl;

	/* Tag list may grow while inside this loop */
	dm_list_iterate_items(sl, &cmd->tags) {
		if (!_load_config_file(cmd, sl->str, 0))
			return_0;
	}

	return 1;
}

static int _init_profiles(struct cmd_context *cmd)
{
	const char *dir;

	if (!(dir = find_config_tree_str(cmd, config_profile_dir_CFG, NULL)))
		return_0;

	if (!cmd->profile_params) {
		if (!(cmd->profile_params = dm_pool_zalloc(cmd->libmem, sizeof(*cmd->profile_params)))) {
			log_error("profile_params alloc failed");
			return 0;
		}
		dm_list_init(&cmd->profile_params->profiles_to_load);
		dm_list_init(&cmd->profile_params->profiles);
	}

	if (!(dm_strncpy(cmd->profile_params->dir, dir, sizeof(cmd->profile_params->dir)))) {
		log_error("_init_profiles: dm_strncpy failed");
		return 0;
	}

	return 1;
}

static struct dm_config_tree *_merge_config_files(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	struct config_tree_list *cfl;

	/* Replace temporary duplicate copy of lvm.conf */
	if (cft->root) {
		if (!(cft = config_open(CONFIG_MERGED_FILES, NULL, 0))) {
			log_error("Failed to create config tree");
			return 0;
		}
	}

	dm_list_iterate_items(cfl, &cmd->config_files) {
		/* Merge all config trees into cmd->cft using merge/tag rules */
		if (!merge_config_tree(cmd, cft, cfl->cft, CONFIG_MERGE_TYPE_TAGS))
			return_0;
	}

	return cft;
}

static void _destroy_tags(struct cmd_context *cmd)
{
	struct dm_list *slh, *slht;

	dm_list_iterate_safe(slh, slht, &cmd->tags) {
		dm_list_del(slh);
	}
}

int config_files_changed(struct cmd_context *cmd)
{
	struct config_tree_list *cfl;

	dm_list_iterate_items(cfl, &cmd->config_files) {
		if (config_file_changed(cfl->cft))
			return 1;
	}

	return 0;
}

static void _destroy_config(struct cmd_context *cmd)
{
	struct config_tree_list *cfl;
	struct dm_config_tree *cft;
	struct profile *profile, *tmp_profile;

	/*
	 * Configuration cascade:
	 * CONFIG_STRING -> CONFIG_PROFILE -> CONFIG_FILE/CONFIG_MERGED_FILES
	 */

	/* CONFIG_FILE/CONFIG_MERGED_FILES */
	if ((cft = remove_config_tree_by_source(cmd, CONFIG_MERGED_FILES)))
		config_destroy(cft);
	else
		remove_config_tree_by_source(cmd, CONFIG_FILE);

	dm_list_iterate_items(cfl, &cmd->config_files)
		config_destroy(cfl->cft);
	dm_list_init(&cmd->config_files);

	/* CONFIG_PROFILE */
	if (cmd->profile_params) {
		remove_config_tree_by_source(cmd, CONFIG_PROFILE_COMMAND);
		remove_config_tree_by_source(cmd, CONFIG_PROFILE_METADATA);
		/*
		 * Destroy config trees for any loaded profiles and
		 * move these profiles to profile_to_load list.
		 * Whenever these profiles are referenced later,
		 * they will get loaded again automatically.
		 */
		dm_list_iterate_items_safe(profile, tmp_profile, &cmd->profile_params->profiles) {
			config_destroy(profile->cft);
			profile->cft = NULL;
			dm_list_move(&cmd->profile_params->profiles_to_load, &profile->list);
		}
	}

	/* CONFIG_STRING */
	if ((cft = remove_config_tree_by_source(cmd, CONFIG_STRING)))
		config_destroy(cft);

	if (cmd->cft)
		log_error(INTERNAL_ERROR "_destroy_config: "
			  "cmd config tree not destroyed fully");
}

static int _init_dev_cache(struct cmd_context *cmd)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	size_t len, udev_dir_len = strlen(DM_UDEV_DEV_DIR);
	int len_diff;
	int device_list_from_udev;

	init_dev_disable_after_error_count(
		find_config_tree_int(cmd, devices_disable_after_error_count_CFG, NULL));

	if (!dev_cache_init(cmd))
		return_0;

	/*
	 * Override existing config and hardcode device_list_from_udev = 0 if:
	 *   - udev is not running
	 *   - udev is disabled using DM_DISABLE_UDEV environment variable
	 */
	if (_check_disable_udev("obtain device list by scanning device directory"))
		device_list_from_udev = 0;
	else
		device_list_from_udev = udev_is_running() ?
			find_config_tree_bool(cmd, devices_obtain_device_list_from_udev_CFG, NULL) : 0;

	init_obtain_device_list_from_udev(device_list_from_udev);

	if (!(cn = find_config_tree_node(cmd, devices_scan_CFG, NULL))) {
		if (!dev_cache_add_dir("/dev")) {
			log_error("Failed to add /dev to internal "
				  "device cache");
			return 0;
		}
		log_verbose("device/scan not in config file: "
			    "Defaulting to /dev");
		return 1;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_error("Invalid string in config file: "
				  "devices/scan");
			return 0;
		}

		if (device_list_from_udev) {
			len = strlen(cv->v.str);

			/*
			 * DM_UDEV_DEV_DIR always has '/' at its end.
			 * If the item in the conf does not have it, be sure
			 * to make the right comparison without the '/' char!
			 */
			len_diff = len && cv->v.str[len - 1] != '/' ?
					udev_dir_len - 1 != len :
					udev_dir_len != len;

			if (len_diff || strncmp(DM_UDEV_DEV_DIR, cv->v.str, len)) {
				log_very_verbose("Non standard udev dir %s, resetting "
						 "devices/obtain_device_list_from_udev.",
						 cv->v.str);
				device_list_from_udev = 0;
				init_obtain_device_list_from_udev(0);
			}
		}

		if (!dev_cache_add_dir(cv->v.str)) {
			log_error("Failed to add %s to internal device cache",
				  cv->v.str);
			return 0;
		}
	}

	if (!(cn = find_config_tree_node(cmd, devices_loopfiles_CFG, NULL)))
		return 1;

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_error("Invalid string in config file: "
				  "devices/loopfiles");
			return 0;
		}

		if (!dev_cache_add_loopfile(cv->v.str)) {
			log_error("Failed to add loopfile %s to internal "
				  "device cache", cv->v.str);
			return 0;
		}
	}


	return 1;
}

#define MAX_FILTERS 8

static struct dev_filter *_init_lvmetad_filter_chain(struct cmd_context *cmd)
{
	int nr_filt = 0;
	const struct dm_config_node *cn;
	struct dev_filter *filters[MAX_FILTERS] = { 0 };
	struct dev_filter *composite;

	/*
	 * Filters listed in order: top one gets applied first.
	 * Failure to initialise some filters is not fatal.
	 * Update MAX_FILTERS definition above when adding new filters.
	 */

	/*
	 * sysfs filter. Only available on 2.6 kernels.  Non-critical.
	 * Listed first because it's very efficient at eliminating
	 * unavailable devices.
	 */
	if (find_config_tree_bool(cmd, devices_sysfs_scan_CFG, NULL)) {
		if ((filters[nr_filt] = sysfs_filter_create()))
			nr_filt++;
	}

	/* regex filter. Optional. */
	if ((cn = find_config_tree_node(cmd, devices_global_filter_CFG, NULL))) {
		if (!(filters[nr_filt] = regex_filter_create(cn->v))) {
			log_error("Failed to create global regex device filter");
			goto bad;
		}
		nr_filt++;
	}

	/* device type filter. Required. */
	if (!(filters[nr_filt] = lvm_type_filter_create(cmd->dev_types))) {
		log_error("Failed to create lvm type filter");
		goto bad;
	}
	nr_filt++;

	/* usable device filter. Required. */
	if (!(filters[nr_filt] = usable_filter_create(cmd->dev_types,
						      lvmetad_used() ? FILTER_MODE_PRE_LVMETAD
								     : FILTER_MODE_NO_LVMETAD))) {
		log_error("Failed to create usabled device filter");
		goto bad;
	}
	nr_filt++;

	/* mpath component filter. Optional, non-critical. */
	if (find_config_tree_bool(cmd, devices_multipath_component_detection_CFG, NULL)) {
		if ((filters[nr_filt] = mpath_filter_create(cmd->dev_types)))
			nr_filt++;
	}

	/* partitioned device filter. Required. */
	if (!(filters[nr_filt] = partitioned_filter_create(cmd->dev_types))) {
		log_error("Failed to create partitioned device filter");
		goto bad;
	}
	nr_filt++;

	/* md component filter. Optional, non-critical. */
	if (find_config_tree_bool(cmd, devices_md_component_detection_CFG, NULL)) {
		init_md_filtering(1);
		if ((filters[nr_filt] = md_filter_create(cmd->dev_types)))
			nr_filt++;
	}

	/* firmware raid filter. Optional, non-critical. */
	if (find_config_tree_bool(cmd, devices_fw_raid_component_detection_CFG, NULL)) {
		init_fwraid_filtering(1);
		if ((filters[nr_filt] = fwraid_filter_create(cmd->dev_types)))
			nr_filt++;
	}

	if (!(composite = composite_filter_create(nr_filt, 1, filters)))
		goto_bad;

	return composite;

bad:
	while (--nr_filt >= 0)
		 filters[nr_filt]->destroy(filters[nr_filt]);

	return NULL;
}

/*
 * The way the filtering is initialized depends on whether lvmetad is uesd or not.
 *
 * If lvmetad is used, there are three filter chains:
 *
 *   - cmd->lvmetad_filter - the lvmetad filter chain used when scanning devs for lvmetad update:
 *     sysfs filter -> global regex filter -> type filter ->
 *     usable device filter(FILTER_MODE_PRE_LVMETAD) ->
 *     mpath component filter -> partitioned filter ->
 *     md component filter -> fw raid filter
 *
 *   - cmd->filter - the filter chain used for lvmetad responses:
 *     persistent filter -> usable device filter(FILTER_MODE_POST_LVMETAD) ->
 *     regex filter
 *
 *   - cmd->full_filter - the filter chain used for all the remaining situations:
 *     lvmetad_filter -> filter
 *
 * If lvmetad isnot used, there's just one filter chain:
 *
 *   - cmd->filter == cmd->full_filter:
 *     persistent filter -> regex filter -> sysfs filter ->
 *     global regex filter -> type filter ->
 *     usable device filter(FILTER_MODE_NO_LVMETAD) ->
 *     mpath component filter -> partitioned filter ->
 *     md component filter -> fw raid filter
 *
 */
static int _init_filters(struct cmd_context *cmd, unsigned load_persistent_cache)
{
	const char *dev_cache;
	struct dev_filter *filter = NULL, *filter_components[2] = {0};
	struct stat st;
	const struct dm_config_node *cn;
	struct timespec ts, cts;

	cmd->dump_filter = 0;

	cmd->lvmetad_filter = _init_lvmetad_filter_chain(cmd);
	if (!cmd->lvmetad_filter)
		goto_bad;

	init_ignore_suspended_devices(find_config_tree_bool(cmd, devices_ignore_suspended_devices_CFG, NULL));
	init_ignore_lvm_mirrors(find_config_tree_bool(cmd, devices_ignore_lvm_mirrors_CFG, NULL));

	/*
	 * If lvmetad is used, there's a separation between pre-lvmetad filter chain
	 * ("cmd->lvmetad_filter") applied only if scanning for lvmetad update and
	 * post-lvmetad filter chain ("filter") applied on each lvmetad response.
	 * However, if lvmetad is not used, these two chains are not separated
	 * and we use exactly one filter chain during device scanning ("filter"
	 * that includes also "cmd->lvmetad_filter" chain).
	 */
	/* filter component 0 */
	if (lvmetad_used()) {
		if (!(filter_components[0] = usable_filter_create(cmd->dev_types, FILTER_MODE_POST_LVMETAD))) {
			log_verbose("Failed to create usable device filter.");
			goto bad;
		}
	} else {
		filter_components[0] = cmd->lvmetad_filter;
		cmd->lvmetad_filter = NULL;
	}

	/* filter component 1 */
	if ((cn = find_config_tree_node(cmd, devices_filter_CFG, NULL))) {
		if (!(filter_components[1] = regex_filter_create(cn->v)))
			goto_bad;
		/* we have two filter components - create composite filter */
		if (!(filter = composite_filter_create(2, 0, filter_components)))
			goto_bad;
	} else
		/* we have only one filter component - no need to create composite filter */
		filter = filter_components[0];

	if (!(dev_cache = find_config_tree_str(cmd, devices_cache_CFG, NULL)))
		goto_bad;

	if (!(filter = persistent_filter_create(cmd->dev_types, filter, dev_cache))) {
		log_verbose("Failed to create persistent device filter.");
		goto bad;
	}

	cmd->filter = filter;

	if (lvmetad_used()) {
		filter_components[0] = cmd->lvmetad_filter;
		filter_components[1] = cmd->filter;
		if (!(cmd->full_filter = composite_filter_create(2, 0, filter_components)))
			goto_bad;
	} else
		cmd->full_filter = filter;

	/* Should we ever dump persistent filter state? */
	if (find_config_tree_bool(cmd, devices_write_cache_state_CFG, NULL))
		cmd->dump_filter = 1;

	if (!*cmd->system_dir)
		cmd->dump_filter = 0;

	/*
	 * Only load persistent filter device cache on startup if it is newer
	 * than the config file and this is not a long-lived process. Also avoid
	 * it when lvmetad is enabled.
	 */
	if (!find_config_tree_bool(cmd, global_use_lvmetad_CFG, NULL) &&
	    load_persistent_cache && !cmd->is_long_lived &&
	    !stat(dev_cache, &st)) {
		lvm_stat_ctim(&ts, &st);
		cts = config_file_timestamp(cmd->cft);
		if (timespeccmp(&ts, &cts, >) &&
		    !persistent_filter_load(cmd->filter, NULL))
			log_verbose("Failed to load existing device cache from %s",
				    dev_cache);
	}

	return 1;
bad:
	if (!filter) {
		/*
		 * composite filter not created - destroy
		 * each component directly
		 */
		if (filter_components[0])
			filter_components[0]->destroy(filter_components[0]);
		if (filter_components[1])
			filter_components[1]->destroy(filter_components[1]);
	} else {
		/*
		 * composite filter created - destroy it - this
		 * will also destroy any of its components
		 */
		filter->destroy(filter);
	}

	/* if lvmetad is used, the cmd->lvmetad_filter is separate */
	if (cmd->lvmetad_filter)
		cmd->lvmetad_filter->destroy(cmd->lvmetad_filter);

	return 0;
}

struct format_type *get_format_by_name(struct cmd_context *cmd, const char *format)
{
        struct format_type *fmt;

        dm_list_iterate_items(fmt, &cmd->formats)
                if (!strcasecmp(fmt->name, format) ||
                    !strcasecmp(fmt->name + 3, format) ||
                    (fmt->alias && !strcasecmp(fmt->alias, format)))
                        return fmt;

        return NULL;
}

static int _init_formats(struct cmd_context *cmd)
{
	const char *format;

	struct format_type *fmt;

#ifdef HAVE_LIBDL
	const struct dm_config_node *cn;
#endif

#ifdef LVM1_INTERNAL
	if (!(fmt = init_lvm1_format(cmd)))
		return 0;
	fmt->library = NULL;
	dm_list_add(&cmd->formats, &fmt->list);
#endif

#ifdef POOL_INTERNAL
	if (!(fmt = init_pool_format(cmd)))
		return 0;
	fmt->library = NULL;
	dm_list_add(&cmd->formats, &fmt->list);
#endif

#ifdef HAVE_LIBDL
	/* Load any formats in shared libs if not static */
	if (!is_static() &&
	    (cn = find_config_tree_node(cmd, global_format_libraries_CFG, NULL))) {

		const struct dm_config_value *cv;
		struct format_type *(*init_format_fn) (struct cmd_context *);
		void *lib;

		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Invalid string in config file: "
					  "global/format_libraries");
				return 0;
			}
			if (!(lib = load_shared_library(cmd, cv->v.str,
							"format", 0)))
				return_0;

			if (!(init_format_fn = dlsym(lib, "init_format"))) {
				log_error("Shared library %s does not contain "
					  "format functions", cv->v.str);
				dlclose(lib);
				return 0;
			}

			if (!(fmt = init_format_fn(cmd))) {
				dlclose(lib);
				return_0;
			}

			fmt->library = lib;
			dm_list_add(&cmd->formats, &fmt->list);
		}
	}
#endif

	if (!(fmt = create_text_format(cmd)))
		return 0;
	fmt->library = NULL;
	dm_list_add(&cmd->formats, &fmt->list);

	cmd->fmt_backup = fmt;

	format = find_config_tree_str(cmd, global_format_CFG, NULL);

	dm_list_iterate_items(fmt, &cmd->formats) {
		if (!strcasecmp(fmt->name, format) ||
		    (fmt->alias && !strcasecmp(fmt->alias, format))) {
			cmd->default_settings.fmt_name = fmt->name;
			cmd->fmt = fmt;
			return 1;
		}
	}

	log_error("_init_formats: Default format (%s) not found", format);
	return 0;
}

int init_lvmcache_orphans(struct cmd_context *cmd)
{
	struct format_type *fmt;

	dm_list_iterate_items(fmt, &cmd->formats)
		if (!lvmcache_add_orphan_vginfo(fmt->orphan_vg_name, fmt))
			return_0;

	return 1;
}

struct segtype_library {
	struct cmd_context *cmd;
	void *lib;
	const char *libname;
};

int lvm_register_segtype(struct segtype_library *seglib,
			 struct segment_type *segtype)
{
	struct segment_type *segtype2;

	segtype->library = seglib->lib;

	dm_list_iterate_items(segtype2, &seglib->cmd->segtypes) {
		if (strcmp(segtype2->name, segtype->name))
			continue;
		log_error("Duplicate segment type %s: "
			  "unloading shared library %s",
			  segtype->name, seglib->libname);
		segtype->ops->destroy(segtype);
		return 0;
	}

	dm_list_add(&seglib->cmd->segtypes, &segtype->list);

	return 1;
}

static int _init_single_segtype(struct cmd_context *cmd,
				struct segtype_library *seglib)
{
	struct segment_type *(*init_segtype_fn) (struct cmd_context *);
	struct segment_type *segtype;

	if (!(init_segtype_fn = dlsym(seglib->lib, "init_segtype"))) {
		log_error("Shared library %s does not contain segment type "
			  "functions", seglib->libname);
		return 0;
	}

	if (!(segtype = init_segtype_fn(seglib->cmd)))
		return_0;

	return lvm_register_segtype(seglib, segtype);
}

static int _init_segtypes(struct cmd_context *cmd)
{
	int i;
	struct segment_type *segtype;
	struct segtype_library seglib = { .cmd = cmd, .lib = NULL };
	struct segment_type *(*init_segtype_array[])(struct cmd_context *cmd) = {
		init_striped_segtype,
		init_zero_segtype,
		init_error_segtype,
		/* disabled until needed init_free_segtype, */
#ifdef SNAPSHOT_INTERNAL
		init_snapshot_segtype,
#endif
#ifdef MIRRORED_INTERNAL
		init_mirrored_segtype,
#endif
		NULL
	};

#ifdef HAVE_LIBDL
	const struct dm_config_node *cn;
#endif

	for (i = 0; init_segtype_array[i]; i++) {
		if (!(segtype = init_segtype_array[i](cmd)))
			return 0;
		segtype->library = NULL;
		dm_list_add(&cmd->segtypes, &segtype->list);
	}

#ifdef REPLICATOR_INTERNAL
	if (!init_replicator_segtype(cmd, &seglib))
		return 0;
#endif

#ifdef RAID_INTERNAL
	if (!init_raid_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef THIN_INTERNAL
	if (!init_thin_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef CACHE_INTERNAL
	if (!init_cache_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef HAVE_LIBDL
	/* Load any formats in shared libs unless static */
	if (!is_static() &&
	    (cn = find_config_tree_node(cmd, global_segment_libraries_CFG, NULL))) {

		const struct dm_config_value *cv;
		int (*init_multiple_segtypes_fn) (struct cmd_context *,
						  struct segtype_library *);

		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Invalid string in config file: "
					  "global/segment_libraries");
				return 0;
			}
			seglib.libname = cv->v.str;
			if (!(seglib.lib = load_shared_library(cmd,
							seglib.libname,
							"segment type", 0)))
				return_0;

			if ((init_multiple_segtypes_fn =
			    dlsym(seglib.lib, "init_multiple_segtypes"))) {
				if (dlsym(seglib.lib, "init_segtype"))
					log_warn("WARNING: Shared lib %s has "
						 "conflicting init fns.  Using"
						 " init_multiple_segtypes().",
						 seglib.libname);
			} else
				init_multiple_segtypes_fn =
				    _init_single_segtype;
 
			if (!init_multiple_segtypes_fn(cmd, &seglib)) {
				struct dm_list *sgtl, *tmp;
				log_error("init_multiple_segtypes() failed: "
					  "Unloading shared library %s",
					  seglib.libname);
				dm_list_iterate_safe(sgtl, tmp, &cmd->segtypes) {
					segtype = dm_list_item(sgtl, struct segment_type);
					if (segtype->library == seglib.lib) {
						dm_list_del(&segtype->list);
						segtype->ops->destroy(segtype);
					}
				}
				dlclose(seglib.lib);
				return_0;
			}
		}
	}
#endif

	return 1;
}

static int _init_hostname(struct cmd_context *cmd)
{
	struct utsname uts;

	if (uname(&uts)) {
		log_sys_error("uname", "_init_hostname");
		return 0;
	}

	if (!(cmd->hostname = dm_pool_strdup(cmd->libmem, uts.nodename))) {
		log_error("_init_hostname: dm_pool_strdup failed");
		return 0;
	}

	if (!(cmd->kernel_vsn = dm_pool_strdup(cmd->libmem, uts.release))) {
		log_error("_init_hostname: dm_pool_strdup kernel_vsn failed");
		return 0;
	}

	return 1;
}

static int _init_backup(struct cmd_context *cmd)
{
	uint32_t days, min;
	const char *dir;

	if (!cmd->system_dir[0]) {
		log_warn("WARNING: Metadata changes will NOT be backed up");
		backup_init(cmd, "", 0);
		archive_init(cmd, "", 0, 0, 0);
		return 1;
	}

	/* set up archiving */
	cmd->default_settings.archive =
	    find_config_tree_bool(cmd, backup_archive_CFG, NULL);

	days = (uint32_t) find_config_tree_int(cmd, backup_retain_days_CFG, NULL);

	min = (uint32_t) find_config_tree_int(cmd, backup_retain_min_CFG, NULL);

	if (!(dir = find_config_tree_str(cmd, backup_archive_dir_CFG, NULL)))
		return_0;

	if (!archive_init(cmd, dir, days, min,
			  cmd->default_settings.archive)) {
		log_debug("archive_init failed.");
		return 0;
	}

	/* set up the backup */
	cmd->default_settings.backup = find_config_tree_bool(cmd, backup_backup_CFG, NULL);

	if (!(dir = find_config_tree_str(cmd, backup_backup_dir_CFG, NULL)))
		return_0;

	if (!backup_init(cmd, dir, cmd->default_settings.backup)) {
		log_debug("backup_init failed.");
		return 0;
	}

	return 1;
}

static void _init_rand(struct cmd_context *cmd)
{
	if (read_urandom(&cmd->rand_seed, sizeof(cmd->rand_seed))) {
		reset_lvm_errno(1);
		return;
	}

	cmd->rand_seed = (unsigned) time(NULL) + (unsigned) getpid();
	reset_lvm_errno(1);
}

static void _init_globals(struct cmd_context *cmd)
{
	init_full_scan_done(0);
	init_mirror_in_sync(0);
}

/*
 * Close and reopen stream on file descriptor fd.
 */
static int _reopen_stream(FILE *stream, int fd, const char *mode, const char *name, FILE **new_stream)
{
	int fd_copy, new_fd;

	if ((fd_copy = dup(fd)) < 0) {
		log_sys_error("dup", name);
		return 0;
	}

	if (fclose(stream))
		log_sys_error("fclose", name);

	if ((new_fd = dup2(fd_copy, fd)) < 0)
		log_sys_error("dup2", name);
	else if (new_fd != fd)
		log_error("dup2(%d, %d) returned %d", fd_copy, fd, new_fd);

	if (close(fd_copy) < 0)
		log_sys_error("close", name);

	if (!(*new_stream = fdopen(fd, mode))) {
		log_sys_error("fdopen", name);
		return 0;
	}

	return 1;
}

/* Entry point */
struct cmd_context *create_toolcontext(unsigned is_long_lived,
				       const char *system_dir,
				       unsigned set_buffering,
				       unsigned threaded)
{
	struct cmd_context *cmd;
	FILE *new_stream;
	int flags;

#ifdef M_MMAP_MAX
	mallopt(M_MMAP_MAX, 0);
#endif

	if (!setlocale(LC_ALL, ""))
		log_very_verbose("setlocale failed");

#ifdef INTL_PACKAGE
	bindtextdomain(INTL_PACKAGE, LOCALEDIR);
#endif

	init_syslog(DEFAULT_LOG_FACILITY);

	if (!(cmd = dm_zalloc(sizeof(*cmd)))) {
		log_error("Failed to allocate command context");
		return NULL;
	}
	cmd->is_long_lived = is_long_lived;
	cmd->threaded = threaded ? 1 : 0;
	cmd->handles_missing_pvs = 0;
	cmd->handles_unknown_segments = 0;
	cmd->independent_metadata_areas = 0;
	cmd->ignore_clustered_vgs = 0;
	cmd->hosttags = 0;
	dm_list_init(&cmd->arg_value_groups);
	dm_list_init(&cmd->formats);
	dm_list_init(&cmd->segtypes);
	dm_list_init(&cmd->tags);
	dm_list_init(&cmd->config_files);
	label_init();

	/* FIXME Make this configurable? */
	reset_lvm_errno(1);

#ifndef VALGRIND_POOL
	/* Set in/out stream buffering before glibc */
	if (set_buffering) {
		/* Allocate 2 buffers */
		if (!(cmd->linebuffer = dm_malloc(2 * linebuffer_size))) {
			log_error("Failed to allocate line buffer.");
			goto out;
		}

		/* nohup might set stdin O_WRONLY ! */
		if (is_valid_fd(STDIN_FILENO) &&
		    ((flags = fcntl(STDIN_FILENO, F_GETFL)) > 0) &&
		    (flags & O_ACCMODE) != O_WRONLY) {
			if (!_reopen_stream(stdin, STDIN_FILENO, "r", "stdin", &new_stream))
				goto_out;
			stdin = new_stream;
			if (setvbuf(stdin, cmd->linebuffer, _IOLBF, linebuffer_size)) {
				log_sys_error("setvbuf", "");
				goto out;
			}
		}

		if (is_valid_fd(STDOUT_FILENO) &&
		    ((flags = fcntl(STDOUT_FILENO, F_GETFL)) > 0) &&
		    (flags & O_ACCMODE) != O_RDONLY) {
			if (!_reopen_stream(stdout, STDOUT_FILENO, "w", "stdout", &new_stream))
				goto_out;
			stdout = new_stream;
			if (setvbuf(stdout, cmd->linebuffer + linebuffer_size,
				     _IOLBF, linebuffer_size)) {
				log_sys_error("setvbuf", "");
				goto out;
			}
		}
		/* Buffers are used for lines without '\n' */
	} else
		/* Without buffering, must not use stdin/stdout */
		init_silent(1);
#endif

	/*
	 * Environment variable LVM_SYSTEM_DIR overrides this below.
	 */
        if (system_dir)
		strncpy(cmd->system_dir, system_dir, sizeof(cmd->system_dir) - 1);
	else
		strcpy(cmd->system_dir, DEFAULT_SYS_DIR);

	if (!_get_env_vars(cmd))
		goto_out;

	/* Create system directory if it doesn't already exist */
	if (*cmd->system_dir && !dm_create_dir(cmd->system_dir)) {
		log_error("Failed to create LVM2 system dir for metadata backups, config "
			  "files and internal cache.");
		log_error("Set environment variable LVM_SYSTEM_DIR to alternative location "
			  "or empty string.");
		goto out;
	}

	if (!(cmd->libmem = dm_pool_create("library", 4 * 1024))) {
		log_error("Library memory pool creation failed");
		goto out;
	}

	if (!(cmd->mem = dm_pool_create("command", 4 * 1024))) {
		log_error("Command memory pool creation failed");
		goto out;
	}

	if (!_init_lvm_conf(cmd))
		goto_out;

	_init_logging(cmd);

	if (!_init_hostname(cmd))
		goto_out;

	if (!_init_tags(cmd, cmd->cft))
		goto_out;

	/* Load lvmlocal.conf */
	if (*cmd->system_dir && !_load_config_file(cmd, "", 1))
		goto_out;

	if (!_init_tag_configs(cmd))
		goto_out;

	if (!(cmd->cft = _merge_config_files(cmd, cmd->cft)))
		goto_out;

	if (!_process_config(cmd))
		goto_out;

	if (!_init_profiles(cmd))
		goto_out;

	if (!(cmd->dev_types = create_dev_types(cmd->proc_dir,
						find_config_tree_node(cmd, devices_types_CFG, NULL))))
		goto_out;

	if (!_init_dev_cache(cmd))
		goto_out;

	if (!_init_filters(cmd, 1))
		goto_out;

	memlock_init(cmd);

	if (!_init_formats(cmd))
		goto_out;

	if (!init_lvmcache_orphans(cmd))
		goto_out;

	if (!_init_segtypes(cmd))
		goto_out;

	if (!_init_backup(cmd))
		goto_out;

	_init_rand(cmd);

	_init_globals(cmd);

	cmd->default_settings.cache_vgmetadata = 1;
	cmd->current_settings = cmd->default_settings;

	cmd->config_initialized = 1;
out:
	if (!cmd->config_initialized) {
		destroy_toolcontext(cmd);
		cmd = NULL;
	}


	return cmd;
}

static void _destroy_formats(struct cmd_context *cmd, struct dm_list *formats)
{
	struct dm_list *fmtl, *tmp;
	struct format_type *fmt;
	void *lib;

	dm_list_iterate_safe(fmtl, tmp, formats) {
		fmt = dm_list_item(fmtl, struct format_type);
		dm_list_del(&fmt->list);
		lib = fmt->library;
		fmt->ops->destroy(fmt);
#ifdef HAVE_LIBDL
		if (lib)
			dlclose(lib);
#endif
	}

	cmd->independent_metadata_areas = 0;
}

static void _destroy_segtypes(struct dm_list *segtypes)
{
	struct dm_list *sgtl, *tmp;
	struct segment_type *segtype;
	void *lib;

	dm_list_iterate_safe(sgtl, tmp, segtypes) {
		segtype = dm_list_item(sgtl, struct segment_type);
		dm_list_del(&segtype->list);
		lib = segtype->library;
		segtype->ops->destroy(segtype);
#ifdef HAVE_LIBDL
		/*
		 * If no segtypes remain from this library, close it.
		 */
		if (lib) {
			struct segment_type *segtype2;
			dm_list_iterate_items(segtype2, segtypes)
				if (segtype2->library == lib)
					goto skip_dlclose;
			dlclose(lib);
skip_dlclose:
			;
		}
#endif
	}
}

static void _destroy_dev_types(struct cmd_context *cmd)
{
	if (!cmd->dev_types)
		return;

	dm_free(cmd->dev_types);
	cmd->dev_types = NULL;
}

static void _destroy_filters(struct cmd_context *cmd)
{
	if (cmd->full_filter) {
		cmd->full_filter->destroy(cmd->full_filter);
		cmd->lvmetad_filter = cmd->filter = cmd->full_filter = NULL;
	}
}

int refresh_filters(struct cmd_context *cmd)
{
	int r, saved_ignore_suspended_devices = ignore_suspended_devices();

	_destroy_filters(cmd);
	if (!(r = _init_filters(cmd, 0)))
                stack;

	/*
	 * During repair code must not reset suspended flag.
	 */
	init_ignore_suspended_devices(saved_ignore_suspended_devices);

	return r;
}

int refresh_toolcontext(struct cmd_context *cmd)
{
	struct dm_config_tree *cft_cmdline, *cft_tmp;
	const char *profile_command_name, *profile_metadata_name;
	struct profile *profile;

	log_verbose("Reloading config files");

	/*
	 * Don't update the persistent filter cache as we will
	 * perform a full rescan.
	 */

	activation_release();
	lvmcache_destroy(cmd, 0, 0);
	label_exit();
	_destroy_segtypes(&cmd->segtypes);
	_destroy_formats(cmd, &cmd->formats);
	_destroy_filters(cmd);

	if (!dev_cache_exit())
		stack;
	_destroy_dev_types(cmd);
	_destroy_tags(cmd);

	/* save config string passed on the command line */
	cft_cmdline = remove_config_tree_by_source(cmd, CONFIG_STRING);

	/* save the global profile name used */
	profile_command_name = cmd->profile_params->global_command_profile ?
				cmd->profile_params->global_command_profile->name : NULL;
	profile_metadata_name = cmd->profile_params->global_metadata_profile ?
				cmd->profile_params->global_metadata_profile->name : NULL;

	_destroy_config(cmd);

	cmd->config_initialized = 0;

	cmd->hosttags = 0;

	cmd->lib_dir = NULL;

	if (!_init_lvm_conf(cmd))
		return_0;

	/* Temporary duplicate cft pointer holding lvm.conf - replaced later */
	cft_tmp = cmd->cft;
	if (cft_cmdline)
		cmd->cft = dm_config_insert_cascaded_tree(cft_cmdline, cft_tmp);

	/* Reload the global profile. */
	if (profile_command_name) {
		if (!(profile = add_profile(cmd, profile_command_name, CONFIG_PROFILE_COMMAND)) ||
		    !override_config_tree_from_profile(cmd, profile))
			return_0;
	}
	if (profile_metadata_name) {
		if (!(profile = add_profile(cmd, profile_metadata_name, CONFIG_PROFILE_METADATA)) ||
		    !override_config_tree_from_profile(cmd, profile))
			return_0;
	}

	/* Uses cmd->cft i.e. cft_cmdline + lvm.conf */
	_init_logging(cmd);

	/* Init tags from lvm.conf. */
	if (!_init_tags(cmd, cft_tmp))
		return_0;

	/* Load lvmlocal.conf */
	if (*cmd->system_dir && !_load_config_file(cmd, "", 1))
		return_0;

	/* Doesn't change cmd->cft */
	if (!_init_tag_configs(cmd))
		return_0;

	/* Merge all the tag config files with lvm.conf, returning a
	 * fresh cft pointer in place of cft_tmp. */
	if (!(cmd->cft = _merge_config_files(cmd, cft_tmp)))
		return_0;

	/* Finally we can make the proper, fully-merged, cmd->cft */
	if (cft_cmdline)
		cmd->cft = dm_config_insert_cascaded_tree(cft_cmdline, cmd->cft);

	if (!_process_config(cmd))
		return_0;

	if (!_init_profiles(cmd))
		return_0;

	if (!(cmd->dev_types = create_dev_types(cmd->proc_dir,
						find_config_tree_node(cmd, devices_types_CFG, NULL))))
		return_0;

	if (!_init_dev_cache(cmd))
		return_0;

	if (!_init_filters(cmd, 0))
		return_0;

	if (!_init_formats(cmd))
		return_0;

	if (!init_lvmcache_orphans(cmd))
		return_0;

	if (!_init_segtypes(cmd))
		return_0;

	if (!_init_backup(cmd))
		return_0;

	cmd->config_initialized = 1;

	reset_lvm_errno(1);
	return 1;
}

void destroy_toolcontext(struct cmd_context *cmd)
{
	struct dm_config_tree *cft_cmdline;
	FILE *new_stream;
	int flags;

	if (cmd->dump_filter && cmd->filter && cmd->filter->dump &&
	    !cmd->filter->dump(cmd->filter, 1))
		stack;

	archive_exit(cmd);
	backup_exit(cmd);
	lvmcache_destroy(cmd, 0, 0);
	label_exit();
	_destroy_segtypes(&cmd->segtypes);
	_destroy_formats(cmd, &cmd->formats);
	_destroy_filters(cmd);
	if (cmd->mem)
		dm_pool_destroy(cmd->mem);
	dev_cache_exit();
	_destroy_dev_types(cmd);
	_destroy_tags(cmd);

	if ((cft_cmdline = remove_config_tree_by_source(cmd, CONFIG_STRING)))
		config_destroy(cft_cmdline);
	_destroy_config(cmd);

	if (cmd->cft_def_hash)
		dm_hash_destroy(cmd->cft_def_hash);

	if (cmd->libmem)
		dm_pool_destroy(cmd->libmem);

#ifndef VALGRIND_POOL
	if (cmd->linebuffer) {
		/* Reset stream buffering to defaults */
		if (is_valid_fd(STDIN_FILENO) &&
		    ((flags = fcntl(STDIN_FILENO, F_GETFL)) > 0) &&
		    (flags & O_ACCMODE) != O_WRONLY) {
			if (_reopen_stream(stdin, STDIN_FILENO, "r", "stdin", &new_stream)) {
				stdin = new_stream;
				setlinebuf(stdin);
			} else
				cmd->linebuffer = NULL;	/* Leave buffer in place (deliberate leak) */
		}

		if (is_valid_fd(STDOUT_FILENO) &&
		    ((flags = fcntl(STDOUT_FILENO, F_GETFL)) > 0) &&
		    (flags & O_ACCMODE) != O_RDONLY) {
			if (_reopen_stream(stdout, STDOUT_FILENO, "w", "stdout", &new_stream)) {
				stdout = new_stream;
				setlinebuf(stdout);
			} else
				cmd->linebuffer = NULL;	/* Leave buffer in place (deliberate leak) */
		}

		dm_free(cmd->linebuffer);
	}
#endif
	dm_free(cmd);

	lvmetad_release_token();
	lvmetad_disconnect();
	lvmpolld_disconnect();

	release_log_memory();
	activation_exit();
	reset_log_duplicated();
	fin_log();
	fin_syslog();
	reset_lvm_errno(0);
}
