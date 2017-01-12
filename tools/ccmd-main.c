#include <asm/types.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <syslog.h>
#include <sched.h>
#include <dirent.h>
#include <ctype.h>
#include <getopt.h>

#include "ccmd.h"


/* input util functions (in ccmd-util.c) */
char *split_line(char *buf, int *argc, char **argv, char sep);
int val_str_to_num(char *str);
int opt_str_to_num(char *str);
int lvp_name_to_enum(char *str);
int lv_to_enum(char *name);
uint64_t lv_to_bits(char *name);

/* header output functions (in ccmd-util.c) */
void print_header_struct(void);
void print_header_count(void);
void print_ambiguous(void);

/* man page output (in ccmd-main.c) */
void print_man(char *man_command_name, int include_primary, int include_secondary);


/* create table of value names, e.g. String, and corresponding enum from vals.h */

struct val_name val_names[VAL_COUNT + 1] = {
#define val(a, b, c, d) { # a, a, b, c, d },
#include "vals.h"
#undef val
};

/* create table of option names, e.g. --foo, and corresponding enum from args.h */

struct opt_name opt_names[ARG_COUNT + 1] = {
#define arg(a, b, c, d, e, f, g) { # a, a, b, "", "--" c, d, e, f, g },
#include "args.h"
#undef arg
};

/* create table of lv property names, e.g. lv_is_foo, and corresponding enum from lv_props.h */

struct lvp_name lvp_names[LVP_COUNT + 1] = {
#define lvp(a, b, c) { # a, a, b },
#include "lv_props.h"
#undef lvp
};

/* create table of lv type names, e.g. linear and corresponding enum from lv_types.h */

struct lvt_name lvt_names[LVT_COUNT + 1] = {
#define lvt(a, b, c) { # a, a, b },
#include "lv_types.h"
#undef lvt
};

/* create table of command names, e.g. vgcreate */

struct cmd_name cmd_names[MAX_CMD_NAMES] = {
#define xx(a, b, c) { # a , b } ,
#include "commands.h"
#undef xx
};

/* array of pointers into opt_names[] that is sorted alphabetically (by long opt name) */

struct opt_name *opt_names_alpha[ARG_COUNT + 1];

/* lvm_all is for recording options that are common for all lvm commands */

struct command lvm_all;

/* saves OO_FOO lines (groups of optional options) to include in multiple defs */

static int oo_line_count;
#define MAX_OO_LINES 256

struct oo_line {
	char *name;
	char *line;
};
static struct oo_line oo_lines[MAX_OO_LINES];

#define REQUIRED 1  /* required option */
#define OPTIONAL 0  /* optional option */
#define IGNORE -1   /* ignore option */

/* control man page output */

static int include_man_secondary = 1; /* include SECONDARY forms in man output */
static int include_man_primary = 1;   /* include primary forms in man output */
static char *man_command_name = NULL; /* print man page for a single command name */


static void add_optional_opt_line(struct command *cmd, int argc, char *argv[]);

/*
 * Parse command-lines.in and record those definitions
 * in an array of struct command: cmd_array[].
 */

static const char *is_command_name(char *str)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!cmd_names[i].name)
			break;
		if (!strcmp(cmd_names[i].name, str))
			return cmd_names[i].name;
	}
	return NULL;
}

struct cmd_name *find_command_name(const char *str)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!cmd_names[i].name)
			break;
		if (!strcmp(cmd_names[i].name, str))
			return &cmd_names[i];
	}
	return NULL;
}

static int is_opt_name(char *str)
{
	if (!strncmp(str, "--", 2))
		return 1;

	if ((str[0] == '-') && (str[1] != '-')) {
		printf("Options must be specified in long form: %s\n", str);
		exit(1);
	}

	return 0;
}

/*
 * "Select" as a pos name means that the position
 * can be empty if the --select option is used.
 */

