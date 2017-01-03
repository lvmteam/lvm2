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

/* needed to include args.h */
#define ARG_COUNTABLE 0x00000001
#define ARG_GROUPABLE 0x00000002
struct cmd_context;
struct arg_values;

int yes_no_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int activation_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int cachemode_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int discards_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int mirrorlog_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int size_kb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int size_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int size_mb_arg_with_percent(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int int_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int uint32_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int int_arg_with_sign(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int int_arg_with_sign_and_percent(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int major_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int minor_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int string_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int tag_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int permission_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int metadatatype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int units_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int segtype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int alloc_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int locktype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int readahead_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int vgmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
int pvmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
int metadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
int polloperation_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
int writemostly_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
int syncaction_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
int reportformat_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
int configreport_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
int configtype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }

/* also see arg_props in tools.h and args.h */
struct opt_name {
	const char *name;       /* "foo_ARG" */
	int opt_enum;           /* foo_ARG */
	const char short_opt;   /* -f */
	char _padding[7];
	const char *long_opt;   /* --foo */
	int val_enum;           /* xyz_VAL when --foo takes a val like "--foo xyz" */
	uint32_t unused1;
	uint32_t unused2;
};

/* also see val_props in tools.h and vals.h */
struct val_name {
	const char *enum_name;  /* "foo_VAL" */
	int val_enum;           /* foo_VAL */
	int (*fn) (struct cmd_context *cmd, struct arg_values *av); /* foo_arg() */
	const char *name;       /* FooVal */
	const char *usage;
};

/* also see lv_props in tools.h and lv_props.h */
struct lvp_name {
	const char *enum_name; /* "is_foo_LVP" */
	int lvp_enum;          /* is_foo_LVP */
	const char *name;      /* "lv_is_foo" */
};

/* also see lv_types in tools.h and lv_types.h */
struct lvt_name {
	const char *enum_name; /* "foo_LVT" */
	int lvt_enum;          /* foo_LVT */
	const char *name;      /* "foo" */
};

/* create foo_VAL enums for option and position values */

enum {
#define val(a, b, c, d) a ,
#include "vals.h"
#undef val
};

/* create foo_ARG enums for --option's */

enum {
#define arg(a, b, c, d, e, f) a ,
#include "args.h"
#undef arg
};

/* create foo_LVP enums for LV properties */

enum {
#define lvp(a, b, c) a,
#include "lv_props.h"
#undef lvp
};

/* create foo_LVT enums for LV types */

enum {
#define lvt(a, b, c) a,
#include "lv_types.h"
#undef lvt
};

/* create table of value names, e.g. String, and corresponding enum from vals.h */

static struct val_name val_names[VAL_COUNT + 1] = {
#define val(a, b, c, d) { # a, a, b, c, d },
#include "vals.h"
#undef val
};

/* create table of option names, e.g. --foo, and corresponding enum from args.h */

static struct opt_name opt_names[ARG_COUNT + 1] = {
#define arg(a, b, c, d, e, f) { # a, a, b, "", "--" c, d, e, f },
#include "args.h"
#undef arg
};

/* create table of lv property names, e.g. lv_is_foo, and corresponding enum from lv_props.h */

static struct lvp_name lvp_names[LVP_COUNT + 1] = {
#define lvp(a, b, c) { # a, a, b },
#include "lv_props.h"
#undef lvp
};

/* create table of lv type names, e.g. linear and corresponding enum from lv_types.h */

static struct lvt_name lvt_names[LVT_COUNT + 1] = {
#define lvt(a, b, c) { # a, a, b },
#include "lv_types.h"
#undef lvt
};

#include "command.h"

#define MAX_CMD_NAMES 128
struct cmd_name {
	const char *name;
	const char *desc;
	int common_options[ARG_COUNT + 1];
	int all_options[ARG_COUNT + 1];
	int variants;
	int variant_has_ro;
	int variant_has_rp;
	int variant_has_oo;
	int variant_has_op;
};

/* create table of command names, e.g. vgcreate */

static struct cmd_name cmd_names[MAX_CMD_NAMES] = {
#define xx(a, b, c) { # a , b } ,
#include "commands.h"
#undef xx
};

#define MAX_LINE 1024
#define MAX_LINE_ARGC 256

#define REQUIRED 1  /* required option */
#define OPTIONAL 0  /* optional option */
#define IGNORE -1   /* ignore option */

struct oo_line {
	char *name;
	char *line;
};

#define MAX_CMDS 256
int cmd_count;
struct command cmd_array[MAX_CMDS];

struct command lvm_all; /* for printing common options for all lvm commands */

#define MAX_OO_LINES 256
int oo_line_count;
struct oo_line oo_lines[MAX_OO_LINES];

static int include_man_secondary = 1;
static int include_man_primary = 1;
static char *man_command_name = NULL;

static void add_optional_opt_line(struct command *cmd, int argc, char *argv[]);

/*
 * modifies buf, replacing the sep characters with \0
 * argv pointers point to positions in buf
 */

static char *split_line(char *buf, int *argc, char **argv, char sep)
{
	char *p = buf, *rp = NULL;
	int i;

	argv[0] = p;

	for (i = 1; i < MAX_LINE_ARGC; i++) {
		p = strchr(buf, sep);
		if (!p)
			break;
		*p = '\0';

		argv[i] = p + 1;
		buf = p + 1;
	}
	*argc = i;

	/* we ended by hitting \0, return the point following that */
	if (!rp)
		rp = strchr(buf, '\0') + 1;

	return rp;
}

/* convert value string, e.g. Number, to foo_VAL enum */

static int val_str_to_num(char *str)
{
	char name[32] = { 0 };
	char *new;
	int i;

	/* compare the name before any suffix like _new or _<lvtype> */

	strncpy(name, str, 31);
	if ((new = strstr(name, "_")))
		*new = '\0';

	for (i = 0; i < VAL_COUNT; i++) {
		if (!val_names[i].name)
			break;
		if (!strncmp(name, val_names[i].name, strlen(val_names[i].name)))
			return val_names[i].val_enum;
	}

	return 0;
}

/* convert "--option" to foo_ARG enum */

static int opt_str_to_num(char *str)
{
	char long_name[32];
	char *p;
	int i;

	/*
	 * --foo_long means there are two args entries
	 * for --foo, one with a short option and one
	 * without, and we want the one without the
	 * short option.
	 */
	if (strstr(str, "_long")) {
		strcpy(long_name, str);
		p = strstr(long_name, "_long");
		*p = '\0';

		for (i = 0; i < ARG_COUNT; i++) {
			if (!opt_names[i].long_opt)
				continue;
			/* skip anything with a short opt */
			if (opt_names[i].short_opt)
				continue;
			if (!strcmp(opt_names[i].long_opt, long_name))
				return opt_names[i].opt_enum;
		}

		printf("Unknown opt str: %s %s\n", str, long_name);
		exit(1);
	}

	for (i = 0; i < ARG_COUNT; i++) {
		if (!opt_names[i].long_opt)
			continue;
		/* These are only selected using --foo_long */
		if (strstr(opt_names[i].name, "_long_ARG"))
			continue;
		if (!strcmp(opt_names[i].long_opt, str))
			return opt_names[i].opt_enum;
	}

	printf("Unknown opt str: \"%s\"\n", str);
	exit(1);
}

static char *val_bits_to_str(uint64_t val_bits)
{
	static char buf[1024];
	int i;
	int or = 0;

	memset(buf, 0, sizeof(buf));

	for (i = 0; i < VAL_COUNT; i++) {
		if (val_bits & val_enum_to_bit(i)) {
			if (or) strcat(buf, " | ");
			strcat(buf, "val_enum_to_bit(");
			strcat(buf, val_names[i].enum_name);
			strcat(buf, ")");
			or = 1;
		}
	}

	return buf;
}

/*
 * When bits for foo_LVP and bar_LVP are both set in bits, print:
 * lvp_enum_to_bit(foo_LVP) | lvp_enum_to_bit(bar_LVP)
 */

static char *lvp_bits_to_str(uint64_t bits)
{
	static char lvp_buf[1024];
	int i;
	int or = 0;

	memset(lvp_buf, 0, sizeof(lvp_buf));

	for (i = 0; i < LVP_COUNT; i++) {
		if (bits & lvp_enum_to_bit(i)) {
			if (or) strcat(lvp_buf, " | ");
			strcat(lvp_buf, "lvp_enum_to_bit(");
			strcat(lvp_buf, lvp_names[i].enum_name);
			strcat(lvp_buf, ")");
			or = 1;
		}
	}

	return lvp_buf;
}

/*
 * When bits for foo_LVT and bar_LVT are both set in bits, print:
 * lvt_enum_to_bit(foo_LVT) | lvt_enum_to_bit(bar_LVT)
 */

static char *lvt_bits_to_str(uint64_t bits)
{
	static char lvt_buf[1024];
	int i;
	int or = 0;

	memset(lvt_buf, 0, sizeof(lvt_buf));

	for (i = 1; i < LVT_COUNT; i++) {
		if (bits & lvt_enum_to_bit(i)) {
			if (or) strcat(lvt_buf, " | ");
			strcat(lvt_buf, "lvt_enum_to_bit(");
			strcat(lvt_buf, lvt_names[i].enum_name);
			strcat(lvt_buf, ")");
			or = 1;
		}
	}

	return lvt_buf;
}

/* "lv_is_prop" to is_prop_LVP */

static int lvp_name_to_enum(char *str)
{
	int i;

	for (i = 1; i < LVP_COUNT; i++) {
		if (!strcmp(str, lvp_names[i].name))
			return lvp_names[i].lvp_enum;
	}
	printf("unknown lv property %s\n", str);
	exit(1);
}

/* type_LVT to "type" */

static const char *lvt_enum_to_name(int lvt_enum)
{
	return lvt_names[lvt_enum].name;
}

/* "type" to type_LVT */

static int lvt_name_to_enum(char *str)
{
	int i;

	for (i = 1; i < LVT_COUNT; i++) {
		if (!strcmp(str, lvt_names[i].name))
			return lvt_names[i].lvt_enum;
	}
	printf("unknown lv type %s\n", str);
	exit(1);
}

/* LV_<type> to <type>_LVT */

int lv_to_enum(char *name)
{
	return lvt_name_to_enum(name + 3);
}

/*
 * LV_<type1>_<type2> to lvt_bits
 *
 * type1 to lvt_enum
 * lvt_bits |= lvt_enum_to_bit(lvt_enum)
 * type2 to lvt_enum
 * lvt_bits |= lvt_enum_to_bit(lvt_enum)
 */

uint64_t lv_to_bits(char *name)
{
	char buf[64];
	char *argv[MAX_LINE_ARGC];
	uint64_t lvt_bits = 0;
	int lvt_enum;
	int argc;
	int i;

	strcpy(buf, name);

	split_line(buf, &argc, argv, '_');

	/* 0 is "LV" */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "new"))
			continue;
		lvt_enum = lvt_name_to_enum(argv[i]);
		lvt_bits |= lvt_enum_to_bit(lvt_enum);
	}

	return lvt_bits;
}

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

