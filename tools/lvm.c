/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"
#include "archive.h"
#include "defaults.h"
#include "lvm1_label.h"
#include "label.h"
#include "version.h"

#include "stub.h"

#include <assert.h>
#include <getopt.h>
#include <signal.h>
#include <syslog.h>
#include <libgen.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>

#ifdef READLINE_SUPPORT
  #include <readline/readline.h>
  #include <readline/history.h>
  #ifndef HAVE_RL_COMPLETION_MATCHES
    #define rl_completion_matches(a, b) completion_matches((char *)a, b)
  #endif
#endif

/* 
 * Exported table of valid switches
 */
struct arg the_args[ARG_COUNT + 1] = {

#define arg(a, b, c, d) {b, "--" c, d, 0, NULL},
#include "args.h"
#undef arg

};


static int _array_size;
static int _num_commands;
static struct command *_commands;
struct cmd_context *cmd;

/* Whether or not to dump persistent filter state */
static int _dump_filter;

static int _interactive;
static FILE *_log;

/* lvm1 label handler */
static struct labeller *_lvm1_label;


/*
 * This structure only contains those options that
 * can have a default and per command setting.
 */
struct config_info {
	int debug;
	int verbose;
	int test;
	int syslog;

	int archive;		/* should we archive ? */
	int backup;		/* should we backup ? */

	mode_t umask;
};

static struct config_info _default_settings;
static struct config_info _current_settings;


/*
 * The lvm_sys_dir contains:
 *
 * o  The lvm configuration (lvm.conf)
 * o  The persistent filter cache (.cache)
 * o  Volume group backups (/backup)
 * o  Archive of old vg configurations (/archive)
 */
static char _sys_dir[PATH_MAX] = "/etc/lvm";
static char _dev_dir[PATH_MAX];
static char _proc_dir[PATH_MAX];

/* static functions */
static void register_commands(void);
static struct command *find_command(const char *name);
static void register_command(const char *name, command_fn fn,
			     const char *desc, const char *usage, ...);
static void create_new_command(const char *name, command_fn command,
			       const char *desc, const char *usage,
			       int nargs, int *args);

static void alloc_command(void);
static void add_getopt_arg(int arg, char **ptr, struct option **o);
static int process_command_line(struct command *com, int *argc, char ***argv);
static struct arg *find_arg(struct command *com, int a);
static int process_common_commands(struct command *com);
static int run_command(int argc, char **argv);
static int init(void);
static void fin(void);
static int run_script(int argc, char **argv);

#ifdef READLINE_SUPPORT
static int shell(void);
#endif

static void display_help(void);

int main(int argc, char **argv)
{
	char *namebase, *base;
	int ret, alias = 0;

	if (!init())
		return -1;

	namebase = strdup(argv[0]);
	base = basename(namebase);
	while (*base == '/')
		base++;
	if (strcmp(base, "lvm"))
		alias = 1;
	free(namebase);

	register_commands();

#ifdef READLINE_SUPPORT
	if (!alias && argc == 1) {
		ret = shell();
		goto out;
	}
#endif

	if (!alias) {
		if (argc < 2) {
			log_fatal("Please supply an LVM command.");
			display_help();
			ret = EINVALID_CMD_LINE;
			goto out;
		}

		argc--;
		argv++;
	}

	ret = run_command(argc, argv);
	if ((ret == ENO_SUCH_CMD) && (!alias))
		ret = run_script(argc, argv);
	if (ret == ENO_SUCH_CMD)
		log_error("No such command.  Try 'help'.");

      out:
	fin();
	return ret;
}

void usage(const char *name)
{
	struct command *com = find_command(name);

	if (!com)
		return;

	log_error("%s: %s\n\n%s", com->name, com->desc, com->usage);
}

int yes_no_arg(struct arg *a)
{
	a->sign = SIGN_NONE;

	if (!strcmp(a->value, "y"))
		a->i_value = 1;

	else if (!strcmp(a->value, "n"))
		a->i_value = 0;

	else
		return 0;

	return 1;
}

int _get_int_arg(struct arg *a, char **ptr)
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

	a->i_value = (uint32_t) v;
	return 1;
}

int size_arg(struct arg *a)
{
	char *ptr;
	int i;
	static char *suffixes = "kmgt";
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
	}
	a->i_value = (uint32_t) v;

	return 1;

}