static int is_pos_name(char *str)
{
	if (!strncmp(str, "VG", 2))
		return 1;
	if (!strncmp(str, "LV", 2))
		return 1;
	if (!strncmp(str, "PV", 2))
		return 1;
	if (!strncmp(str, "Tag", 3))
		return 1;
	if (!strncmp(str, "String", 6))
		return 1;
	if (!strncmp(str, "Select", 6))
		return 1;
	return 0;
}

static int is_oo_definition(char *str)
{
	if (!strncmp(str, "OO_", 3) && strstr(str, ":"))
		return 1;
	return 0;
}

static int is_oo_line(char *str)
{
	if (!strncmp(str, "OO:", 3))
		return 1;
	return 0;
}

static int is_io_line(char *str)
{
	if (!strncmp(str, "IO:", 3))
		return 1;
	return 0;
}

static int is_op_line(char *str)
{
	if (!strncmp(str, "OP:", 3))
		return 1;
	return 0;
}

static int is_desc_line(char *str)
{
	if (!strncmp(str, "DESC:", 5))
		return 1;
	return 0;
}

static int is_flags_line(char *str)
{
	if (!strncmp(str, "FLAGS:", 6))
		return 1;
	return 0;
}

static int is_rule_line(char *str)
{
	if (!strncmp(str, "RULE:", 5))
		return 1;
	return 0;
}

static int is_id_line(char *str)
{
	if (!strncmp(str, "ID:", 3))
		return 1;
	return 0;
}

/*
 * Save a positional arg in a struct arg_def.
 * Parse str for anything that can appear in a position,
 * like VG, VG|LV, VG|LV_linear|LV_striped, etc.
 */

static void set_pos_def(struct command *cmd, char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int val_enum;
	int i;

	split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		val_enum = val_str_to_num(name);

		if (!val_enum) {
			printf("Unknown pos arg: %s\n", name);
			exit(1);
		}

		def->val_bits |= val_enum_to_bit(val_enum);

		if ((val_enum == lv_VAL) && strstr(name, "_"))
			def->lvt_bits = lv_to_bits(name);

		if (strstr(name, "_new")) {
			if (val_enum == lv_VAL)
				def->flags |= ARG_DEF_FLAG_NEW_LV;
			else if (val_enum == vg_VAL)
				def->flags |= ARG_DEF_FLAG_NEW_VG;
		}
	}
}

/*
 * Save an option arg in a struct arg_def.
 * Parse str for anything that can follow --option.
 */

static void set_opt_def(struct command *cmd, char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int val_enum;
	int i, j;

	split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		val_enum = val_str_to_num(name);

		if (!val_enum) {
			/* a literal number or string */

			if (isdigit(name[0]))
				val_enum = constnum_VAL;

			else if (isalpha(name[0]))
				val_enum = conststr_VAL;

			else {
				printf("Unknown opt arg: %s\n", name);
				exit(0);
			}
		}


		def->val_bits |= val_enum_to_bit(val_enum);

		if (val_enum == constnum_VAL)
			def->num = (uint64_t)atoi(name);

		if (val_enum == conststr_VAL)
			def->str = strdup(name);

		if (val_enum == lv_VAL) {
			if (strstr(name, "_"))
				def->lvt_bits = lv_to_bits(name);
		}

		if (strstr(name, "_new")) {
			if (val_enum == lv_VAL)
				def->flags |= ARG_DEF_FLAG_NEW_LV;
			else if (val_enum == vg_VAL)
				def->flags |= ARG_DEF_FLAG_NEW_VG;
				
		}
	}
}

/*
 * Save a set of common options so they can be included in
 * multiple command defs.
 *
 * OO_FOO: --opt1 ...
 *
 * oo->name = "OO_FOO";
 * oo->line = "--opt1 ...";
 */

static void add_oo_definition_line(const char *name, const char *line)
{
	struct oo_line *oo;
	char *colon;
	char *start;

	oo = &oo_lines[oo_line_count++];
	oo->name = strdup(name);

	if ((colon = strstr(oo->name, ":")))
		*colon = '\0';
	else {
		printf("invalid OO definition\n");
		exit(1);
	}

	start = strstr(line, ":") + 2;
	oo->line = strdup(start);
}