static const char *cmd_name_desc(const char *name)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!cmd_names[i].name)
			break;
		if (!strcmp(cmd_names[i].name, name))
			return cmd_names[i].desc;
	}
	return NULL;
}

static struct cmd_name *find_command_name(const char *str)
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
 * parse str for anything that can appear in a position,
 * like VG, VG|LV, VG|LV_linear|LV_striped, etc
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
 * parse str for anything that can follow --option
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

/* when OO_FOO: continues on multiple lines */

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

/* add optional_opt_args entries when OO_FOO appears on OO: line */

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

/* process something that follows a pos arg, which is not a new pos arg */

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

/* process what follows OO:, which are optional opt args */

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

/* process what follows OP:, which are optional pos args */

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

/* add required opt args from OO_FOO definition */

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

/* add to required_opt_args when OO_FOO appears on required line */
 
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

/* process what follows command_name, which are required opt/pos args */

static void add_required_line(struct command *cmd, int argc, char *argv[])
{
	int i;
	int takes_arg;
	int prev_was_opt = 0, prev_was_pos = 0;

	/* argv[0] is command name */

	for (i = 1; i < argc; i++) {
		if (is_opt_name(argv[i])) {
			add_opt_arg(cmd, argv[i], &takes_arg, REQUIRED);
			prev_was_opt = 1;
			prev_was_pos = 0;
		} else if (prev_was_opt && takes_arg) {
			update_prev_opt_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 0;
		} else if (is_pos_name(argv[i])) {
			add_pos_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 1;
		} else if (!strncmp(argv[i], "OO_", 3)) {
			cmd->cmd_flags |= CMD_FLAG_ONE_REQUIRED_OPT;
			include_required_opt_args(cmd, argv[i]);
		} else if (prev_was_pos) {
			update_prev_pos_arg(cmd, argv[i], REQUIRED);
		} else
			printf("Can't parse argc %d argv %s prev %s\n",
				i, argv[i], argv[i-1]);

	}
}

static void print_def(struct arg_def *def, int usage)
{
	int val_enum;
	int lvt_enum;
	int sep = 0;
	int i;

	for (val_enum = 0; val_enum < VAL_COUNT; val_enum++) {
		if (def->val_bits & val_enum_to_bit(val_enum)) {

			if (val_enum == conststr_VAL)
				printf("%s", def->str);

			else if (val_enum == constnum_VAL)
				printf("%llu", (unsigned long long)def->num);

			else {
				if (sep) printf("|");

				if (!usage || !val_names[val_enum].usage)
					printf("%s", val_names[val_enum].name);
				else
					printf("%s", val_names[val_enum].usage);

				sep = 1;
			}

			if (val_enum == lv_VAL && def->lvt_bits) {
				for (lvt_enum = 1; lvt_enum < LVT_COUNT; lvt_enum++) {
					if (lvt_bit_is_set(def->lvt_bits, lvt_enum))
						printf("_%s", lvt_enum_to_name(lvt_enum));
				}
			}

			if ((val_enum == vg_VAL) && (def->flags & ARG_DEF_FLAG_NEW_VG))
				printf("_new");
			if ((val_enum == lv_VAL) && (def->flags & ARG_DEF_FLAG_NEW_LV))
				printf("_new");
		}
	}

	if (def->flags & ARG_DEF_FLAG_MAY_REPEAT)
		printf(" ...");
}

