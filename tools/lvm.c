/*
 * Copyright (C) 2001  Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "tools.h"

#include <assert.h>
#include <getopt.h>
#include <signal.h>
#include <syslog.h>
#include <libgen.h>
#include <sys/stat.h>

#include "stub.h"

#ifdef READLINE_SUPPORT
#include <readline/readline.h>
#include <readline/history.h>
#endif

/* define the table of valid switches */
struct arg the_args[ARG_COUNT + 1] = {

#define xx(a, b, c, d) {b, "--" c, d, 0, NULL},
#include "args.h"
#undef xx

};


/* a register of the lvm commands */
struct command {
	const char *name;
	const char *desc;
	const char *usage;
	command_fn fn;

	int num_args;
	int *valid_args;
};

static int _array_size;
static int _num_commands;
static struct command *_commands;

static struct dev_filter *_filter;
static struct io_space *_ios;
static struct config_file *_cf;

static int _interactive;
static FILE *_log;
static int _debug_level;

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
static char **lvm_completion(char *text, int start_pos, int end_pos);
static char *list_cmds(char *text, int state);
static char *list_args(char *text, int state);
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
			ret = LVM_EINVALID_CMD_LINE;
			goto out;
		}

		argc--;
		argv++;
	}

	ret = run_command(argc, argv);
	if ((ret == LVM_ENO_SUCH_CMD) && (!alias))
		ret = run_script(argc, argv);
	if (ret == LVM_ENO_SUCH_CMD)
		log_error("No such command");

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
	if (!strcmp(a->value, "y"))
		a->i_value = 1;

	else if (!strcmp(a->value, "n"))
		a->i_value = 0;

	else
		return 0;

	return 1;
}

int size_arg(struct arg *a)
{
	static char *suffixes = "kmgt";

	char *ptr;
	int i;
	long v = strtol(a->value, &ptr, 10);

	if (ptr == a->value)
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

	a->i_value = (int) v;
	return 1;

}

int int_arg(struct arg *a)
{
	char *ptr;
	long v = strtol(a->value, &ptr, 10);

	if (ptr == a->value || *ptr)
		return 0;

	a->i_value = (int) v;
	return 1;
}

int string_arg(struct arg *a)
{
	return 1;
}

int permission_arg(struct arg *a)
{
	if ((!strcmp(a->value, "rw")) || (!strcmp(a->value, "wr")))
		a->i_value = ACCESS_READ | ACCESS_WRITE;

	else if (!strcmp(a->value, "r"))
		a->i_value = ACCESS_READ;

	else
		return 0;

	return 1;
}

char yes_no_prompt(char *prompt, ...)
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
	while (getchar() != '\n');
	return c;
}

static void register_commands()
{
#define xx(a, b, c...) register_command(# a, a, b, ## c, \
                                        debug_ARG, help_ARG, suspend_ARG, \
                                        version_ARG, verbose_ARG, -1);
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
		exit(LVM_ENOMEM);
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
		exit(LVM_ENOMEM);
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
		(*o)->val = a->short_arg ? a->short_arg : (int) a;
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
	int i;

	for (i = 0; i < com->num_args; i++) {
		a = the_args + com->valid_args[i];

		if ((opt == a->short_arg) || (opt == (int) a))
			return a;
	}

	return 0;
}

static int process_common_commands(struct command *com)
{
	int l;

	if (arg_count(suspend_ARG))
		kill(getpid(), SIGSTOP);

	l = arg_count(debug_ARG);
	init_debug(l ? l : _debug_level);

	init_verbose(arg_count(verbose_ARG));

	init_test(arg_count(test_ARG));

	if (arg_count(help_ARG)) {
		usage(com->name);
		return LVM_ECMD_PROCESSED;
	}

	if (arg_count(version_ARG)) {
		/* FIXME: Add driver and software version */
		log_error("%s: ", com->name);
		return LVM_ECMD_PROCESSED;
	}

	/* Set autobackup if command takes this option */
	for (l = 0; l < com->num_args; l++)
		if (com->valid_args[l] == autobackup_ARG) {
			if (init_autobackup())
				return LVM_EINVALID_CMD_LINE;
			else
				break;
		}

	return 0;
}

int help(int argc, char **argv)
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

static void display_help()
{
	int i;

	log_error("Available lvm commands:");
	log_error("Use 'lvm help <command>' for more information");
	log_error("");

	for (i = 0; i < _num_commands; i++) {
		struct command *com = _commands + i;

		log_error("%-16.16s%s", com->name, com->desc);
	}
}

