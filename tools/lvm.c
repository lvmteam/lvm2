/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"
#include "label.h"
#include "version.h"

#include "stub.h"

#include <signal.h>
#include <syslog.h>
#include <libgen.h>
#include <sys/stat.h>
#include <time.h>

#ifdef HAVE_GETOPTLONG
#  include <getopt.h>
#  define GETOPTLONG_FN(a, b, c, d, e) getopt_long((a), (b), (c), (d), (e))
#  define OPTIND_INIT 0
#else
struct option {
};
extern int optind;
extern char *optarg;
#  define GETOPTLONG_FN(a, b, c, d, e) getopt((a), (b), (c))
#  define OPTIND_INIT 1
#endif

#ifdef READLINE_SUPPORT
#  include <readline/readline.h>
#  include <readline/history.h>
#  ifndef HAVE_RL_COMPLETION_MATCHES
#    define rl_completion_matches(a, b) completion_matches((char *)a, b)
#  endif
#endif

/*
 * Exported table of valid switches
 */
struct arg the_args[ARG_COUNT + 1] = {

#define arg(a, b, c, d) {b, "--" c, d, 0, NULL, 0, 0, INT64_C(0), UINT64_C(0), 0, NULL},
#include "args.h"
#undef arg

};

static int _array_size;
static int _num_commands;
static struct command *_commands;

static int _interactive;

int yes_no_arg(struct cmd_context *cmd, struct arg *a)
{
	a->sign = SIGN_NONE;

	if (!strcmp(a->value, "y")) {
		a->i_value = 1;
		a->ui_value = 1;
	}

	else if (!strcmp(a->value, "n")) {
		a->i_value = 0;
		a->ui_value = 0;
	}

	else
		return 0;

	return 1;
}

int metadatatype_arg(struct cmd_context *cmd, struct arg *a)
{
	struct format_type *fmt;
	char *format;

	format = a->value;

	list_iterate_items(fmt, &cmd->formats) {
		if (!strcasecmp(fmt->name, format) ||
		    !strcasecmp(fmt->name + 3, format) ||
		    (fmt->alias && !strcasecmp(fmt->alias, format))) {
			a->ptr = fmt;
			return 1;
		}
	}

	return 0;
}

static int _get_int_arg(struct arg *a, char **ptr)
{
	char *val;
	long v;

	val = a->value;
	switch (*val) {
	case '+':
		a->sign = SIGN_PLUS;
		val++;
		break;
	case '-':
		a->sign = SIGN_MINUS;
		val++;
		break;
	default:
		a->sign = SIGN_NONE;
	}

	if (!isdigit(*val))
		return 0;

	v = strtol(val, ptr, 10);

	if (*ptr == val)
		return 0;

	a->i_value = (int32_t) v;
	a->ui_value = (uint32_t) v;
	a->i64_value = (int64_t) v;
	a->ui64_value = (uint64_t) v;

	return 1;
}

static int _size_arg(struct cmd_context *cmd, struct arg *a, int factor)
{
	char *ptr;
	int i;
	static const char *suffixes = "kmgt";
	char *val;
	double v;

	val = a->value;
	switch (*val) {
	case '+':
		a->sign = SIGN_PLUS;
		val++;
		break;
	case '-':
		a->sign = SIGN_MINUS;
		val++;
		break;
	default:
		a->sign = SIGN_NONE;
	}

	if (!isdigit(*val))
		return 0;

	v = strtod(val, &ptr);

	if (ptr == val)
		return 0;

	if (*ptr) {
		for (i = strlen(suffixes) - 1; i >= 0; i--)
			if (suffixes[i] == tolower((int) *ptr))
				break;

		if (i < 0)
			return 0;

		while (i-- > 0)
			v *= 1024;
	} else
		v *= factor;

	a->i_value = (int32_t) v;
	a->ui_value = (uint32_t) v;
	a->i64_value = (int64_t) v;
	a->ui64_value = (uint64_t) v;

	return 1;
}

int size_kb_arg(struct cmd_context *cmd, struct arg *a)
{
	return _size_arg(cmd, a, 1);
}

int size_mb_arg(struct cmd_context *cmd, struct arg *a)
{
	return _size_arg(cmd, a, 1024);
}