/* Support OO_FOO: continuing on multiple lines. */

static void append_oo_definition_line(const char *new_line)
{
	struct oo_line *oo;
	char *old_line;
	char *line;
	int len;

	oo = &oo_lines[oo_line_count-1];

	old_line = oo->line;

	/* +2 = 1 space between old and new + 1 terminating \0 */
	len = strlen(old_line) + strlen(new_line) + 2;
	line = malloc(len);
	memset(line, 0, len);

	strcat(line, old_line);
	strcat(line, " ");
	strcat(line, new_line);

	free(oo->line);
	oo->line = line;
}

/* Find a saved OO_FOO definition. */

char *get_oo_line(char *str)
{
	char *name;
	char *end;
	char str2[64];
	int i;

	strcpy(str2, str);
	if ((end = strstr(str2, ":")))
		*end = '\0';
	if ((end = strstr(str2, ",")))
		*end = '\0';

	for (i = 0; i < oo_line_count; i++) {
		name = oo_lines[i].name;
		if (!strcmp(name, str2))
			return oo_lines[i].line;
	}
	return NULL;
}

/*
 * Add optional_opt_args entries when OO_FOO appears on OO: line,
 * i.e. include common options from an OO_FOO definition.
 */

static void include_optional_opt_args(struct command *cmd, char *str)
{
	char *oo_line;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;

	if (!(oo_line = get_oo_line(str))) {
		printf("No OO line found for %s\n", str);
		exit(1);
	}

	if (!(line = strdup(oo_line)))
		exit(1); 

	split_line(line, &line_argc, line_argv, ' ');
	add_optional_opt_line(cmd, line_argc, line_argv);
	free(line);
}

/*
 * When an --option is seen, add a new opt_args entry for it.
 * This function sets the opt_args.opt value for it.
 */

static void add_opt_arg(struct command *cmd, char *str, int *takes_arg, int required)
{
	char *comma;
	int opt;

	/* opt_arg.opt set here */
	/* opt_arg.def will be set in update_prev_opt_arg() if needed */

	if ((comma = strstr(str, ",")))
		*comma = '\0';

	/*
	 * Work around nasty hack where --uuid is used for both uuid_ARG
	 * and uuidstr_ARG.  The input uses --uuidstr, where an actual
	 * command uses --uuid string.
	 */
	if (!strcmp(str, "--uuidstr")) {
		opt = uuidstr_ARG;
		goto skip;
	}

	opt = opt_str_to_num(str);
skip:
	if (required > 0)
		cmd->required_opt_args[cmd->ro_count++].opt = opt;
	else if (!required)
		cmd->optional_opt_args[cmd->oo_count++].opt = opt;
	else if (required < 0)
		cmd->ignore_opt_args[cmd->io_count++].opt = opt;
	else
		exit(1);

	*takes_arg = opt_names[opt].val_enum ? 1 : 0;
}

/*
 * After --option has been seen, this function sets opt_args.def value
 * for the value that appears after --option.
 */

static void update_prev_opt_arg(struct command *cmd, char *str, int required)
{
	struct arg_def def = { 0 };
	char *comma;

	if (str[0] == '-') {
		printf("Option %s must be followed by an arg.\n", str);
		exit(1);
	}

	/* opt_arg.def set here */
	/* opt_arg.opt was previously set in add_opt_arg() when --foo was read */

	if ((comma = strstr(str, ",")))
		*comma = '\0';

	set_opt_def(cmd, str, &def);

	if (required > 0)
		cmd->required_opt_args[cmd->ro_count-1].def = def;
	else if (!required)
		cmd->optional_opt_args[cmd->oo_count-1].def = def;
	else if (required < 0)
		cmd->ignore_opt_args[cmd->io_count-1].def = def;
	else
		exit(1);
}

/*
 * When an position arg is seen, add a new pos_args entry for it.
 * This function sets the pos_args.pos and pos_args.def.
 */