static int run_command(int argc, char **argv)
{
	int ret = 0;
	struct command *com;

	if (!(com = find_command(argv[0])))
		return LVM_ENO_SUCH_CMD;

	if (!process_command_line(com, &argc, &argv)) {
		log_error("Error during parsing of command line.");
		return LVM_EINVALID_CMD_LINE;
	}

	if ((ret = process_common_commands(com)))
		return ret;

	ret = com->fn(argc, argv);

	if (ret == LVM_EINVALID_CMD_LINE && !_interactive)
		usage(com->name);

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

struct config_file *active_config_file(void) {
	return _cf;
}

struct dev_filter *active_filter(void) {
	return _filter;
}

struct io_space *active_ios(void) {
	return _ios;
}

static void __init_log(struct config_file *cf)
{
	const char *log_file = find_config_str(cf->root, "log/file", '/', 0);

	if (log_file) {
		/* set up the logging */
		if (!(_log = fopen(log_file, "w")))
			log_error("couldn't open log file %s\n", log_file);
		else
			init_log(_log);
	}

	_debug_level = find_config_int(cf->root, "log/level", '/', 0);
	init_debug(_debug_level);
}

static int init(void)
{
	int ret = 0;
	const char *e = getenv("LVM_CONFIG_FILE");
	struct stat info;

	if (!(_cf=create_config_file())) {
		stack;
		goto out;
	}

	/* Use LOG_USER for syslog messages by default */
	init_syslog(LOG_USER);

	/* send log messages to stderr for now */
	init_log(stderr);

	e = e ? e : "/etc/lvm/lvm.conf";
	if (stat(e, &info) != -1) {
		/* we've found a config file */
		if (!read_config(_cf, e)) {
			stack;
			goto out;
		}

		__init_log(_cf);
	}

	if ((dev_cache_init)) {
		stack;
		goto out;
	}

	if (!(_filter = config_filter_create(_cf->root))) {
		goto out;
	}

	if (!(_ios = create_lvm_v1_format(_filter))) {
		goto out;
	}

	ret = 1;

      out:
	return ret;
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
	_ios->destroy(_ios);
	config_filter_destroy(_filter);
	dev_cache_exit();
	destroy_config_file(_cf);
	__fin_commands();
	dump_memory();
	fin_log();

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
		return LVM_ENO_SUCH_CMD;

	while (fgets(buffer, sizeof(buffer), script) != NULL) {
		if (!magic_number) {
			if (buffer[0] == '#' && buffer[1] == '!')
				magic_number = 1;
			else
				return LVM_ENO_SUCH_CMD;
		}
		if ((strlen(buffer) == sizeof(buffer) - 1)
		    && (buffer[sizeof(buffer) - 1] - 2 != '\n')) {
			buffer[50] = '\0';
			log_error("Line too long (max 255) beginning: %s",
				  buffer);
			ret = LVM_EINVALID_CMD_LINE;
			break;
		}
		if (split(buffer, &argc, argv, MAX_ARGS) == MAX_ARGS) {
			buffer[50] = '\0';
			log_error("Too many arguments: %s", buffer);
			ret = LVM_EINVALID_CMD_LINE;
			break;
		}
		if (!argc)
			continue;
		if (!strcmp(argv[0], "quit"))
			break;
		run_command(argc, argv);
	}

	fclose(script);
	return ret;
}

#ifdef READLINE_SUPPORT
/* Custom completion function */
static char **lvm_completion(char *text, int start_pos, int end_pos)
{
	char **match_list = NULL;
	int p = 0;

	while (isspace((int) *(rl_line_buffer + p)))
		p++;

	/* First word should be one of our commands */
	if (start_pos == p)
		match_list = completion_matches(text, list_cmds);
	else if (*text == '-')
		match_list = completion_matches(text, list_args);
	/* else other args */

	/* No further completion */
	rl_attempted_completion_over = 1;
	return match_list;
}

/* List matching commands */
static char *list_cmds(char *text, int state)
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
static char *list_args(char *text, int state)
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

static int shell(void)
{
	int argc;
	char *input, *argv[MAX_ARGS];

	rl_readline_name = "lvm";
	rl_attempted_completion_function = (CPPFunction *) lvm_completion;

	_interactive = 1;
	while (1) {
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

		if (split(input, &argc, argv, MAX_ARGS) == MAX_ARGS) {
			log_error("Too many arguments, sorry.");
			continue;
		}

		if (!argc)
			continue;

		if (!strcmp(argv[0], "quit"))
			break;

		run_command(argc, argv);
		free(input);
	}

	free(input);
	return 0;
}
#endif