int int_arg(struct cmd_context *cmd, struct arg *a)
{
	char *ptr;

	if (!_get_int_arg(a, &ptr) || (*ptr) || (a->sign == SIGN_MINUS))
		return 0;

	return 1;
}

int int_arg_with_sign(struct cmd_context *cmd, struct arg *a)
{
	char *ptr;

	if (!_get_int_arg(a, &ptr) || (*ptr))
		return 0;

	return 1;
}

int minor_arg(struct cmd_context *cmd, struct arg *a)
{
	char *ptr;

	if (!_get_int_arg(a, &ptr) || (*ptr) || (a->sign == SIGN_MINUS))
		return 0;

	if (a->i_value > 255) {
		log_error("Minor number outside range 0-255");
		return 0;
	}

	return 1;
}

int major_arg(struct cmd_context *cmd, struct arg *a)
{
	char *ptr;

	if (!_get_int_arg(a, &ptr) || (*ptr) || (a->sign == SIGN_MINUS))
		return 0;

	if (a->i_value > 255) {
		log_error("Major number outside range 0-255");
		return 0;
	}

	/* FIXME Also Check against /proc/devices */

	return 1;
}

int string_arg(struct cmd_context *cmd, struct arg *a)
{
	return 1;
}

int permission_arg(struct cmd_context *cmd, struct arg *a)
{
	a->sign = SIGN_NONE;

	if ((!strcmp(a->value, "rw")) || (!strcmp(a->value, "wr")))
		a->ui_value = LVM_READ | LVM_WRITE;

	else if (!strcmp(a->value, "r"))
		a->ui_value = LVM_READ;

	else
		return 0;

	return 1;
}

char yes_no_prompt(const char *prompt, ...)
{
	int c = 0;
	va_list ap;

	while (c != 'y' && c != 'n') {
		if (c == '\n' || c == 0) {
			va_start(ap, prompt);
			vprintf(prompt, ap);
			va_end(ap);
		}
		c = tolower(getchar());
	}

	while (getchar() != '\n') ;

	return c;
}

static void __alloc(int size)
{
	if (!(_commands = dbg_realloc(_commands, sizeof(*_commands) * size))) {
		log_fatal("Couldn't allocate memory.");
		exit(ECMD_FAILED);
	}

	_array_size = size;
}

static void _alloc_command(void)
{
	if (!_array_size)
		__alloc(32);

	if (_array_size <= _num_commands)
		__alloc(2 * _array_size);
}

static void _create_new_command(const char *name, command_fn command,
				const char *desc, const char *usagestr,
				int nargs, int *args)
{
	struct command *nc;

	_alloc_command();

	nc = _commands + _num_commands++;

	nc->name = name;
	nc->desc = desc;
	nc->usage = usagestr;
	nc->fn = command;
	nc->num_args = nargs;
	nc->valid_args = args;
}

static void _register_command(const char *name, command_fn fn,
			      const char *desc, const char *usagestr, ...)
{
	int nargs = 0, i;
	int *args;
	va_list ap;

	/* count how many arguments we have */
	va_start(ap, usagestr);
	while (va_arg(ap, int) >= 0)
		 nargs++;
	va_end(ap);

	/* allocate space for them */
	if (!(args = dbg_malloc(sizeof(*args) * nargs))) {
		log_fatal("Out of memory.");
		exit(ECMD_FAILED);
	}

	/* fill them in */
	va_start(ap, usagestr);
	for (i = 0; i < nargs; i++)
		args[i] = va_arg(ap, int);
	va_end(ap);

	/* enter the command in the register */
	_create_new_command(name, fn, desc, usagestr, nargs, args);
}

static void _register_commands()
{
#define xx(a, b, c...) _register_command(# a, a, b, ## c, \
					driverloaded_ARG, \
                                        debug_ARG, help_ARG, help2_ARG, \
                                        version_ARG, verbose_ARG, \
					quiet_ARG, -1);
#include "commands.h"
#undef xx
}

static struct command *_find_command(const char *name)
{
	int i;
	char *namebase, *base;

	namebase = strdup(name);
	base = basename(namebase);

	for (i = 0; i < _num_commands; i++) {
		if (!strcmp(base, _commands[i].name))
			break;
	}

	free(namebase);

	if (i >= _num_commands)
		return 0;

	return _commands + i;
}