static void add_pos_arg(struct command *cmd, char *str, int required)
{
	struct arg_def def = { 0 };

	/* pos_arg.pos and pos_arg.def are set here */

	set_pos_def(cmd, str, &def);

	if (required) {
		cmd->required_pos_args[cmd->rp_count].pos = cmd->pos_count++;
		cmd->required_pos_args[cmd->rp_count].def = def;
		cmd->rp_count++;
	} else {
		cmd->optional_pos_args[cmd->op_count].pos = cmd->pos_count++;;
		cmd->optional_pos_args[cmd->op_count].def = def;
		cmd->op_count++;
	}
}

/* Process something that follows a pos arg, which is not a new pos arg. */

static void update_prev_pos_arg(struct command *cmd, char *str, int required)
{
	struct arg_def *def;

	/* a previous pos_arg.def is modified here */

	if (required)
		def = &cmd->required_pos_args[cmd->rp_count-1].def;
	else
		def = &cmd->optional_pos_args[cmd->op_count-1].def;

	if (!strcmp(str, "..."))
		def->flags |= ARG_DEF_FLAG_MAY_REPEAT;
	else {
		printf("Unknown pos arg: %s\n", str);
		exit(1);
	}
}

/* Process what follows OO:, which are the optional opt args for the cmd def. */

static void add_optional_opt_line(struct command *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OO:", 3))
			continue;
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, OPTIONAL);
		else if (!strncmp(argv[i], "OO_", 3))
			include_optional_opt_args(cmd, argv[i]);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], OPTIONAL);
		else
			printf("Can't parse argc %d argv %s prev %s\n",
				i, argv[i], argv[i-1]);
	}
}

/* Process what follows IO:, which are the ignore options for the cmd def. */

static void add_ignore_opt_line(struct command *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "IO:", 3))
			continue;
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, IGNORE);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], IGNORE);
		else
			printf("Can't parse argc %d argv %s prev %s\n",
				i, argv[i], argv[i-1]);
	}
}

/* Process what follows OP:, which are optional pos args for the cmd def. */

static void add_optional_pos_line(struct command *cmd, int argc, char *argv[])
{
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OP:", 3))
			continue;
		if (is_pos_name(argv[i]))
			add_pos_arg(cmd, argv[i], OPTIONAL);
		else
			update_prev_pos_arg(cmd, argv[i], OPTIONAL);
	}
}

static void add_required_opt_line(struct command *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, REQUIRED);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], REQUIRED);
		else
			printf("Can't parse argc %d argv %s prev %s\n",
				i, argv[i], argv[i-1]);
	}
}

/*
 * Add to required_opt_args from an OO_FOO definition.
 * (This is the special case of vgchange/lvchange where one
 * optional option is required, and others are then optional.)
 * The set of options from OO_FOO are saved in required_opt_args,
 * and flag CMD_FLAG_ONE_REQUIRED_OPT is set on the cmd indicating
 * this special case.
 */
 
static void include_required_opt_args(struct command *cmd, char *str)
{
	char *oo_line;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;

	if (!(oo_line = get_oo_line(str))) {
		printf("No OO line found for %s\n", str);
		exit(1);
	}

	if (!(line = strdup(oo_line)))
		exit(1); 

	split_line(line, &line_argc, line_argv, ' ');
	add_required_opt_line(cmd, line_argc, line_argv);
	free(line);
}

/* Process what follows command_name, which are required opt/pos args. */

static void add_required_line(struct command *cmd, int argc, char *argv[])
{
	int i;
	int takes_arg;
	int prev_was_opt = 0, prev_was_pos = 0;

	/* argv[0] is command name */

	for (i = 1; i < argc; i++) {

		if (is_opt_name(argv[i])) {
			/* add new required_opt_arg */
			add_opt_arg(cmd, argv[i], &takes_arg, REQUIRED);
			prev_was_opt = 1;
			prev_was_pos = 0;

		} else if (prev_was_opt && takes_arg) {
			/* set value for previous required_opt_arg */
			update_prev_opt_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 0;

		} else if (is_pos_name(argv[i])) {
			/* add new required_pos_arg */
			add_pos_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 1;

		} else if (!strncmp(argv[i], "OO_", 3)) {
			/* one required_opt_arg is required, special case lv/vgchange */
			cmd->cmd_flags |= CMD_FLAG_ONE_REQUIRED_OPT;
			include_required_opt_args(cmd, argv[i]);

		} else if (prev_was_pos) {
			/* set property for previous required_pos_arg */
			update_prev_pos_arg(cmd, argv[i], REQUIRED);
		} else
			printf("Can't parse argc %d argv %s prev %s\n",
				i, argv[i], argv[i-1]);

	}
}