int int_arg(struct arg *a)
{
	char *ptr;

	if (!_get_int_arg(a, &ptr) || (*ptr) || (a->sign == SIGN_MINUS))
		return 0;

	return 1;
}

int int_arg_with_sign(struct arg *a)
{
	char *ptr;

	if (!_get_int_arg(a, &ptr) || (*ptr))
		return 0;

	return 1;
}

int minor_arg(struct arg *a)
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

int string_arg(struct arg *a)
{
	return 1;
}

int permission_arg(struct arg *a)
{
	a->sign = SIGN_NONE;

	if ((!strcmp(a->value, "rw")) || (!strcmp(a->value, "wr")))
		a->i_value = LVM_READ | LVM_WRITE;

	else if (!strcmp(a->value, "r"))
		a->i_value = LVM_READ;

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

static void register_commands()
{
#define xx(a, b, c...) register_command(# a, a, b, ## c, \
                                        debug_ARG, help_ARG, suspend_ARG, \
                                        version_ARG, verbose_ARG, \
					quiet_ARG, -1);
#include "commands.h"
#undef xx
}

static void register_command(const char *name, command_fn fn,
			     const char *desc, const char *usage, ...)
{
	int nargs = 0, i;
	int *args;
	va_list ap;

	/* count how many arguments we have */
	va_start(ap, usage);
	while (va_arg(ap, int) >= 0)
		 nargs++;
	va_end(ap);

	/* allocate space for them */
	if (!(args = dbg_malloc(sizeof(*args) * nargs))) {
		log_fatal("Out of memory.");
		exit(ECMD_FAILED);
	}

	/* fill them in */
	va_start(ap, usage);
	for (i = 0; i < nargs; i++)
		args[i] = va_arg(ap, int);
	va_end(ap);

	/* enter the command in the register */
	create_new_command(name, fn, desc, usage, nargs, args);
}

static struct command *find_command(const char *name)
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

static void create_new_command(const char *name, command_fn command,
			       const char *desc, const char *usage,
			       int nargs, int *args)
{
	struct command *nc;

	alloc_command();

	nc = _commands + _num_commands++;

	nc->name = name;
	nc->desc = desc;
	nc->usage = usage;
	nc->fn = command;
	nc->num_args = nargs;
	nc->valid_args = args;
}

static void __alloc(int size)
{
	if (!(_commands = dbg_realloc(_commands, sizeof(*_commands) * size))) {
		log_fatal("Couldn't allocate memory.");
		exit(ECMD_FAILED);
	}

	_array_size = size;
}

static void alloc_command(void)
{
	if (!_array_size)
		__alloc(32);

	if (_array_size <= _num_commands)
		__alloc(2 * _array_size);
}

/*
 * Sets up the short and long argument.  If there
 * is no short argument then the index of the
 * argument in the the_args array is set as the
 * long opt value.  Yuck.  Of course this means we
 * can't have more than 'a' long arguments.  Since
 * we have only 1 ATM (--version) I think we can
 * live with this restriction.
 */
static void add_getopt_arg(int arg, char **ptr, struct option **o)
{
	struct arg *a = the_args + arg;

	if (a->short_arg) {
		*(*ptr)++ = a->short_arg;

		if (a->fn)
			*(*ptr)++ = ':';
	}

	if (*(a->long_arg + 2)) {
		(*o)->name = a->long_arg + 2;
		(*o)->has_arg = a->fn ? 1 : 0;
		(*o)->flag = NULL;
		(*o)->val = arg;
		(*o)++;
	}
}