static void _usage(const char *name)
{
	struct command *com = _find_command(name);

	if (!com)
		return;

	log_error("%s: %s\n\n%s", com->name, com->desc, com->usage);
}

/*
 * Sets up the short and long argument.  If there
 * is no short argument then the index of the
 * argument in the the_args array is set as the
 * long opt value.  Yuck.  Of course this means we
 * can't have more than 'a' long arguments.
 */
static void _add_getopt_arg(int arg, char **ptr, struct option **o)
{
	struct arg *a = the_args + arg;

	if (a->short_arg) {
		*(*ptr)++ = a->short_arg;

		if (a->fn)
			*(*ptr)++ = ':';
	}
#ifdef HAVE_GETOPTLONG
	if (*(a->long_arg + 2)) {
		(*o)->name = a->long_arg + 2;
		(*o)->has_arg = a->fn ? 1 : 0;
		(*o)->flag = NULL;
		if (a->short_arg)
			(*o)->val = a->short_arg;
		else
			(*o)->val = arg;
		(*o)++;
	}
#endif
}

static struct arg *_find_arg(struct command *com, int opt)
{
	struct arg *a;
	int i, arg;

	for (i = 0; i < com->num_args; i++) {
		arg = com->valid_args[i];
		a = the_args + arg;

		/*
		 * opt should equal either the
		 * short arg, or the index into
		 * 'the_args'.
		 */
		if ((a->short_arg && (opt == a->short_arg)) ||
		    (!a->short_arg && (opt == arg)))
			return a;
	}

	return 0;
}

static int _process_command_line(struct cmd_context *cmd, int *argc,
				 char ***argv)
{
	int i, opt;
	char str[((ARG_COUNT + 1) * 2) + 1], *ptr = str;
	struct option opts[ARG_COUNT + 1], *o = opts;
	struct arg *a;

	for (i = 0; i < ARG_COUNT; i++) {
		a = the_args + i;

		/* zero the count and arg */
		a->count = 0;
		a->value = 0;
		a->i_value = 0;
		a->ui_value = 0;
		a->i64_value = 0;
		a->ui64_value = 0;
	}

	/* fill in the short and long opts */
	for (i = 0; i < cmd->command->num_args; i++)
		_add_getopt_arg(cmd->command->valid_args[i], &ptr, &o);

	*ptr = '\0';
	memset(o, 0, sizeof(*o));

	/* initialise getopt_long & scan for command line switches */
	optarg = 0;
	optind = OPTIND_INIT;
	while ((opt = GETOPTLONG_FN(*argc, *argv, str, opts, NULL)) >= 0) {

		if (opt == '?')
			return 0;

		a = _find_arg(cmd->command, opt);

		if (!a) {
			log_fatal("Unrecognised option.");
			return 0;
		}

		if (a->fn) {
			if (a->count) {
				log_error("Option%s%c%s%s may not be repeated",
					  a->short_arg ? " -" : "",
					  a->short_arg ? : ' ',
					  (a->short_arg && a->long_arg) ? 
						"/" : "",
					  a->long_arg ? : "");
					return 0;
			}

			if (!optarg) {
				log_error("Option requires argument.");
				return 0;
			}

			a->value = optarg;

			if (!a->fn(cmd, a)) {
				log_error("Invalid argument %s", optarg);
				return 0;
			}
		}

		a->count++;
	}

	*argc -= optind;
	*argv += optind;
	return 1;
}

static int _merge_synonym(struct cmd_context *cmd, int oldarg, int newarg)
{
	const struct arg *old;
	struct arg *new;

	if (arg_count(cmd, oldarg) && arg_count(cmd, newarg)) {
		log_error("%s and %s are synonyms.  Please only supply one.",
			  the_args[oldarg].long_arg, the_args[newarg].long_arg);
		return 0;
	}

	if (!arg_count(cmd, oldarg))
		return 1;

	old = the_args + oldarg;
	new = the_args + newarg;

	new->count = old->count;
	new->value = old->value;
	new->i_value = old->i_value;
	new->ui_value = old->ui_value;
	new->i64_value = old->i64_value;
	new->ui64_value = old->ui64_value;
	new->sign = old->sign;

	return 1;
}