static void add_flags(struct command *cmd, char *line)
{
	if (strstr(line, "SECONDARY_SYNTAX"))
		cmd->cmd_flags |= CMD_FLAG_SECONDARY_SYNTAX;
}

#define MAX_RULE_OPTS 64

static void add_rule(struct command *cmd, char *line)
{
	struct cmd_rule *rule;
	char *line_argv[MAX_LINE_ARGC];
	char *arg;
	int line_argc;
	int i, lvt_enum, lvp_enum;
	int check = 0;

	if (cmd->rule_count == CMD_MAX_RULES) {
		printf("too many rules for cmd\n");
		exit(1);
	}

	rule = &cmd->rules[cmd->rule_count++];

	split_line(line, &line_argc, line_argv, ' ');

	for (i = 0; i < line_argc; i++) {
		arg = line_argv[i];

		if (!strcmp(arg, "not")) {
			rule->rule = RULE_INVALID;
			check = 1;
		}

		else if (!strcmp(arg, "and")) {
			rule->rule = RULE_REQUIRE;
			check = 1;
		}

		else if (!strncmp(arg, "all", 3)) {
			/* opt/lvt_bits/lvp_bits all remain 0 to mean all */
			continue;
		}

		else if (!strncmp(arg, "--", 2)) {
			if (!rule->opts) {
				if (!(rule->opts = malloc(MAX_RULE_OPTS * sizeof(int)))) {
					printf("no mem\n");
					exit(1);
				}
				memset(rule->opts, 0, MAX_RULE_OPTS * sizeof(int));
			}

			if (!rule->check_opts) {
				if (!(rule->check_opts = malloc(MAX_RULE_OPTS * sizeof(int)))) {
					printf("no mem\n");
					exit(1);
				}
				memset(rule->check_opts, 0, MAX_RULE_OPTS * sizeof(int));
			}

			if (check)
				rule->check_opts[rule->check_opts_count++] = opt_str_to_num(arg);
			else
				rule->opts[rule->opts_count++] = opt_str_to_num(arg);
		}

		else if (!strncmp(arg, "LV_", 3)) {
			lvt_enum = lv_to_enum(arg);

			if (check)
				rule->check_lvt_bits |= lvt_enum_to_bit(lvt_enum);
			else
				rule->lvt_bits |= lvt_enum_to_bit(lvt_enum);
		}

		else if (!strncmp(arg, "lv_is_", 6)) {
			lvp_enum = lvp_name_to_enum(arg);

			if (check)
				rule->check_lvp_bits |= lvp_enum_to_bit(lvp_enum);
			else
				rule->lvp_bits |= lvp_enum_to_bit(lvp_enum);
		}
	}
}

/* The given option is common to all lvm commands (set in lvm_all). */

int is_lvm_all_opt(int opt)
{
	int oo;

	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		if (lvm_all.optional_opt_args[oo].opt == opt)
			return 1;
	}
	return 0;
}

/* Find common options for all variants of each command name. */