void print_expanded(void)
{
	struct command *cmd;
	int onereq;
	int i, ro, rp, oo, op;

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];
		printf("%s", cmd->name);

		onereq = (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT) ? 1 : 0;

		if (cmd->ro_count) {
			if (onereq)
				printf(" (");

			for (ro = 0; ro < cmd->ro_count; ro++) {
				if (ro && onereq)
					printf(",");
				printf(" %s", opt_names[cmd->required_opt_args[ro].opt].long_opt);
				if (cmd->required_opt_args[ro].def.val_bits) {
					printf(" ");
					print_def(&cmd->required_opt_args[ro].def, 0);
				}
			}
			if (onereq)
				printf(" )");
		}

		if (cmd->rp_count) {
			for (rp = 0; rp < cmd->rp_count; rp++) {
				if (cmd->required_pos_args[rp].def.val_bits) {
					printf(" ");
					print_def(&cmd->required_pos_args[rp].def, 0);
				}
			}
		}

		if (cmd->oo_count) {
			printf("\n");
			printf("OO:");
			for (oo = 0; oo < cmd->oo_count; oo++) {
				if (oo)
					printf(",");
				printf(" %s", opt_names[cmd->optional_opt_args[oo].opt].long_opt);
				if (cmd->optional_opt_args[oo].def.val_bits) {
					printf(" ");
					print_def(&cmd->optional_opt_args[oo].def, 0);
				}
			}
		}

		if (cmd->op_count) {
			printf("\n");
			printf("OP:");
			for (op = 0; op < cmd->op_count; op++) {
				if (cmd->optional_pos_args[op].def.val_bits) {
					printf(" ");
					print_def(&cmd->optional_pos_args[op].def, 0);
				}
			}
		}

		printf("\n\n");
	}
}

static int opt_arg_matches(struct opt_arg *oa1, struct opt_arg *oa2)
{
	if (oa1->opt != oa2->opt)
		return 0;

	/* FIXME: some cases may need more specific val_bits checks */
	if (oa1->def.val_bits != oa2->def.val_bits)
		return 0;

	if (oa1->def.str && oa2->def.str && strcmp(oa1->def.str, oa2->def.str))
		return 0;

	if (oa1->def.num != oa2->def.num)
		return 0;

	/*
	 * Do NOT compare lv_types because we are checking if two
	 * command lines are ambiguous before the LV type is known.
	 */

	return 1;
}

static int pos_arg_matches(struct pos_arg *pa1, struct pos_arg *pa2)
{
	if (pa1->pos != pa2->pos)
		return 0;

	/* FIXME: some cases may need more specific val_bits checks */
	if (pa1->def.val_bits != pa2->def.val_bits)
		return 0;

	if (pa1->def.str && pa2->def.str && strcmp(pa1->def.str, pa2->def.str))
		return 0;

	if (pa1->def.num != pa2->def.num)
		return 0;

	/*
	 * Do NOT compare lv_types because we are checking if two
	 * command lines are ambiguous before the LV type is known.
	 */

	return 1;
}

static const char *opt_to_enum_str(int opt)
{
	return opt_names[opt].name;
}

static char *flags_to_str(int flags)
{
	static char buf_flags[32];

	memset(buf_flags, 0, sizeof(buf_flags));

	if (flags & ARG_DEF_FLAG_MAY_REPEAT) {
		if (buf_flags[0])
			strcat(buf_flags, " | ");
		strcat(buf_flags, "ARG_DEF_FLAG_MAY_REPEAT");
	}
	if (flags & ARG_DEF_FLAG_NEW_VG) {
		if (buf_flags[0])
			strcat(buf_flags, " | ");
		strcat(buf_flags, "ARG_DEF_FLAG_NEW_VG");
	}
	if (flags & ARG_DEF_FLAG_NEW_LV) {
		if (buf_flags[0])
			strcat(buf_flags, " | ");
		strcat(buf_flags, "ARG_DEF_FLAG_NEW_LV");
	}

	return buf_flags;
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

static char *rule_to_define_str(int rule_type)
{
	switch (rule_type) {
	case RULE_INVALID:
		return "RULE_INVALID";
	case RULE_REQUIRE:
		return "RULE_REQUIRE";
	}
}

static char *cmd_flags_to_str(uint32_t flags)
{
	static char buf_cmd_flags[32];

	memset(buf_cmd_flags, 0, sizeof(buf_cmd_flags));

	if (flags & CMD_FLAG_SECONDARY_SYNTAX) {
		if (buf_cmd_flags[0])
			strcat(buf_cmd_flags, " | ");
		strcat(buf_cmd_flags, "CMD_FLAG_SECONDARY_SYNTAX");
	}
	if (flags & CMD_FLAG_ONE_REQUIRED_OPT) {
		if (buf_cmd_flags[0])
			strcat(buf_cmd_flags, " | ");
		strcat(buf_cmd_flags, "CMD_FLAG_ONE_REQUIRED_OPT");
	}

	return buf_cmd_flags;
}

void print_command_count(void)
{
	struct command *cmd;
	int i, j;

	printf("/* Do not edit. This file is generated by tools/create-commands */\n");
	printf("/* using command definitions from tools/command-lines.in */\n");
	printf("#define COMMAND_COUNT %d\n", cmd_count);

	printf("enum {\n");
	printf("\tno_CMD,\n");  /* enum value 0 is not used */

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];

		if (!cmd->command_line_id) {
			printf("Missing ID: at %d\n", i);
			exit(1);
		}

		for (j = 0; j < i; j++) {
			if (!strcmp(cmd->command_line_id, cmd_array[j].command_line_id))
				goto next;
		}

		printf("\t%s_CMD,\n", cmd->command_line_id);
	next:
		;
	}
	printf("\tCOMMAND_ID_COUNT,\n");
	printf("};\n");
}

static int is_lvm_all_opt(int opt)
{
	int oo;

	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		if (lvm_all.optional_opt_args[oo].opt == opt)
			return 1;
	}
	return 0;
}

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

	/*
	for (cn = 0; cn < MAX_CMD_NAMES; cn++) {
		if (!cmd_names[cn].name)
			break;

		printf("%s (%d)\n", cmd_names[cn].name, cmd_names[cn].variants);
		for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
			if (cmd_names[cn].common_options[opt_enum])
				printf("  %s\n", opt_names[opt_enum].long_opt);
		}
	}
	*/
}