int version(struct cmd_context *cmd, int argc, char **argv)
{
	char vsn[80];

	log_print("LVM version:     %s", LVM_VERSION);
	if (library_version(vsn, sizeof(vsn)))
		log_print("Library version: %s", vsn);
	if (driver_version(vsn, sizeof(vsn)))
		log_print("Driver version:  %s", vsn);

	return ECMD_PROCESSED;
}

static int _get_settings(struct cmd_context *cmd)
{
	cmd->current_settings = cmd->default_settings;

	if (arg_count(cmd, debug_ARG))
		cmd->current_settings.debug = _LOG_FATAL +
		    (arg_count(cmd, debug_ARG) - 1);

	if (arg_count(cmd, verbose_ARG))
		cmd->current_settings.verbose = arg_count(cmd, verbose_ARG);

	if (arg_count(cmd, quiet_ARG)) {
		cmd->current_settings.debug = 0;
		cmd->current_settings.verbose = 0;
	}

	if (arg_count(cmd, test_ARG))
		cmd->current_settings.test = arg_count(cmd, test_ARG);

	if (arg_count(cmd, driverloaded_ARG)) {
		cmd->current_settings.activation =
		    arg_int_value(cmd, driverloaded_ARG,
				  cmd->default_settings.activation);
	}

	if (arg_count(cmd, autobackup_ARG)) {
		cmd->current_settings.archive = 1;
		cmd->current_settings.backup = 1;
	}

	if (arg_count(cmd, partial_ARG)) {
		init_partial(1);
		log_print("Partial mode. Incomplete volume groups will "
			  "be activated read-only.");
	} else
		init_partial(0);

	if (arg_count(cmd, ignorelockingfailure_ARG))
		init_ignorelockingfailure(1);
	else
		init_ignorelockingfailure(0);

	if (arg_count(cmd, nosuffix_ARG))
		cmd->current_settings.suffix = 0;

	if (arg_count(cmd, units_ARG))
		if (!(cmd->current_settings.unit_factor =
		      units_to_bytes(arg_str_value(cmd, units_ARG, ""),
				     &cmd->current_settings.unit_type))) {
			log_error("Invalid units specification");
			return EINVALID_CMD_LINE;
		}

	/* Handle synonyms */
	if (!_merge_synonym(cmd, resizable_ARG, resizeable_ARG) ||
	    !_merge_synonym(cmd, allocation_ARG, allocatable_ARG) ||
	    !_merge_synonym(cmd, allocation_ARG, resizeable_ARG))
		return EINVALID_CMD_LINE;

	/* Zero indicates success */
	return 0;
}

static int _process_common_commands(struct cmd_context *cmd)
{
	if (arg_count(cmd, help_ARG) || arg_count(cmd, help2_ARG)) {
		_usage(cmd->command->name);
		return ECMD_PROCESSED;
	}

	if (arg_count(cmd, version_ARG)) {
		return version(cmd, 0, (char **) NULL);
	}

	/* Zero indicates it's OK to continue processing this command */
	return 0;
}

static void _display_help(void)
{
	int i;

	log_error("Available lvm commands:");
	log_error("Use 'lvm help <command>' for more information");
	log_error(" ");

	for (i = 0; i < _num_commands; i++) {
		struct command *com = _commands + i;

		log_error("%-16.16s%s", com->name, com->desc);
	}
}

int help(struct cmd_context *cmd, int argc, char **argv)
{
	if (!argc)
		_display_help();
	else {
		int i;
		for (i = 0; i < argc; i++)
			_usage(argv[i]);
	}

	return 0;
}

static void _apply_settings(struct cmd_context *cmd)
{
	init_debug(cmd->current_settings.debug);
	init_verbose(cmd->current_settings.verbose);
	init_test(cmd->current_settings.test);

	init_msg_prefix(cmd->default_settings.msg_prefix);
	init_cmd_name(cmd->default_settings.cmd_name);

	archive_enable(cmd->current_settings.archive);
	backup_enable(cmd->current_settings.backup);

	set_activation(cmd->current_settings.activation);

	cmd->fmt = arg_ptr_value(cmd, metadatatype_ARG,
				 cmd->current_settings.fmt);
}