static void factor_common_options(void)
{
	int cn, opt_enum, ci, oo, ro, found;
	struct command *cmd;

	for (cn = 0; cn < MAX_CMD_NAMES; cn++) {
		if (!cmd_names[cn].name)
			break;

		for (ci = 0; ci < cmd_count; ci++) {
			cmd = &cmd_array[ci];

			if (strcmp(cmd->name, cmd_names[cn].name))
				continue;

			cmd_names[cn].variants++;
		}

		for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {

			for (ci = 0; ci < cmd_count; ci++) {
				cmd = &cmd_array[ci];

				if (strcmp(cmd->name, cmd_names[cn].name))
					continue;

				if (cmd->ro_count)
					cmd_names[cn].variant_has_ro = 1;
				if (cmd->rp_count)
					cmd_names[cn].variant_has_rp = 1;
				if (cmd->oo_count)
					cmd_names[cn].variant_has_oo = 1;
				if (cmd->op_count)
					cmd_names[cn].variant_has_op = 1;

				for (ro = 0; ro < cmd->ro_count; ro++) {
					cmd_names[cn].all_options[cmd->required_opt_args[ro].opt] = 1;

					if ((cmd->required_opt_args[ro].opt == size_ARG) && !strncmp(cmd->name, "lv", 2))
						cmd_names[cn].all_options[extents_ARG] = 1;
				}
				for (oo = 0; oo < cmd->oo_count; oo++)
					cmd_names[cn].all_options[cmd->optional_opt_args[oo].opt] = 1;

				found = 0;

				for (oo = 0; oo < cmd->oo_count; oo++) {
					if (cmd->optional_opt_args[oo].opt == opt_enum) {
						found = 1;
						break;
					}
				}

				if (!found)
					goto next_opt;
			}

			/* all commands starting with this name use this option */
			cmd_names[cn].common_options[opt_enum] = 1;
 next_opt:
			;
		}
	}
}

static int long_name_compare(const void *on1, const void *on2)
{
	struct opt_name **optname1 = (void *)on1;
	struct opt_name **optname2 = (void *)on2;
	return strcmp((*optname1)->long_opt + 2, (*optname2)->long_opt + 2);
}

/* Create list of option names for printing alphabetically. */

static void create_opt_names_alpha(void)
{
	int i;

	for (i = 0; i < ARG_COUNT; i++)
		opt_names_alpha[i] = &opt_names[i];

	qsort(opt_names_alpha, ARG_COUNT, sizeof(long), long_name_compare);
}

static void print_help(int argc, char *argv[])
{
	printf("%s [options] --output <format> <filename>\n", argv[0]);
	printf("\n");
	printf("output formats:\n");
	printf("struct:    print C structures for command-lines.h\n");
	printf("count:     print defines and enums for command-lines-count.h\n");
	printf("ambiguous: print commands differing only by LV types\n");
	printf("man:       print man page format.\n");
	printf("\n");
	printf("options:\n");
	printf("-c|--man-command <commandname>  man output for one command name\n");
}