void print_usage_common(struct command *cmd)
{
	struct cmd_name *cname;
	int i, sep, ro, rp, oo, op, opt_enum;

	if (!(cname = find_command_name(cmd->name)))
		return;

	sep = 0;

	/*
	 * when there's more than one variant, options that
	 * are common to all commands with a common name.
	 */

	if (cname->variants < 2)
		goto all;

	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cname->common_options[opt_enum])
			continue;

		if (is_lvm_all_opt(opt_enum))
			continue;

		if (!sep) {
			printf("\n");
			printf("\" [");
		} else {
			printf(",");
		}

		for (oo = 0; oo < cmd->oo_count; oo++) {
			if (cmd->optional_opt_args[oo].opt != opt_enum)
				continue;

			printf(" %s", opt_names[opt_enum].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
			break;
		}
	}

 all:
	/* options that are common to all lvm commands */

	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		opt_enum = lvm_all.optional_opt_args[oo].opt;

		if (!sep) {
			printf("\n");
			printf("\" [");
		} else {
			printf(",");
		}

		printf(" %s", opt_names[opt_enum].long_opt);
		if (lvm_all.optional_opt_args[oo].def.val_bits) {
			printf(" ");
			print_def(&lvm_all.optional_opt_args[oo].def, 1);
		}
		sep = 1;
	}

	printf(" ]\"");
	printf(";\n");
}

void print_usage(struct command *cmd)
{
	struct cmd_name *cname;
	int onereq = (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT) ? 1 : 0;
	int i, sep, ro, rp, oo, op, opt_enum;

	if (!(cname = find_command_name(cmd->name)))
		return;

	printf("\"%s", cmd->name);

	if (cmd->ro_count) {
		if (onereq)
			printf(" (");
		for (ro = 0; ro < cmd->ro_count; ro++) {
			if (ro && onereq)
				printf(",");
			printf(" %s", opt_names[cmd->required_opt_args[ro].opt].long_opt);

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def(&cmd->required_opt_args[ro].def, 1);
			}
		}
		if (onereq)
			printf(" )");
	}

	if (cmd->rp_count) {
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_def(&cmd->required_pos_args[rp].def, 1);
			}
		}
	}

	printf("\"");

 oo_count:
	if (!cmd->oo_count)
		goto op_count;

	sep = 0;

	if (cmd->oo_count) {
		printf("\n");
		printf("\" [");

		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			/*
			 * Skip common opts which are in the usage_common string.
			 * The common opts are those in lvm_all and in
			 * cname->common_options.
			 */

			if (is_lvm_all_opt(opt_enum))
				continue;

			if ((cname->variants > 1) && cname->common_options[opt_enum])
				continue;

			if (sep)
				printf(",");

			printf(" %s", opt_names[opt_enum].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
		}

		if (sep)
			printf(",");
		printf(" COMMON_OPTIONS");
		printf(" ]\"");
	}

 op_count:
	if (!cmd->op_count)
		goto done;

	printf("\n");
	printf("\" [");

	if (cmd->op_count) {
		for (op = 0; op < cmd->op_count; op++) {
			if (cmd->optional_pos_args[op].def.val_bits) {
				printf(" ");
				print_def(&cmd->optional_pos_args[op].def, 1);
			}
		}
	}

	printf(" ]\"");

 done:
	printf(";\n");
}

static void print_val_man(const char *str)
{
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;
	int i;

	if (!strcmp(str, "Number") ||
	    !strcmp(str, "String") ||
	    !strncmp(str, "VG", 2) ||
	    !strncmp(str, "LV", 2) ||
	    !strncmp(str, "PV", 2) ||
	    !strcmp(str, "Tag")) {
		printf("\\fI%s\\fP", str);
		return;
	}

	if (strstr(str, "Number[") || strstr(str, "]Number")) {
		for (i = 0; i < strlen(str); i++) {
			if (str[i] == 'N')
				printf("\\fI");
			if (str[i] == 'r') {
				printf("%c", str[i]);
				printf("\\fP");
				continue;
			}
			printf("%c", str[i]);
		}
		return;
	}

	if (strstr(str, "|")) {
		int len = strlen(str);
		line = strdup(str);
		split_line(line, &line_argc, line_argv, '|');
		for (i = 0; i < line_argc; i++) {
			if (i) {
				printf("|");

				/* this is a hack to add a line break for
				   a long string of opt values */
				if ((len > 40) && (i >= (line_argc / 2) + 1)) {
					printf("\n");
					printf("       ");
					len = 0;
				}
			}
			if (strstr(line_argv[i], "Number"))
				printf("\\fI%s\\fP", line_argv[i]);
			else
				printf("\\fB%s\\fP", line_argv[i]);
		}
		return;
	}

	printf("\\fB%s\\fP", str);
}

static void print_def_man(struct arg_def *def, int usage)
{
	int val_enum;
	int lvt_enum;
	int sep = 0;
	int i;

	for (val_enum = 0; val_enum < VAL_COUNT; val_enum++) {
		if (def->val_bits & val_enum_to_bit(val_enum)) {

			if (val_enum == conststr_VAL) {
				printf("\\fB");
				printf("%s", def->str);
				printf("\\fP");
			}

			else if (val_enum == constnum_VAL) {
				printf("\\fB");
				printf("%llu", (unsigned long long)def->num);
				printf("\\fP");
			}

			else {
				if (sep) printf("|");

				if (!usage || !val_names[val_enum].usage) {
					printf("\\fI");
					printf("%s", val_names[val_enum].name);
					printf("\\fP");
				} else {
					print_val_man(val_names[val_enum].usage);
				}

				sep = 1;
			}

			if (val_enum == lv_VAL && def->lvt_bits) {
				printf("\\fI");
				for (lvt_enum = 1; lvt_enum < LVT_COUNT; lvt_enum++) {
					if (lvt_bit_is_set(def->lvt_bits, lvt_enum))
						printf("_%s", lvt_enum_to_name(lvt_enum));
				}
				printf("\\fP");
			}

			if ((val_enum == vg_VAL) && (def->flags & ARG_DEF_FLAG_NEW_VG)) {
				printf("\\fI");
				printf("_new");
				printf("\\fP");
			}
			if ((val_enum == lv_VAL) && (def->flags & ARG_DEF_FLAG_NEW_LV)) {
				printf("\\fI");
				printf("_new");
				printf("\\fP");
			}
		}
	}

	if (def->flags & ARG_DEF_FLAG_MAY_REPEAT)
		printf(" ...");
}

static char *man_long_opt_name(const char *cmdname, int opt_enum)
{
	static char long_opt_name[64];

	memset(&long_opt_name, 0, sizeof(long_opt_name));

	switch (opt_enum) {
	case syncaction_ARG:
		strncpy(long_opt_name, "--[raid]syncaction", 63);
		break;
	case writemostly_ARG:
		strncpy(long_opt_name, "--[raid]writemostly", 63);
		break;
	case minrecoveryrate_ARG:
		strncpy(long_opt_name, "--[raid]minrecoveryrate", 63);
		break;
	case maxrecoveryrate_ARG:
		strncpy(long_opt_name, "--[raid]maxrecoveryrate", 63);
		break;
	case writebehind_ARG:
		strncpy(long_opt_name, "--[raid]writebehind", 63);
		break;
	case vgmetadatacopies_ARG:
		if (!strncmp(cmdname, "vg", 2))
			strncpy(long_opt_name, "--[vg]metadatacopies", 63);
		else
			strncpy(long_opt_name, "--vgmetadatacopies", 63);
		break;
	case pvmetadatacopies_ARG:
		if (!strncmp(cmdname, "pv", 2))
			strncpy(long_opt_name, "--[pv]metadatacopies", 63);
		else
			strncpy(long_opt_name, "--pvmetadatacopies", 63);
		break;
	default:
		strncpy(long_opt_name, opt_names[opt_enum].long_opt, 63);
		break;
	}

	return long_opt_name;
}