static char *_copy_command_line(struct cmd_context *cmd, int argc, char **argv)
{
	int i;

	/*
	 * Build up the complete command line, used as a
	 * description for backups.
	 */
	if (!pool_begin_object(cmd->mem, 128))
		goto bad;

	for (i = 0; i < argc; i++) {
		if (!pool_grow_object(cmd->mem, argv[i], strlen(argv[i])))
			goto bad;

		if (i < (argc - 1))
			if (!pool_grow_object(cmd->mem, " ", 1))
				goto bad;
	}

	/*
	 * Terminate.
	 */
	if (!pool_grow_object(cmd->mem, "\0", 1))
		goto bad;

	return pool_end_object(cmd->mem);

      bad:
	log_err("Couldn't copy command line.");
	pool_abandon_object(cmd->mem);
	return NULL;
}

static int _run_command(struct cmd_context *cmd, int argc, char **argv)
{
	int ret = 0;
	int locking_type;

	if (!(cmd->cmd_line = _copy_command_line(cmd, argc, argv)))
		return ECMD_FAILED;

	log_debug("Processing: %s", cmd->cmd_line);

	if (!(cmd->command = _find_command(argv[0])))
		return ENO_SUCH_CMD;

	if (!_process_command_line(cmd, &argc, &argv)) {
		log_error("Error during parsing of command line.");
		return EINVALID_CMD_LINE;
	}

	set_cmd_name(cmd->command->name);

	if (reload_config_file(&cmd->cf)) {
		;
		/* FIXME Reinitialise various settings inc. logging, filters */
	}

	if ((ret = _get_settings(cmd)))
		goto out;
	_apply_settings(cmd);

	if ((ret = _process_common_commands(cmd)))
		goto out;

	locking_type = find_config_int(cmd->cf->root, "global/locking_type",
				       '/', 1);
	if (!init_locking(locking_type, cmd->cf)) {
		log_error("Locking type %d initialisation failed.",
			  locking_type);
		ret = ECMD_FAILED;
		goto out;
	}

	ret = cmd->command->fn(cmd, argc, argv);

	fin_locking();

      out:
	if (test_mode()) {
		log_verbose("Test mode: Wiping internal cache");
		lvmcache_destroy();
	}

	cmd->current_settings = cmd->default_settings;
	_apply_settings(cmd);

	/*
	 * free off any memory the command used.
	 */
	pool_empty(cmd->mem);

	if (ret == EINVALID_CMD_LINE && !_interactive)
		_usage(cmd->command->name);

	return ret;
}

static int _split(char *str, int *argc, char **argv, int max)
{
	char *b = str, *e;
	*argc = 0;

	while (*b) {
		while (*b && isspace(*b))
			b++;

		if ((!*b) || (*b == '#'))
			break;

		e = b;
		while (*e && !isspace(*e))
			e++;

		argv[(*argc)++] = b;
		if (!*e)
			break;
		*e++ = '\0';
		b = e;
		if (*argc == max)
			break;
	}

	return *argc;
}

static void _init_rand(void)
{
	srand((unsigned int) time(NULL) + (unsigned int) getpid());
}

static int _init_backup(struct cmd_context *cmd, struct config_tree *cf)
{
	uint32_t days, min;
	char default_dir[PATH_MAX];
	const char *dir;

	if (!cmd->sys_dir) {
		log_warn("WARNING: Metadata changes will NOT be backed up");
		backup_init("");
		archive_init("", 0, 0);
		return 1;
	}

	/* set up archiving */
	cmd->default_settings.archive =
	    find_config_bool(cmd->cf->root, "backup/archive", '/',
			     DEFAULT_ARCHIVE_ENABLED);

	days = (uint32_t) find_config_int(cmd->cf->root, "backup/retain_days",
					  '/', DEFAULT_ARCHIVE_DAYS);

	min = (uint32_t) find_config_int(cmd->cf->root, "backup/retain_min",
					 '/', DEFAULT_ARCHIVE_NUMBER);

	if (lvm_snprintf
	    (default_dir, sizeof(default_dir), "%s/%s", cmd->sys_dir,
	     DEFAULT_ARCHIVE_SUBDIR) == -1) {
		log_err("Couldn't create default archive path '%s/%s'.",
			cmd->sys_dir, DEFAULT_ARCHIVE_SUBDIR);
		return 0;
	}

	dir = find_config_str(cmd->cf->root, "backup/archive_dir", '/',
			      default_dir);

	if (!archive_init(dir, days, min)) {
		log_debug("backup_init failed.");
		return 0;
	}

	/* set up the backup */
	cmd->default_settings.backup =
	    find_config_bool(cmd->cf->root, "backup/backup", '/',
			     DEFAULT_BACKUP_ENABLED);

	if (lvm_snprintf
	    (default_dir, sizeof(default_dir), "%s/%s", cmd->sys_dir,
	     DEFAULT_BACKUP_SUBDIR) == -1) {
		log_err("Couldn't create default backup path '%s/%s'.",
			cmd->sys_dir, DEFAULT_BACKUP_SUBDIR);
		return 0;
	}

	dir = find_config_str(cmd->cf->root, "backup/backup_dir", '/',
			      default_dir);

	if (!backup_init(dir)) {
		log_debug("backup_init failed.");
		return 0;
	}

	return 1;
}