int main(int argc, char *argv[])
{
	char *outputformat = NULL;
	char *inputfile = NULL;
	FILE *file;
	struct command *cmd;
	char line[MAX_LINE];
	char line_orig[MAX_LINE];
	const char *name;
	char *line_argv[MAX_LINE_ARGC];
	char *n;
	int line_argc;
	int prev_was_oo_def = 0;
	int prev_was_oo = 0;
	int prev_was_op = 0;

	if (argc < 2) {
		print_help(argc, argv);
		exit(EXIT_FAILURE);
	}

	create_opt_names_alpha();

	static struct option long_options[] = {
		{"help",      no_argument,       0, 'h' },
		{"output",    required_argument, 0, 'o' },
		{"man-primary", required_argument, 0, 'p' },
		{"man-secondary", required_argument, 0, 's' },
		{"man-command", required_argument, 0, 'c' },
		{0, 0, 0, 0 }
	};

        while (1) {
		int c;
		int option_index = 0;

		c = getopt_long(argc, argv, "ho:p:s:c:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case '0':
			break;
		case 'h':
                        print_help(argc, argv);
			exit(EXIT_SUCCESS);
		case 'o':
			outputformat = strdup(optarg);
			break;
		case 'p':
			include_man_primary = atoi(optarg);
			break;
		case 's':
			include_man_secondary = atoi(optarg);
			break;
		case 'c':
			man_command_name = strdup(optarg);
			break;
		}
	}

	if (optind < argc)
		inputfile = argv[optind];
	else {
		printf("Missing filename.\n");
		return 0;
	}

	if (!(file = fopen(inputfile, "r"))) {
		printf("Cannot open %s\n", argv[1]);
		return -1;
	}

	/* Process each line of input file. */

	while (fgets(line, MAX_LINE, file)) {
		if (line[0] == '#')
			continue;
		if (line[0] == '\n')
			continue;
		if (line[0] == '-' && line[1] == '-' && line[2] == '-')
			continue;

		if ((n = strchr(line, '\n')))
			*n = '\0';

		memcpy(line_orig, line, sizeof(line));
		split_line(line, &line_argc, line_argv, ' ');

		if (!line_argc)
			continue;

		/* command ... */
		if ((name = is_command_name(line_argv[0]))) {
			if (cmd_count >= MAX_CMDS) {
				printf("MAX_CMDS too small\n");
				return -1;
			}
			cmd = &cmd_array[cmd_count++];
			cmd->name = name;
			cmd->pos_count = 1;
			add_required_line(cmd, line_argc, line_argv);

			/* Every cmd gets the OO_ALL options */
			include_optional_opt_args(cmd, "OO_ALL:");
			continue;
		}

		if (is_desc_line(line_argv[0])) {
			char *desc = strdup(line_orig);
			if (cmd->desc) {
				int newlen = strlen(cmd->desc) + strlen(desc) + 2;
				char *newdesc = malloc(newlen);
				memset(newdesc, 0, newlen);
				snprintf(newdesc, newlen, "%s %s", cmd->desc, desc);
				cmd->desc = newdesc;
				free(desc);
			} else
				cmd->desc = desc;
			continue;
		}

		if (is_flags_line(line_argv[0])) {
			add_flags(cmd, line_orig);
			continue;
		}

		if (is_rule_line(line_argv[0])) {
			add_rule(cmd, line_orig);
			continue;
		}

		if (is_id_line(line_argv[0])) {
			cmd->command_line_id = strdup(line_argv[1]);
			continue;
		}

		/* OO_FOO: ... */
		if (is_oo_definition(line_argv[0])) {
			add_oo_definition_line(line_argv[0], line_orig);
			prev_was_oo_def = 1;
			prev_was_oo = 0;
			prev_was_op = 0;
			continue;
		}

		/* OO: ... */
		if (is_oo_line(line_argv[0])) {
			add_optional_opt_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 1;
			prev_was_op = 0;
			continue;
		}

		/* OP: ... */
		if (is_op_line(line_argv[0])) {
			add_optional_pos_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 0;
			prev_was_op = 1;
			continue;
		}

		/* IO: ... */
		if (is_io_line(line_argv[0])) {
			add_ignore_opt_line(cmd, line_argc, line_argv);
			prev_was_oo = 0;
			prev_was_op = 0;
			continue;
		}

		/* handle OO_FOO:, OO:, OP: continuing on multiple lines */

		if (prev_was_oo_def) {
			append_oo_definition_line(line_orig);
			continue;
		}

		if (prev_was_oo) {
			add_optional_opt_line(cmd, line_argc, line_argv);
			continue;
		}

		if (prev_was_op) {
			add_optional_pos_line(cmd, line_argc, line_argv);
			continue;
		}
	}

	fclose(file);

	/*
	 * Looks at all variants of each command name and figures out
	 * which options are common to all variants (for compact output)
	 */
	factor_common_options();

	/*
	 * Predefined string of options common to all commands
	 * (for compact output)
	 */
	include_optional_opt_args(&lvm_all, "OO_USAGE_COMMON");

	/*
	 * Print output.
	 */

	if (!strcmp(outputformat, "struct"))
		print_header_struct();

	else if (!strcmp(outputformat, "count"))
		print_header_count();

	else if (!strcmp(outputformat, "ambiguous"))
		print_ambiguous();

	else if (!strcmp(outputformat, "man"))
		print_man(man_command_name, include_man_primary, include_man_secondary);
	else
		print_help(argc, argv);

	return 0;
}