void print_man_usage(struct command *cmd)
{
	struct cmd_name *cname;
	int onereq = (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT) ? 1 : 0;
	int i, sep, ro, rp, oo, op, opt_enum;

	if (!(cname = find_command_name(cmd->name)))
		return;

	printf("\\fB%s\\fP", cmd->name);

	if (!onereq)
		goto ro_normal;

	/*
	 * one required option in a set, print as:
	 * ( -a|--a,
	 *   -b|--b,
	 *      --c,
	 *      --d )
	 *
	 * First loop through ro prints those with short opts,
	 * and the second loop prints those without short opts.
	 */

	if (cmd->ro_count) {
		printf("\n");
		printf(".RS 4\n");
		printf("(");

		sep = 0;

		/* print required options with a short opt */
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if (!opt_names[opt_enum].short_opt)
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			if (opt_names[opt_enum].short_opt) {
				printf(" \\fB-%c\\fP|\\fB%s\\fP",
				       opt_names[opt_enum].short_opt,
				       man_long_opt_name(cmd->name, opt_enum));
			} else {
				printf("   ");
				printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));
			}

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}

			sep = 1;
		}

		/* print required options without a short opt */
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if (opt_names[opt_enum].short_opt)
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			printf("   ");
			printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}

			sep = 1;
		}

		printf(" )\n");
		printf(".RE\n");
	}

	/* print required position args on a new line after the onereq set */
	if (cmd->rp_count) {
		printf(".RS 4\n");
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_pos_args[rp].def, 1);
			}
		}

		printf("\n");
		printf(".RE\n");
	} else {
		/* printf("\n"); */
	}

	printf(".br\n");
	goto oo_count;

 ro_normal:

	/*
	 * all are required options, print as:
	 * -a|--aaa <val> -b|--bbb <val>
	 */

	if (cmd->ro_count) {
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if (opt_names[opt_enum].short_opt) {
				printf(" \\fB-%c\\fP|\\fB%s\\fP",
				       opt_names[opt_enum].short_opt,
				       man_long_opt_name(cmd->name, opt_enum));
			} else {
				printf(" \\fB%s\\fP", opt_names[cmd->required_opt_args[ro].opt].long_opt);
			}

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}
		}
	}

	/* print required position args on the same line as the required options */
	if (cmd->rp_count) {
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_pos_args[rp].def, 1);
			}
		}

		printf("\n");
	} else {
		printf("\n");
	}

	printf(".br\n");

 oo_count:
	if (!cmd->oo_count)
		goto op_count;

	sep = 0;

	if (cmd->oo_count) {
		printf(".RS 4\n");
		printf("[");

		/* print optional options with short opts */

		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			if (!opt_names[opt_enum].short_opt)
				continue;

			/*
			 * Skip common opts which are in the usage_common string.
			 * The common opts are those in lvm_all and in
			 * cname->common_options.
			 */

			if (is_lvm_all_opt(opt_enum))
				continue;

			if ((cname->variants > 1) && cname->common_options[opt_enum])
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			printf(" \\fB-%c\\fP|\\fB%s\\fP",
				opt_names[opt_enum].short_opt,
				man_long_opt_name(cmd->name, opt_enum));

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
		}

		/* print optional options without short opts */

		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			if (opt_names[opt_enum].short_opt)
				continue;

			/*
			 * Skip common opts which are in the usage_common string.
			 * The common opts are those in lvm_all and in
			 * cname->common_options.
			 */

			if (is_lvm_all_opt(opt_enum))
				continue;

			if ((cname->variants > 1) && cname->common_options[opt_enum])
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			/* space alignment without short opt */
			printf("   ");

			printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
		}

		if (sep) {
			printf(",");
			printf("\n.br\n");
			printf(" ");
			/* space alignment without short opt */
			printf("   ");
		}
		printf(" COMMON_OPTIONS");
		printf(" ]\n");
		printf(".RE\n");
		printf(".br\n");
	}

 op_count:
	if (!cmd->op_count)
		goto done;

	printf(".RS 4\n");
	printf("[");

	if (cmd->op_count) {
		for (op = 0; op < cmd->op_count; op++) {
			if (cmd->optional_pos_args[op].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_pos_args[op].def, 1);
			}
		}
	}

	printf(" ]\n");
	printf(".RE\n");

 done:
	printf("\n");
}

void print_man_usage_common(struct command *cmd)
{
	struct cmd_name *cname;
	int i, sep, ro, rp, oo, op, opt_enum;

	if (!(cname = find_command_name(cmd->name)))
		return;

	sep = 0;

	printf(".RS 4\n");
	printf("[");

	/*
	 * when there's more than one variant, options that
	 * are common to all commands with a common name.
	 */

	if (cname->variants < 2)
		goto all;

	/* print those with short opts */
	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cname->common_options[opt_enum])
			continue;

		if (!opt_names[opt_enum].short_opt)
			continue;

		if (is_lvm_all_opt(opt_enum))
			continue;

		if (sep) {
			printf(",");
			printf("\n.br\n");
			printf(" ");
		}

		for (oo = 0; oo < cmd->oo_count; oo++) {
			if (cmd->optional_opt_args[oo].opt != opt_enum)
				continue;

			printf(" \\fB-%c\\fP|\\fB%s\\fP",
				opt_names[opt_enum].short_opt,
				man_long_opt_name(cmd->name, opt_enum));

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
			break;
		}

	}

	/* print those without short opts */
	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cname->common_options[opt_enum])
			continue;

		if (opt_names[opt_enum].short_opt)
			continue;

		if (is_lvm_all_opt(opt_enum))
			continue;

		if (sep) {
			printf(",");
			printf("\n.br\n");
			printf(" ");
		}

		for (oo = 0; oo < cmd->oo_count; oo++) {
			if (cmd->optional_opt_args[oo].opt != opt_enum)
				continue;

			/* space alignment without short opt */
			printf("   ");

			printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
			break;
		}
	}
 all:
	/* options that are common to all lvm commands */

	/* those with short opts */
	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		opt_enum = lvm_all.optional_opt_args[oo].opt;

		if (!opt_names[opt_enum].short_opt)
			continue;

		if (sep) {
			printf(",");
			printf("\n.br\n");
			printf(" ");
		}

		printf(" \\fB-%c\\fP|\\fB%s\\fP",
			opt_names[opt_enum].short_opt,
			man_long_opt_name(cmd->name, opt_enum));

		if (lvm_all.optional_opt_args[oo].def.val_bits) {
			printf(" ");
			print_def(&lvm_all.optional_opt_args[oo].def, 1);
		}
		sep = 1;
	}

	/* those without short opts */
	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		opt_enum = lvm_all.optional_opt_args[oo].opt;

		if (opt_names[opt_enum].short_opt)
			continue;

		if (sep) {
			printf(",");
			printf("\n.br\n");
			printf(" ");
		}

		/* space alignment without short opt */
		printf("   ");

		printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));

		if (lvm_all.optional_opt_args[oo].def.val_bits) {
			printf(" ");
			print_def(&lvm_all.optional_opt_args[oo].def, 1);
		}
		sep = 1;
	}
	printf(" ]\n");
}