static struct cmd_context *_init(void)
{
	struct cmd_context *cmd;

	if (!(cmd = create_toolcontext(&the_args[0]))) {
		stack;
		return NULL;
	}

	_init_rand();

	if (!_init_backup(cmd, cmd->cf))
		return NULL;

	_apply_settings(cmd);

	return cmd;
}

static void _fin_commands(struct cmd_context *cmd)
{
	int i;

	for (i = 0; i < _num_commands; i++)
		dbg_free(_commands[i].valid_args);

	dbg_free(_commands);
}

static void _fin(struct cmd_context *cmd)
{
	archive_exit();
	backup_exit();
	_fin_commands(cmd);

	destroy_toolcontext(cmd);
}

static int _run_script(struct cmd_context *cmd, int argc, char **argv)
{
	FILE *script;

	char buffer[CMD_LEN];
	int ret = 0;
	int magic_number = 0;

	if ((script = fopen(argv[0], "r")) == NULL)
		return ENO_SUCH_CMD;

	while (fgets(buffer, sizeof(buffer), script) != NULL) {
		if (!magic_number) {
			if (buffer[0] == '#' && buffer[1] == '!')
				magic_number = 1;
			else
				return ENO_SUCH_CMD;
		}
		if ((strlen(buffer) == sizeof(buffer) - 1)
		    && (buffer[sizeof(buffer) - 1] - 2 != '\n')) {
			buffer[50] = '\0';
			log_error("Line too long (max 255) beginning: %s",
				  buffer);
			ret = EINVALID_CMD_LINE;
			break;
		}
		if (_split(buffer, &argc, argv, MAX_ARGS) == MAX_ARGS) {
			buffer[50] = '\0';
			log_error("Too many arguments: %s", buffer);
			ret = EINVALID_CMD_LINE;
			break;
		}
		if (!argc)
			continue;
		if (!strcmp(argv[0], "quit") || !strcmp(argv[0], "exit"))
			break;
		_run_command(cmd, argc, argv);
	}

	fclose(script);
	return ret;
}

#ifdef READLINE_SUPPORT
/* List matching commands */
static char *_list_cmds(const char *text, int state)
{
	static int i = 0;
	static size_t len = 0;

	/* Initialise if this is a new completion attempt */
	if (!state) {
		i = 0;
		len = strlen(text);
	}

	while (i < _num_commands)
		if (!strncmp(text, _commands[i++].name, len))
			return strdup(_commands[i - 1].name);

	return NULL;
}

/* List matching arguments */
static char *_list_args(const char *text, int state)
{
	static int match_no = 0;
	static size_t len = 0;
	static struct command *com;

	/* Initialise if this is a new completion attempt */
	if (!state) {
		char *s = rl_line_buffer;
		int j = 0;

		match_no = 0;
		com = NULL;
		len = strlen(text);

		/* Find start of first word in line buffer */
		while (isspace(*s))
			s++;

		/* Look for word in list of commands */
		for (j = 0; j < _num_commands; j++) {
			const char *p;
			char *q = s;

			p = _commands[j].name;
			while (*p == *q) {
				p++;
				q++;
			}
			if ((!*p) && *q == ' ') {
				com = _commands + j;
				break;
			}
		}

		if (!com)
			return NULL;
	}

	/* Short form arguments */
	if (len < 3) {
		while (match_no < com->num_args) {
			char s[3];
			char c;
			if (!(c = (the_args +
				   com->valid_args[match_no++])->short_arg))
				continue;

			sprintf(s, "-%c", c);
			if (!strncmp(text, s, len))
				return strdup(s);
		}
	}

	/* Long form arguments */
	if (match_no < com->num_args)
		match_no = com->num_args;

	while (match_no - com->num_args < com->num_args) {
		const char *l;
		l = (the_args +
		     com->valid_args[match_no++ - com->num_args])->long_arg;
		if (*(l + 2) && !strncmp(text, l, len))
			return strdup(l);
	}

	return NULL;
}