static int process_command_line(struct command *com, int *argc, char ***argv)
{
	int i, opt;
	char str[((ARG_COUNT + 1) * 2) + 1], *ptr = str;
	struct option opts[ARG_COUNT + 1], *o = opts;
	struct arg *a;

	for (i = 0; i < ARG_COUNT; i++) {
		struct arg *a = the_args + i;

		/* zero the count and arg */
		a->count = 0;
		a->value = 0;
		a->i_value = 0;
	}

	/* fill in the short and long opts */
	for (i = 0; i < com->num_args; i++)
		add_getopt_arg(com->valid_args[i], &ptr, &o);

	*ptr = '\0';
	memset(o, 0, sizeof(*o));

	/* initialise getopt_long & scan for command line switches */
	optarg = 0;
	optind = 0;
	while ((opt = getopt_long(*argc, *argv, str, opts, NULL)) >= 0) {

		a = find_arg(com, opt);

		if (!a) {
			log_fatal("Unrecognised option.");
			return 0;
		}

		if (a->fn) {

			if (!optarg) {
				log_error("Option requires argument.");
				return 0;
			}

			a->value = optarg;

			if (!a->fn(a)) {
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

static struct arg *find_arg(struct command *com, int opt)
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
		if ((a->short_arg && (opt == a->short_arg)) || (opt == arg))
			return a;
	}

	return 0;
}

static int merge_synonym(int oldarg, int newarg)
{
	struct arg *old, *new;

	if (arg_count(cmd,oldarg) && arg_count(cmd,newarg)) {
		log_error("%s and %s are synonyms.  Please only supply one.",
			  the_args[oldarg].long_arg,
			  the_args[newarg].long_arg);
		return 0;
	}

	if (!arg_count(cmd,oldarg))
		return 1;

	old = the_args + oldarg;
	new = the_args + newarg;

	new->count = old->count;
	new->value = old->value;
	new->i_value = old->i_value;
	new->sign = old->sign;

	return 1;
}

int version(struct cmd_context *cmd, int argc, char **argv)
{
	char version[80];

	log_error("LVM version:     %s", LVM_VERSION);
	if (library_version(version, sizeof(version)))
		log_error("Library version: %s", version);
	if (driver_version(version, sizeof(version)))
		log_error("Driver version:  %s", version);

	return ECMD_PROCESSED;
}

static int process_common_commands(struct command *com)
{
	_current_settings = _default_settings;

	if (arg_count(cmd,suspend_ARG))
		kill(getpid(), SIGSTOP);

	if (arg_count(cmd,debug_ARG))
		_current_settings.debug = _LOG_FATAL +
					  (arg_count(cmd,debug_ARG) - 1);

	if (arg_count(cmd,verbose_ARG))
		_current_settings.verbose = arg_count(cmd,verbose_ARG);

	if (arg_count(cmd,quiet_ARG)) {
		_current_settings.debug = 0;
		_current_settings.verbose = 0;
	}

	if (arg_count(cmd,test_ARG))
		_current_settings.test = arg_count(cmd,test_ARG);

	if (arg_count(cmd,help_ARG)) {
		usage(com->name);
		return ECMD_PROCESSED;
	}

	if (arg_count(cmd,version_ARG)) {
		return version(cmd, 0, (char **)NULL);
	}

	if (arg_count(cmd,autobackup_ARG)) {
		_current_settings.archive = 1;
		_current_settings.backup = 1;
	}

	if (arg_count(cmd,partial_ARG)) {
		init_partial(1);
		log_print("Partial mode. Incomplete volume groups will "
			  "be activated read-only.");
	}
	else
		init_partial(0);

	/* Handle synonyms */
	if (!merge_synonym(resizable_ARG, resizeable_ARG) ||
	    !merge_synonym(allocation_ARG, allocatable_ARG) ||
	    !merge_synonym(allocation_ARG, resizeable_ARG))
		return ECMD_FAILED;

	/* Zero indicates it's OK to continue processing this command */
	return 0;
}

int help(struct cmd_context *cmd, int argc, char **argv)
{
	if (!argc)
		display_help();
	else {
		int i;
		for (i = 0; i < argc; i++)
			usage(argv[i]);
	}

	return 0;
}

static void display_help(void)
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

static void _use_settings(struct config_info *settings)
{
	init_debug(settings->debug);
	init_verbose(settings->verbose);
	init_test(settings->test);

	archive_enable(settings->archive);
	backup_enable(settings->backup);
}

static char *_copy_command_line(struct pool *mem, int argc, char **argv)
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
			if (!pool_grow_object(cmd->mem, " ", 1));
	}

	/*
	 * Terminate.
	 */
	if (!pool_grow_object(mem, "\0", 1))
              goto bad;

	return pool_end_object(mem);

 bad:
	log_err("Couldn't copy command line.");
	pool_abandon_object(mem);
	return NULL;
}