void print_man_all_options(struct cmd_name *cname)
{
	int opt_enum, val_enum;
	int sep = 0;

	/* print those with short opts */
	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cname->all_options[opt_enum])
			continue;

		if (!opt_names[opt_enum].short_opt)
			continue;

		if (sep)
			printf("\n.br\n");

		printf(" \\fB-%c\\fP|\\fB%s\\fP",
			opt_names[opt_enum].short_opt,
			man_long_opt_name(cname->name, opt_enum));

		val_enum = opt_names[opt_enum].val_enum;

		if (!val_names[val_enum].fn) {
			/* takes no arg */
		} else if (!val_names[val_enum].usage) {
			printf(" ");
			printf("\\fI");
			printf("%s", val_names[val_enum].name);
			printf("\\fP");
		} else {
			printf(" ");
			print_val_man(val_names[val_enum].usage);
		}

		sep = 1;
	}

	/* print those without short opts */
	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cname->all_options[opt_enum])
			continue;

		if (opt_names[opt_enum].short_opt)
			continue;

		if (sep)
			printf("\n.br\n");

		/* space alignment without short opt */
		printf("   ");

		printf(" \\fB%s\\fP", man_long_opt_name(cname->name, opt_enum));

		val_enum = opt_names[opt_enum].val_enum;

		if (!val_names[val_enum].fn) {
			/* takes no arg */
		} else if (!val_names[val_enum].usage) {
			printf(" ");
			printf("\\fI");
			printf("%s", val_names[val_enum].name);
			printf("\\fP");
		} else {
			printf(" ");
			print_val_man(val_names[val_enum].usage);
		}

		sep = 1;
	}
}

#define DESC_LINE 256

void print_desc_man(const char *desc)
{
	char buf[DESC_LINE] = {0};
	int di = 0;
	int bi = 0;

	for (di = 0; di < strlen(desc); di++) {
		if (desc[di] == '\0')
			break;
		if (desc[di] == '\n')
			continue;

		if (!strncmp(&desc[di], "DESC:", 5)) {
			if (bi) {
				printf("%s\n", buf);
				printf(".br\n");
				memset(buf, 0, sizeof(buf));
				bi = 0;
			}
			di += 5;
			continue;
		}

		if (!bi && desc[di] == ' ')
			continue;

		buf[bi++] = desc[di];

		if (bi == (DESC_LINE - 1))
			break;
	}

	if (bi) {
		printf("%s\n", buf);
		printf(".br\n");
	}
}

static char *upper_command_name(char *str)
{
	static char str_upper[32];
	int i = 0;

	while (*str) {
		str_upper[i++] = toupper(*str);
		str++;
	}
	str_upper[i] = '\0';
	return str_upper;
}

void print_man_command(void)
{
	struct cmd_name *cname;
	struct command *cmd, *prev_cmd = NULL;
	const char *desc;
	int i, j, ro, rp, oo, op;

	include_optional_opt_args(&lvm_all, "OO_USAGE_COMMON");

	printf(".TH %s 8 \"LVM TOOLS #VERSION#\" \"Sistina Software UK\"\n",
		man_command_name ? upper_command_name(man_command_name) : "LVM_COMMANDS");

	for (i = 0; i < cmd_count; i++) {

		cmd = &cmd_array[i];

		if (prev_cmd && strcmp(prev_cmd->name, cmd->name)) {
			printf("Common options:\n");
			printf(".\n");
			print_man_usage_common(prev_cmd);
			prev_cmd = NULL;
		}

		if ((cmd->cmd_flags & CMD_FLAG_SECONDARY_SYNTAX) && !include_man_secondary)
			continue;

		if (!(cmd->cmd_flags & CMD_FLAG_SECONDARY_SYNTAX) && !include_man_primary)
			continue;

		if (man_command_name && strcmp(man_command_name, cmd->name))
			continue;

		if (!prev_cmd || strcmp(prev_cmd->name, cmd->name)) {
			printf(".SH NAME\n");
			printf(".\n");
			if ((desc = cmd_name_desc(cmd->name)))
				printf("%s \\- %s\n", cmd->name, desc);
			else
				printf("%s\n", cmd->name);
			printf(".br\n");
			printf(".P\n");
			printf(".\n");
			printf(".SH SYNOPSIS\n");
			printf(".br\n");
			printf(".P\n");
			printf(".\n");
			prev_cmd = cmd;

			if (!(cname = find_command_name(cmd->name)))
				return;

			if (cname->variant_has_ro && cname->variant_has_rp)
				printf("\\fB%s\\fP \\fIrequired_option_args\\fP \\fIrequired_position_args\\fP\n", cmd->name);
			else if (cname->variant_has_ro && !cname->variant_has_rp)
				printf("\\fB%s\\fP \\fIrequired_option_args\\fP\n", cmd->name);
			else if (!cname->variant_has_ro && cname->variant_has_rp)
				printf("\\fB%s\\fP \\fIrequired_position_args\\fP\n", cmd->name);
			else if (!cname->variant_has_ro && !cname->variant_has_rp)
				printf("\\fB%s\\fP\n", cmd->name);

			printf(".br\n");

			if (cname->variant_has_oo) {
				printf("    [ \\fIoptional_option_args\\fP ]\n");
				printf(".br\n");
			}

			if (cname->variant_has_op) {
				printf("    [ \\fIoptional_position_args\\fP ]\n");
				printf(".br\n");
			}

			printf(".P\n");
			printf("\n");

			/* listing them all when there's only 1 or 2 is just repetative */
			if (cname->variants > 2) {
				printf(".P\n");
				print_man_all_options(cname);
				printf("\n");
				printf(".P\n");
				printf("\n");
			}

			printf(".SH USAGE\n");
			printf(".br\n");
			printf(".P\n");
			printf(".\n");
		}

		if (cmd->desc) {
			print_desc_man(cmd->desc);
			printf(".P\n");
		}

		print_man_usage(cmd);

		if (i == (cmd_count - 1)) {
			printf("Common options:\n");
			printf(".\n");
			print_man_usage_common(cmd);
		}

		printf("\n");
		continue;
	}
}