/* Custom completion function */
static char **_completion(const char *text, int start_pos, int end_pos)
{
	char **match_list = NULL;
	int p = 0;

	while (isspace((int) *(rl_line_buffer + p)))
		p++;

	/* First word should be one of our commands */
	if (start_pos == p)
		match_list = rl_completion_matches(text, _list_cmds);

	else if (*text == '-')
		match_list = rl_completion_matches(text, _list_args);
	/* else other args */

	/* No further completion */
	rl_attempted_completion_over = 1;
	return match_list;
}

static int _hist_file(char *buffer, size_t size)
{
	char *e = getenv("HOME");

	if (lvm_snprintf(buffer, size, "%s/.lvm_history", e) < 0) {
		log_error("$HOME/.lvm_history: path too long");
		return 0;
	}

	return 1;
}

static void _read_history(struct cmd_context *cmd)
{
	char hist_file[PATH_MAX];

	if (!_hist_file(hist_file, sizeof(hist_file)))
		return;

	if (read_history(hist_file))
		log_very_verbose("Couldn't read history from %s.", hist_file);

	stifle_history(find_config_int(cmd->cf->root, "shell/history_size",
				       '/', DEFAULT_MAX_HISTORY));

}

static void _write_history(void)
{
	char hist_file[PATH_MAX];

	if (!_hist_file(hist_file, sizeof(hist_file)))
		return;

	if (write_history(hist_file))
		log_very_verbose("Couldn't write history to %s.", hist_file);
}

static int _shell(struct cmd_context *cmd)
{
	int argc, ret;
	char *input = NULL, *args[MAX_ARGS], **argv;

	rl_readline_name = "lvm";
	rl_attempted_completion_function = (CPPFunction *) _completion;

	_read_history(cmd);

	_interactive = 1;
	while (1) {
		free(input);
		input = readline("lvm> ");

		/* EOF */
		if (!input) {
			printf("\n");
			break;
		}

		/* empty line */
		if (!*input)
			continue;

		add_history(input);

		argv = args;

		if (_split(input, &argc, argv, MAX_ARGS) == MAX_ARGS) {
			log_error("Too many arguments, sorry.");
			continue;
		}

		if (!strcmp(argv[0], "lvm")) {
			argv++;
			argc--;
		}

		if (!argc)
			continue;

		if (!strcmp(argv[0], "quit") || !strcmp(argv[0], "exit")) {
			remove_history(history_length - 1);
			log_error("Exiting.");
			break;
		}

		ret = _run_command(cmd, argc, argv);
		if (ret == ENO_SUCH_CMD)
			log_error("No such command '%s'.  Try 'help'.",
				  argv[0]);

		_write_history();
	}

	free(input);
	return 0;
}

#endif

int main(int argc, char **argv)
{
	char *namebase, *base;
	int ret, alias = 0;
	struct cmd_context *cmd;

	if (!(cmd = _init()))
		return -1;

	cmd->argv = argv;
	namebase = strdup(argv[0]);
	base = basename(namebase);
	while (*base == '/')
		base++;
	if (strcmp(base, "lvm"))
		alias = 1;
	free(namebase);

	_register_commands();

#ifdef READLINE_SUPPORT
	if (!alias && argc == 1) {
		ret = _shell(cmd);
		goto out;
	}
#endif

	if (!alias) {
		if (argc < 2) {
			log_fatal("Please supply an LVM command.");
			_display_help();
			ret = EINVALID_CMD_LINE;
			goto out;
		}

		argc--;
		argv++;
	}

	ret = _run_command(cmd, argc, argv);
	if ((ret == ENO_SUCH_CMD) && (!alias))
		ret = _run_script(cmd, argc, argv);
	if (ret == ENO_SUCH_CMD)
		log_error("No such command.  Try 'help'.");

      out:
	_fin(cmd);
	if (ret == ECMD_PROCESSED)
		ret = 0;
	return ret;
}