static int run_command(int argc, char **argv)
{
	int ret = 0;
	int locking_type;

	if (!(cmd->cmd_line = _copy_command_line(cmd->mem, argc, argv)))
		return ECMD_FAILED;

	if (!(cmd->command = find_command(argv[0])))
		return ENO_SUCH_CMD;

	if (!process_command_line(cmd->command, &argc, &argv)) {
		log_error("Error during parsing of command line.");
		return EINVALID_CMD_LINE;
	}

	set_cmd_name(cmd->command->name);

	if ((ret = process_common_commands(cmd->command)))
		return ret;

	_use_settings(&_current_settings);

	locking_type = find_config_int(cmd->cf->root, "global/locking_type", 
				       '/', 1);
	if (!init_locking(locking_type, cmd->cf)) {
		log_error("Locking type %d initialisation failed.",
			  locking_type);
		return 0;
	}

	ret = cmd->command->fn(cmd, argc, argv);

	fin_locking();

	/*
	 * set the debug and verbose levels back
	 * to the global default.  We have to do
	 * this so the logging levels get set
	 * correctly for program exit.
	 */
	_use_settings(&_default_settings);

	/*
	 * free off any memory the command used.
	 */
	pool_empty(cmd->mem);

	if (ret == EINVALID_CMD_LINE && !_interactive)
		usage(cmd->command->name);


	return ret;
}

static int split(char *str, int *argc, char **argv, int max)
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

static void __init_log(struct config_file *cf)
{
	char *open_mode = "a";

	const char *log_file, *prefix;


	_default_settings.syslog =
		find_config_int(cf->root, "log/syslog", '/', 1);
	if (_default_settings.syslog != 1)
		fin_syslog();

	if (_default_settings.syslog > 1)
		init_syslog(_default_settings.syslog);

	_default_settings.debug =
		find_config_int(cf->root, "log/level", '/', 0);
	init_debug(_default_settings.debug);

	_default_settings.verbose =
		find_config_int(cf->root, "log/verbose", '/', 0);
	init_verbose(_default_settings.verbose);

	init_indent(find_config_int(cf->root, "log/indent", '/', 1));
	if ((prefix = find_config_str(cf->root, "log/prefix", '/', 0)))
		init_msg_prefix(prefix);

	init_cmd_name(find_config_int(cf->root, "log/command_names", '/', 0));

	_default_settings.test = find_config_int(cf->root, "global/test",
						 '/', 0);
	if (find_config_int(cf->root, "log/overwrite", '/', 0))
		open_mode = "w";

	log_file = find_config_str(cf->root, "log/file", '/', 0);
	if (log_file) {
		/* set up the logging */
		if (!(_log = fopen(log_file, open_mode)))
			log_error("Couldn't open log file %s", log_file);
		else
			init_log(_log);
	}

}