void print_command_struct(int only_usage)
{
	struct command *cmd;
	int i, j, ro, rp, oo, op, ru, ruo, io;

	include_optional_opt_args(&lvm_all, "OO_USAGE_COMMON");

	printf("/* Do not edit. This file is generated by tools/create-commands */\n");
	printf("/* using command definitions from tools/command-lines.in */\n");
	printf("\n");

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];

		if (only_usage) {
			print_usage(cmd);
			print_usage_common(cmd);
			printf("\n");
			continue;
		}

		printf("commands[%d].name = \"%s\";\n", i, cmd->name);
		printf("commands[%d].command_line_id = \"%s\";\n", i, cmd->command_line_id);
		printf("commands[%d].command_line_enum = %s_CMD;\n", i, cmd->command_line_id);
		printf("commands[%d].fn = %s;\n", i, cmd->name);
		printf("commands[%d].ro_count = %d;\n", i, cmd->ro_count);
		printf("commands[%d].rp_count = %d;\n", i, cmd->rp_count);
		printf("commands[%d].oo_count = %d;\n", i, cmd->oo_count);
		printf("commands[%d].op_count = %d;\n", i, cmd->op_count);
		printf("commands[%d].io_count = %d;\n", i, cmd->io_count);
		printf("commands[%d].rule_count = %d;\n", i, cmd->rule_count);

		if (cmd->cmd_flags)
			printf("commands[%d].cmd_flags = %s;\n", i, cmd_flags_to_str(cmd->cmd_flags));
		else
			printf("commands[%d].cmd_flags = 0;\n", i, cmd_flags_to_str(cmd->cmd_flags));

		printf("commands[%d].desc = \"%s\";\n", i, cmd->desc ?: "");
		printf("commands[%d].usage = ", i);
		print_usage(cmd);

		if (cmd->oo_count) {
			printf("commands[%d].usage_common = ", i);
			print_usage_common(cmd);
		} else {
			printf("commands[%d].usage_common = \"NULL\";\n", i);
		}

		if (cmd->ro_count) {
			for (ro = 0; ro < cmd->ro_count; ro++) {
				printf("commands[%d].required_opt_args[%d].opt = %s;\n",
					i, ro, opt_to_enum_str(cmd->required_opt_args[ro].opt));

				if (!cmd->required_opt_args[ro].def.val_bits)
					continue;

				printf("commands[%d].required_opt_args[%d].def.val_bits = %s;\n",
					i, ro, val_bits_to_str(cmd->required_opt_args[ro].def.val_bits));

				if (cmd->required_opt_args[ro].def.lvt_bits)
					printf("commands[%d].required_opt_args[%d].def.lvt_bits = %s;\n",
						i, ro, lvt_bits_to_str(cmd->required_opt_args[ro].def.lvt_bits));

				if (cmd->required_opt_args[ro].def.flags)
					printf("commands[%d].required_opt_args[%d].def.flags = %s;\n",
						i, ro, flags_to_str(cmd->required_opt_args[ro].def.flags));

				if (val_bit_is_set(cmd->required_opt_args[ro].def.val_bits, constnum_VAL))
					printf("commands[%d].required_opt_args[%d].def.num = %d;\n",
						i, ro, cmd->required_opt_args[ro].def.num);

				if (val_bit_is_set(cmd->required_opt_args[ro].def.val_bits, conststr_VAL))
					printf("commands[%d].required_opt_args[%d].def.str = \"%s\";\n",
						i, ro, cmd->required_opt_args[ro].def.str ?: "NULL");
			}
		}

		if (cmd->rp_count) {
			for (rp = 0; rp < cmd->rp_count; rp++) {
				printf("commands[%d].required_pos_args[%d].pos = %d;\n",
					i, rp, cmd->required_pos_args[rp].pos);

				if (!cmd->required_pos_args[rp].def.val_bits)
					continue;

				printf("commands[%d].required_pos_args[%d].def.val_bits = %s;\n",
					i, rp, val_bits_to_str(cmd->required_pos_args[rp].def.val_bits));

				if (cmd->required_pos_args[rp].def.lvt_bits)
					printf("commands[%d].required_pos_args[%d].def.lvt_bits = %s;\n",
						i, rp, lvt_bits_to_str(cmd->required_pos_args[rp].def.lvt_bits));

				if (cmd->required_pos_args[rp].def.flags)
					printf("commands[%d].required_pos_args[%d].def.flags = %s;\n",
						i, rp, flags_to_str(cmd->required_pos_args[rp].def.flags));

				if (val_bit_is_set(cmd->required_pos_args[rp].def.val_bits, constnum_VAL))
					printf("commands[%d].required_pos_args[%d].def.num = %d;\n",
						i, rp, cmd->required_pos_args[rp].def.num);

				if (val_bit_is_set(cmd->required_pos_args[rp].def.val_bits, conststr_VAL))
					printf("commands[%d].required_pos_args[%d].def.str = \"%s\";\n",
						i, rp, cmd->required_pos_args[rp].def.str ?: "NULL");
			}
		}

		if (cmd->oo_count) {
			for (oo = 0; oo < cmd->oo_count; oo++) {
				printf("commands[%d].optional_opt_args[%d].opt = %s;\n",
					i, oo, opt_to_enum_str(cmd->optional_opt_args[oo].opt));

				if (!cmd->optional_opt_args[oo].def.val_bits)
					continue;

				printf("commands[%d].optional_opt_args[%d].def.val_bits = %s;\n",
					i, oo, val_bits_to_str(cmd->optional_opt_args[oo].def.val_bits));

				if (cmd->optional_opt_args[oo].def.lvt_bits)
					printf("commands[%d].optional_opt_args[%d].def.lvt_bits = %s;\n",
						i, oo, lvt_bits_to_str(cmd->optional_opt_args[oo].def.lvt_bits));

				if (cmd->optional_opt_args[oo].def.flags)
					printf("commands[%d].optional_opt_args[%d].def.flags = %s;\n",
						i, oo, flags_to_str(cmd->optional_opt_args[oo].def.flags));

				if (val_bit_is_set(cmd->optional_opt_args[oo].def.val_bits, constnum_VAL)) 
					printf("commands[%d].optional_opt_args[%d].def.num = %d;\n",
						i, oo, cmd->optional_opt_args[oo].def.num);

				if (val_bit_is_set(cmd->optional_opt_args[oo].def.val_bits, conststr_VAL))
					printf("commands[%d].optional_opt_args[%d].def.str = \"%s\";\n",
						i, oo, cmd->optional_opt_args[oo].def.str ?: "NULL");
			}
		}

		if (cmd->io_count) {
			for (io = 0; io < cmd->io_count; io++) {
				printf("commands[%d].ignore_opt_args[%d].opt = %s;\n",
					i, io, opt_to_enum_str(cmd->ignore_opt_args[io].opt));

				if (!cmd->ignore_opt_args[io].def.val_bits)
					continue;

				printf("commands[%d].ignore_opt_args[%d].def.val_bits = %s;\n",
					i, io, val_bits_to_str(cmd->ignore_opt_args[io].def.val_bits));

				if (cmd->ignore_opt_args[io].def.lvt_bits)
					printf("commands[%d].ignore_opt_args[%d].def.lvt_bits = %s;\n",
						i, io, lvt_bits_to_str(cmd->ignore_opt_args[io].def.lvt_bits));

				if (cmd->ignore_opt_args[io].def.flags)
					printf("commands[%d].ignore_opt_args[%d].def.flags = %s;\n",
						i, io, flags_to_str(cmd->ignore_opt_args[io].def.flags));

				if (val_bit_is_set(cmd->ignore_opt_args[io].def.val_bits, constnum_VAL)) 
					printf("commands[%d].ignore_opt_args[%d].def.num = %d;\n",
						i, io, cmd->ignore_opt_args[io].def.num);

				if (val_bit_is_set(cmd->ignore_opt_args[io].def.val_bits, conststr_VAL))
					printf("commands[%d].ignore_opt_args[%d].def.str = \"%s\";\n",
						i, io, cmd->ignore_opt_args[io].def.str ?: "NULL");
			}
		}

		if (cmd->op_count) {
			for (op = 0; op < cmd->op_count; op++) {
				printf("commands[%d].optional_pos_args[%d].pos = %d;\n",
					i, op, cmd->optional_pos_args[op].pos);

				if (!cmd->optional_pos_args[op].def.val_bits)
					continue;

				printf("commands[%d].optional_pos_args[%d].def.val_bits = %s;\n",
					i, op, val_bits_to_str(cmd->optional_pos_args[op].def.val_bits));

				if (cmd->optional_pos_args[op].def.lvt_bits)
					printf("commands[%d].optional_pos_args[%d].def.lvt_bits = %s;\n",
						i, op, lvt_bits_to_str(cmd->optional_pos_args[op].def.lvt_bits));

				if (cmd->optional_pos_args[op].def.flags)
					printf("commands[%d].optional_pos_args[%d].def.flags = %s;\n",
						i, op, flags_to_str(cmd->optional_pos_args[op].def.flags));

				if (val_bit_is_set(cmd->optional_pos_args[op].def.val_bits, constnum_VAL))
					printf("commands[%d].optional_pos_args[%d].def.num = %d;\n",
						i, op, cmd->optional_pos_args[op].def.num);

				if (val_bit_is_set(cmd->optional_pos_args[op].def.val_bits, conststr_VAL))
					printf("commands[%d].optional_pos_args[%d].def.str = \"%s\";\n",
						i, op, cmd->optional_pos_args[op].def.str ?: "NULL");
			}
		}

		if (cmd->rule_count) {
			for (ru = 0; ru < cmd->rule_count; ru++) {

				printf("commands[%d].rules[%d].opts_count = %d;\n", i, ru, cmd->rules[ru].opts_count);

				if (cmd->rules[ru].opts_count) {
					printf("static int _command%d_rule%d_opts[] = { ", i, ru);
					for (ruo = 0; ruo < cmd->rules[ru].opts_count; ruo++) {
						if (ruo)
							printf(", ");
						printf("%s", opt_to_enum_str(cmd->rules[ru].opts[ruo]));
					}
					printf(" };\n");
					printf("commands[%d].rules[%d].opts = _command%d_rule%d_opts;\n", i, ru, i, ru);
				} else {
					printf("commands[%d].rules[%d].opts = NULL;\n", i, ru);
				}

				printf("commands[%d].rules[%d].check_opts_count = %d;\n", i, ru, cmd->rules[ru].check_opts_count);

				if (cmd->rules[ru].check_opts_count) {
					printf("static int _command%d_rule%d_check_opts[] = { ", i, ru);
					for (ruo = 0; ruo < cmd->rules[ru].check_opts_count; ruo++) {
						if (ruo)
							printf(",");
						printf("%s ", opt_to_enum_str(cmd->rules[ru].check_opts[ruo]));
					}
					printf(" };\n");
					printf("commands[%d].rules[%d].check_opts = _command%d_rule%d_check_opts;\n", i, ru, i, ru);
				} else {
					printf("commands[%d].rules[%d].check_opts = NULL;\n", i, ru);
				}

				printf("commands[%d].rules[%d].lvt_bits = %s;\n", i, ru,
					cmd->rules[ru].lvt_bits ? lvt_bits_to_str(cmd->rules[ru].lvt_bits) : "0");

				printf("commands[%d].rules[%d].lvp_bits = %s;\n", i, ru,
					cmd->rules[ru].lvp_bits ? lvp_bits_to_str(cmd->rules[ru].lvp_bits) : "0");

				printf("commands[%d].rules[%d].rule = %s;\n", i, ru,
					rule_to_define_str(cmd->rules[ru].rule));

				printf("commands[%d].rules[%d].check_lvt_bits = %s;\n", i, ru,
					cmd->rules[ru].check_lvt_bits ? lvt_bits_to_str(cmd->rules[ru].check_lvt_bits) : "0");

				printf("commands[%d].rules[%d].check_lvp_bits = %s;\n", i, ru,
					cmd->rules[ru].check_lvp_bits ? lvp_bits_to_str(cmd->rules[ru].check_lvp_bits) : "0");
			}
		}

		printf("\n");
	}
}

struct cmd_pair {
	int i, j;
};

static void print_ambiguous(void)
{
	struct command *cmd, *dup;
	struct cmd_pair dups[64] = { 0 };
	int found = 0;
	int i, j, f, ro, rp;

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];

		for (j = 0; j < cmd_count; j++) {
			dup = &cmd_array[j];

			if (i == j)
				continue;
			if (strcmp(cmd->name, dup->name))
				continue;
			if (cmd->ro_count != dup->ro_count)
				continue;
			if (cmd->rp_count != dup->rp_count)
				continue;

			for (ro = 0; ro < cmd->ro_count; ro++) {
				if (!opt_arg_matches(&cmd->required_opt_args[ro],
						     &dup->required_opt_args[ro]))
					goto next;
			}

			for (rp = 0; rp < cmd->rp_count; rp++) {
				if (!pos_arg_matches(&cmd->required_pos_args[rp],
						     &dup->required_pos_args[rp]))
					goto next;
			}

			for (f = 0; f < found; f++) {
				if ((dups[f].i == j) && (dups[f].j == i))
					goto next;
			}

			printf("Ambiguous commands %d and %d:\n", i, j);
			print_usage(cmd);
			print_usage(dup);
			printf("\n");

			dups[found].i = i;
			dups[found].j = j;
			found++;
next:
			;
		}
	}
}

void print_command_list(void)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!cmd_names[i].name) {
			printf("found %d command names\n", i);
			break;
		}
		printf("%s\n", cmd_names[i].name);
	}
}

void print_option_list(void)
{
	int i;

	for (i = 0; i < ARG_COUNT; i++)
		printf("%d %s %s %c (%d)\n",
			opt_names[i].opt_enum, opt_names[i].name,
			opt_names[i].long_opt, opt_names[i].short_opt ?: ' ',
			opt_names[i].short_opt ? opt_names[i].short_opt : 0);
}

static void print_help(int argc, char *argv[])
{
	printf("%s [options] --output <format> <filename>\n", argv[0]);
	printf("\n");
	printf("output formats:\n");
	printf("struct:    print C structures for command-lines.h\n");
	printf("count:     print defines and enums for command-lines-count.h\n");
	printf("ambiguous: print commands differing only by LV types\n");
	printf("usage:     print usage format.\n");
	printf("expanded:  print expanded input format.\n");
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

	if (!strcmp(argv[1], "debug")) {
		print_command_list();
		print_option_list();
		return 0;
	}

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

	factor_common_options();

	if (!outputformat)
		print_command_struct(1);
	else if (!strcmp(outputformat, "struct")) {
		print_command_struct(0);
		print_ambiguous();
	}
	else if (!strcmp(outputformat, "count"))
		print_command_count();
	else if (!strcmp(outputformat, "usage"))
		print_command_struct(1);
	else if (!strcmp(outputformat, "expanded"))
		print_expanded();
	else if (!strcmp(outputformat, "ambiguous"))
		print_ambiguous();
	else if (!strcmp(outputformat, "man"))
		print_man_command();
	else
		print_help(argc, argv);

	return 0;
}