static int _init_backup(struct config_file *cf)
{
	int days, min;
	char default_dir[PATH_MAX];
	const char *dir;

	if (!_sys_dir) {
		log_warn("WARNING: Metadata changes will NOT be backed up");
		backup_init("");
		archive_init("", 0, 0);
		return 1;
	}

	/* set up archiving */
	_default_settings.archive =
		find_config_bool(cmd->cf->root, "backup/archive", '/',
				 DEFAULT_ARCHIVE_ENABLED);

	days = find_config_int(cmd->cf->root, "backup/retain_days", '/',
			       DEFAULT_ARCHIVE_DAYS);

	min = find_config_int(cmd->cf->root, "backup/retain_min", '/',
				 DEFAULT_ARCHIVE_NUMBER);

	if (lvm_snprintf(default_dir, sizeof(default_dir), "%s/%s", _sys_dir,
			 DEFAULT_ARCHIVE_SUBDIR) == -1) {
		log_err("Couldn't create default archive path '%s/%s'.",
			_sys_dir, DEFAULT_ARCHIVE_SUBDIR);
		return 0;
	}

	dir = find_config_str(cmd->cf->root, "backup/archive_dir", '/',
			      default_dir);

	if (!archive_init(dir, days, min)) {
		log_debug("backup_init failed.");
		return 0;
	}

	/* set up the backup */
	_default_settings.backup =
		find_config_bool(cmd->cf->root, "backup/backup", '/',
				 DEFAULT_BACKUP_ENABLED);

	if (lvm_snprintf(default_dir, sizeof(default_dir), "%s/%s", _sys_dir,
			 DEFAULT_BACKUP_SUBDIR) == -1) {
		log_err("Couldn't create default backup path '%s/%s'.",
			_sys_dir, DEFAULT_BACKUP_SUBDIR);
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

static int dev_cache_setup(struct config_file *cf)
{
	struct config_node *cn;
	struct config_value *cv;

	if (!dev_cache_init()) {
		stack;
		return 0;
	}

	if (!(cn = find_config_node(cf->root, "devices/scan", '/'))) {
		if (!dev_cache_add_dir("/dev")) {
			log_error("Failed to add /dev to internal "
				  "device cache");
			return 0;
		}
		log_verbose
		    ("device/scan not in config file: Defaulting to /dev");
		return 1;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != CFG_STRING) {
			log_error("Invalid string in config file: "
				  "devices/scan");
			return 0;
		}

		if (!dev_cache_add_dir(cv->v.str)) {
			log_error("Failed to add %s to internal device cache",
				  cv->v.str);
			return 0;
		}
	}

	return 1;
}

static struct dev_filter *filter_components_setup(struct config_file *cf)
{
	struct config_node *cn;
	struct dev_filter *f1, *f2, *f3;

	if (!(f2 = lvm_type_filter_create(_proc_dir)))
		return 0;

	if (!(cn = find_config_node(cf->root, "devices/filter", '/'))) {
		log_debug("devices/filter not found in config file: no regex "
			  "filter installed");
		return f2;
	}

	if (!(f1 = regex_filter_create(cn->v))) {
		log_error("Failed to create regex device filter");
		return f2;
	}

	if (!(f3 = composite_filter_create(2, f1, f2))) {
		log_error("Failed to create composite device filter");
		return f2;
	}

	return f3;
}

static struct dev_filter *filter_setup(struct config_file *cf)
{
	const char *lvm_cache;
	struct dev_filter *f3, *f4;
	struct stat st;
	char cache_file[PATH_MAX];

	_dump_filter = 0;

	if (!(f3 = filter_components_setup(cmd->cf)))
		return 0;

	if (lvm_snprintf(cache_file, sizeof(cache_file),
			 "%s/.cache", _sys_dir) < 0) {
		log_error("Persistent cache filename too long ('%s/.cache').",
			  _sys_dir);
		return 0;
	}

	lvm_cache = find_config_str(cf->root, "devices/cache", '/',
				    cache_file);

	if (!(f4 = persistent_filter_create(f3, lvm_cache))) {
		log_error("Failed to create persistent device filter");
		return 0;
	}

	/* Should we ever dump persistent filter state? */
	if (find_config_int(cf->root, "devices/write_cache_state", '/', 1))
		_dump_filter = 1;

	if (!*_sys_dir)
		_dump_filter = 0;

	if (!stat(lvm_cache, &st) && !persistent_filter_load(f4))
		log_verbose("Failed to load existing device cache from %s",
			    lvm_cache);

	return f4;
}

static struct uuid_map *_init_uuid_map(struct dev_filter *filter)
{
	label_init();

	/* add in the lvm1 labeller */
	if (!(_lvm1_label = lvm1_labeller_create())) {
		log_err("Couldn't create lvm1 label handler.");
		return 0;
	}

	if (!(label_register_handler("lvm1", _lvm1_label))) {
		log_err("Couldn't register lvm1 label handler.");
		return 0;
	}

	return uuid_map_create(filter);
}

static void _exit_uuid_map(void)
{
	uuid_map_destroy(cmd->um);
	label_exit();
	_lvm1_label->ops->destroy(_lvm1_label);
	_lvm1_label = NULL;
}

static int _get_env_vars(void)
{
	const char *e;

	/* Set to "" to avoid using any system directory */
	if ((e = getenv("LVM_SYSTEM_DIR"))) {
		if (lvm_snprintf(_sys_dir, sizeof(_sys_dir), "%s", e) < 0) {
			log_error("LVM_SYSTEM_DIR environment variable "
				  "is too long.");
			return 0;
		}
	}

	return 1;
}

static int init(void)
{
	struct stat info;
	char config_file[PATH_MAX] = "";
	mode_t old_umask;

	if (!_get_env_vars())
		return 0;

	/* Create system directory if it doesn't already exist */
	if (!create_dir(_sys_dir))
		return 0;

	if (!(cmd = dbg_malloc(sizeof(*cmd)))) {
		log_error("Failed to allocate command context");
		return 0;
	}

	cmd->args = &the_args[0];

	if (!(cmd->cf = create_config_file())) {
		stack;
		return 0;
	}

	/* Use LOG_USER for syslog messages by default */
	init_syslog(LOG_USER);

	_init_rand();

	if (*_sys_dir && lvm_snprintf(config_file, sizeof(config_file),
				      "%s/lvm.conf", _sys_dir) < 0) {
		log_error("lvm_sys_dir was too long");
		return 0;
	}

	if (stat(config_file, &info) != -1) {
		/* we've found a config file */
		if (!read_config(cmd->cf, config_file)) {
			log_error("Failed to load config file %s",
				  config_file);
			return 0;
		}

		__init_log(cmd->cf);
	}

	_default_settings.umask = find_config_int(cmd->cf->root,
						 "global/umask", '/',
						 DEFAULT_UMASK);

	if ((old_umask = umask((mode_t)_default_settings.umask)) !=
	    (mode_t)_default_settings.umask)
		log_verbose("Set umask to %04o", _default_settings.umask);

	if (lvm_snprintf(_dev_dir, sizeof(_dev_dir), "%s/",
        		 find_config_str(cmd->cf->root, "devices/dir",
					 '/', DEFAULT_DEV_DIR)) < 0) {
		log_error("Device directory given in config file too long");
		return 0;
	}

	cmd->dev_dir = _dev_dir;
	dm_set_dev_dir(cmd->dev_dir);

	dm_log_init(print_log);

	if (lvm_snprintf(_proc_dir, sizeof(_proc_dir), "%s",
        		 find_config_str(cmd->cf->root, "global/proc",
					 '/', DEFAULT_PROC_DIR)) < 0) {
		log_error("Device directory given in config file too long");
		return 0;
	}

	if (!_init_backup(cmd->cf))
		return 0;

	if (!dev_cache_setup(cmd->cf))
		return 0;

	if (!(cmd->filter = filter_setup(cmd->cf))) {
		log_error("Failed to set up internal device filters");
		return 0;
	}

	/* the uuid map uses the filter */
	if (!(cmd->um = _init_uuid_map(cmd->filter))) {
		log_err("Failed to set up the uuid map.");
		return 0;
	}

	if (!(cmd->mem = pool_create(4 * 1024))) {
		log_error("Command pool creation failed");
		return 0;
	}

	if (!(cmd->fid = create_lvm1_format(cmd)))
		return 0;

	_use_settings(&_default_settings);
	return 1;
}

static void __fin_commands(void)
{
	int i;

	for (i = 0; i < _num_commands; i++)
		dbg_free(_commands[i].valid_args);

	dbg_free(_commands);
}

static void fin(void)
{
	if (_dump_filter)
		persistent_filter_dump(cmd->filter);

	cmd->fid->ops->destroy(cmd->fid);
	cmd->filter->destroy(cmd->filter);
	pool_destroy(cmd->mem);
	vgcache_destroy();
	dev_cache_exit();
	destroy_config_file(cmd->cf);
	archive_exit();
	backup_exit();
	_exit_uuid_map();
	dbg_free(cmd);
	__fin_commands();

	dump_memory();
	fin_log();
	fin_syslog();

	if (_log)
		fclose(_log);
}

static int run_script(int argc, char **argv)
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
		if (split(buffer, &argc, argv, MAX_ARGS) == MAX_ARGS) {
			buffer[50] = '\0';
			log_error("Too many arguments: %s", buffer);
			ret = EINVALID_CMD_LINE;
			break;
		}
		if (!argc)
			continue;
		if (!strcmp(argv[0], "quit") || !strcmp(argv[0], "exit"))
			break;
		run_command(argc, argv);
	}

	fclose(script);
	return ret;
}

#ifdef READLINE_SUPPORT
/* List matching commands */
static char *_list_cmds(const char *text, int state)
{
	static int i = 0;
	static int len = 0;

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
	static int len = 0;
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
			char *p;
			char *q = s;

			p = (char *) _commands[j].name;
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
		char *l;
		l = (the_args +
		     com->valid_args[match_no++ - com->num_args])->long_arg;
		if (!strncmp(text, l, len))
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


static void _read_history(void)
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

static int shell(void)
{
	int argc, ret;
	char *input = NULL, *args[MAX_ARGS], **argv;

	rl_readline_name = "lvm";
	rl_attempted_completion_function = (CPPFunction *) _completion;

	_read_history();

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

		if (split(input, &argc, argv, MAX_ARGS) == MAX_ARGS) {
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

		ret = run_command(argc, argv);
		if (ret == ENO_SUCH_CMD)
			log_error("No such command '%s'.  Try 'help'.",
				  argv[0]);
	}

	_write_history();
	free(input);
	return 0;
}
#endif
