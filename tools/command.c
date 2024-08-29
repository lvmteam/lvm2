/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2024 Red Hat, Inc. All rights reserved.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>

#ifndef MAN_PAGE_GENERATOR
#include "tools.h"
#endif /* MAN_PAGE_GENERATOR */


/* see cmd_names[] below, one for each unique "ID" in command-lines.in */

struct cmd_name {
	const char name[68];   /* "foo" from string after ID: */
	uint16_t cmd_enum;     /* foo_CMD */
};

/* create table of value names, e.g. String, and corresponding enum from vals.h */

static const struct val_name val_names[VAL_COUNT + 1] = {
#define val(a, b, c, d) { b, d, c, sizeof(c) - 1, a },
#include "vals.h"
#undef val
};

/* create table of option names, e.g. --foo, and corresponding enum from args.h */

static const struct opt_name opt_names[ARG_COUNT + 1] = {
#define arg(a, b, c, d, e, f, g) { g, "--" c, b, a, d, e, f },
#include "args.h"
#undef arg
};

/* create table of lv property names, e.g. lv_is_foo, and corresponding enum from lv_props.h */

static const struct lv_prop _lv_props[LVP_COUNT + 1] = {
	{ "" },
#define lvp(a) { "lv_" # a, a ## _LVP },
#include "lv_props.h"
#undef lvp
};

/* create table of lv type names, e.g. linear and corresponding enum from lv_types.h */

static const struct lv_type _lv_types[LVT_COUNT + 1] = {
	{ "" },
#define lvt(a) { # a, a ## _LVT },
#include "lv_types.h"
#undef lvt
};

/* create table of command IDs */

static const struct cmd_name cmd_names[CMD_COUNT + 1] = {
#define cmd(a, b) { # b, a },
#include "../include/cmds.h"
#undef cmd
};

/*
 * command_names[] and commands[] are defined in lvmcmdline.c when building lvm,
 * but need to be defined here when building the stand-alone man page generator.
 */

#ifdef MAN_PAGE_GENERATOR

static const struct command_name command_names[] = {
#define xx(a, b, c...) { # a, b, c, NULL, a ## _COMMAND },
#include "commands.h"
#undef xx
};
static struct command commands[COMMAND_COUNT];

#else /* MAN_PAGE_GENERATOR */

const struct command_name command_names[] = {
#define xx(a, b, c...) { # a, b, c, a, a ## _COMMAND },
#include "commands.h"
#undef xx
};

extern struct command commands[COMMAND_COUNT]; /* defined in lvmcmdline.c */

const struct opt_name *get_opt_name(int opt)
{
	return &opt_names[opt];
}

const struct val_name *get_val_name(int val)
{
	return &val_names[val];
}

const struct lv_prop *get_lv_prop(int lvp_enum)
{
	if (!lvp_enum)
		return NULL;
	return &_lv_props[lvp_enum];
}

const struct lv_type *get_lv_type(int lvt_enum)
{
	if (!lvt_enum)
		return NULL;
	return &_lv_types[lvt_enum];
}

#endif /* MAN_PAGE_GENERATOR */

struct command_name_args command_names_args[LVM_COMMAND_COUNT] = { { 0 } };

/* array of pointers into opt_names[] that is sorted alphabetically (by long opt name) */

static const struct opt_name *opt_names_alpha[ARG_COUNT + 1];

/* lvm_all is for recording options that are common for all lvm commands */

static struct command lvm_all;

/* saves OO_FOO lines (groups of optional options) to include in multiple defs */

static int _oo_line_count;
#define MAX_OO_LINES 256

struct oo_line {
	char *name;
	char *line;
};
static struct oo_line _oo_lines[MAX_OO_LINES];

#define REQUIRED 1  /* required option */
#define OPTIONAL 0  /* optional option */
#define IGNORE (-1)   /* ignore option */

#define MAX_LINE 1024
#define MAX_LINE_ARGC 256
#define DESC_LINE 1024

/*
 * Contains _command_input[] which is command-lines.in with comments
 * removed and wrapped as a string.  The _command_input[] string is
 * used to populate commands[].
 */
#include "command-lines-input.h"

static void _add_optional_opt_line(struct cmd_context *cmdtool, struct command *cmd, int argc, char *argv[]);

static unsigned _was_hyphen = 0;
static void printf_hyphen(char c)
{
	/* When .hy 1 was printed, we do not want to emit empty space */
	printf("%c%c\n", _was_hyphen ? '\n' : ' ', c);
	_was_hyphen = 0;
}

/*
 * modifies buf, replacing the sep characters with \0
 * argv pointers point to positions in buf
 */

static void _split_line(char *buf, int *argc, char **argv, char sep)
{
	char *p = buf;
	int i;

	argv[0] = p;

	for (i = 1; i < MAX_LINE_ARGC; i++) {
		p = strchr(p, sep);
		if (!p)
			break;
		*p++ = '\0';

		argv[i] = p;
	}
	*argc = i;
}

/* convert value string, e.g. Number, to foo_VAL enum */

static int _val_str_to_num(char *str)
{
	char name[MAX_LINE_ARGC];
	char *new;
	int i;

	/* compare the name before any suffix like _new or _<lvtype> */

	if (!_dm_strncpy(name, str, sizeof(name)))
		return 0; /* Buffer is too short */

	if ((new = strchr(name, '_')))
		*new = '\0';

	for (i = 0; i < VAL_COUNT; ++i)
		if (!strncmp(name, val_names[i].name, val_names[i].name_len))
			return val_names[i].val_enum;

	return 0;
}

/* convert "--option" to foo_ARG enum */

#define MAX_LONG_OPT_NAME_LEN 32

static int _opt_str_to_num(struct command *cmd, const char *str)
{
	char long_name[MAX_LONG_OPT_NAME_LEN];
	char *p = NULL;
	int i;
	int first = 0, last = ARG_COUNT - 1, middle;

	if (!_dm_strncpy(long_name, str, sizeof(long_name)))
		goto err;

	if ((p = strstr(long_name, "_long")))
		/*
		 * --foo_long means there are two args entries
		 * for --foo, one with a short option and one
		 * without, and we want the one without the
		 * short option (== 0).
		 */
		*p = '\0';

	/* Binary search in sorted array of long options (with duplicates) */
	while (first <= last) {
		middle = first + (last - first) / 2;
		if ((i = strcmp(opt_names_alpha[middle]->long_opt, long_name)) < 0)
			first = middle + 1;
		else if (i > 0)
			last = middle - 1;
		else {
			/* Matching long option string found.
			 * As sorted array contains duplicates, we need to also
			 * check left & right side for possible match
			 */
			for (i = middle;;) {
				if ((!p && !(opt_names_alpha[i]->flags & ARG_LONG_OPT)) ||
				    (p && !opt_names_alpha[i]->short_opt))
					return opt_names_alpha[i]->opt_enum; /* Found */
				/* Check if there is something on the 'left-side' */
				if ((i <= first) || strcmp(opt_names_alpha[--i]->long_opt, long_name))
					break;
			}

			/* Nothing on the left, so look on the 'right-side' */
			for (i = middle + 1; i <= last; ++i) {
				if (strcmp(opt_names_alpha[i]->long_opt, long_name))
					break;
				if ((!p && !(opt_names_alpha[i]->flags & ARG_LONG_OPT)) ||
				    (p && !opt_names_alpha[i]->short_opt))
					return opt_names_alpha[i]->opt_enum; /* Found */
			}

			break; /* Nothing... */
		}
	}
err:
	log_error("Parsing command defs: unknown opt str: \"%s\"%s%s.",
		  str, p ? " ": "", p ? long_name : "");
	cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;

	return ARG_UNUSED;
}

/* "foo" string to foo_CMD int */

unsigned command_id_to_enum(const char *str)
{
	int i;
	unsigned first = 1, last = CMD_COUNT - 1, middle;

	while (first <= last) {
		middle = first + (last - first) / 2;
		if ((i = strcmp(cmd_names[middle].name, str)) < 0)
			first = middle + 1;
		else if (i > 0)
			last = middle - 1;
		else
			return cmd_names[middle].cmd_enum;
	}

	log_error("Cannot find command %s.", str);
	return CMD_NONE;
}

const char *command_enum(unsigned command_enum)
{
	return cmd_names[command_enum].name;
}

/* "lv_is_prop" to is_prop_LVP */

static int _lvp_name_to_enum(struct command *cmd, const char *str)
{
	int i;

	for (i = 1; i < LVP_COUNT; i++) {
		if (!strcmp(str, _lv_props[i].name))
			return _lv_props[i].lvp_enum;
	}

	log_error("Parsing command defs: unknown lv property %s.", str);
	cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
	return LVP_NONE;
}

/* "type" to type_LVT */

static int _lvt_name_to_enum(struct command *cmd, const char *str)
{
	int i;

	for (i = 1; i < LVT_COUNT; i++) {
		if (!strcmp(str, _lv_types[i].name))
			return _lv_types[i].lvt_enum;
	}

	log_error("Parsing command defs: unknown lv type %s.", str);
	cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
	return LVT_NONE;
}

/* LV_<type> to <type>_LVT */

static int _lv_to_enum(struct command *cmd, const char *name)
{
	return _lvt_name_to_enum(cmd, name + 3);
}

/*
 * LV_<type1>_<type2> to lvt_bits
 *
 * type1 to lvt_enum
 * lvt_bits |= lvt_enum_to_bit(lvt_enum)
 * type2 to lvt_enum
 * lvt_bits |= lvt_enum_to_bit(lvt_enum)
 */

#define LVTYPE_LEN 128

static uint64_t _lv_to_bits(struct command *cmd, char *name)
{
	char buf[LVTYPE_LEN];
	char *argv[MAX_LINE_ARGC];
	uint64_t lvt_bits = 0;
	int lvt_enum;
	int argc;
	int i;

	dm_strncpy(buf, name, sizeof(buf));

	_split_line(buf, &argc, argv, '_');

	/* 0 is "LV" */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "new"))
			continue;
		lvt_enum = _lvt_name_to_enum(cmd, argv[i]);
		lvt_bits |= lvt_enum_to_bit(lvt_enum);
	}

	return lvt_bits;
}

static unsigned _find_lvm_command_enum(const char *name)
{
	int first = 0, last, middle;
	int i;

#ifdef MAN_PAGE_GENERATOR
	/* Validate cmd_names & command_names arrays are properly sorted */
	static int _command_names_count = -1;

	if (_command_names_count == -1) {
		for (i = 1; i < CMD_COUNT - 2; i++)
			if (strcmp(cmd_names[i].name, cmd_names[i + 1].name) > 0) {
				log_error("File cmds.h has unsorted name entry %s.",
					  cmd_names[i].name);
				return 0;
			}
		for (i = 1; i < LVM_COMMAND_COUNT; ++i) /* assume > 1 */
			if (strcmp(command_names[i - 1].name, command_names[i].name) > 0) {
				log_error("File commands.h has unsorted name entry %s.",
					  command_names[i].name);
				return 0;
			}
		_command_names_count = i - 1;
	}
#endif
	last = LVM_COMMAND_COUNT - 1;

	while (first <= last) {
		middle = first + (last - first) / 2;
		if ((i = strcmp(command_names[middle].name, name)) < 0)
			first = middle + 1;
		else if (i > 0)
			last = middle - 1;
		else
			return middle;
	}

	return LVM_COMMAND_COUNT;
}

const struct command_name *find_command_name(const char *name)
{
	unsigned r = _find_lvm_command_enum(name);

	return (r < LVM_COMMAND_COUNT) ? &command_names[r] : NULL;
}

static int _is_opt_name(char *str)
{
	if ((str[0] == '-') && (str[1] == '-'))
		return 1;

	if ((str[0] == '-') && (str[1] != '-'))
		log_error("Parsing command defs: options must be specified in long form: %s.", str);

	return 0;
}

/*
 * "Select" as a pos name means that the position
 * can be empty if the --select option is used.
 */

static int _is_pos_name(char *str)
{
	switch (str[0]) {
	case 'V': return (str[1] == 'G'); /* VG */
	case 'L': return (str[1] == 'V'); /* LV */
	case 'P': return (str[1] == 'V'); /* PV */
	case 'T': return (strncmp(str, "Tag", 3) == 0);
	case 'S': return ((strncmp(str, "String", 6) == 0) ||
			  (strncmp(str, "Select", 6) == 0));
	}

	return 0;
}

static int _is_oo_definition(char *str)
{
	if (!strncmp(str, "OO_", 3) && strchr(str, ':'))
		return 1;
	return 0;
}

static int _is_oo_line(char *str)
{
	if (!strncmp(str, "OO:", 3))
		return 1;
	return 0;
}

static int _is_io_line(char *str)
{
	if (!strncmp(str, "IO:", 3))
		return 1;
	return 0;
}

static int _is_op_line(char *str)
{
	if (!strncmp(str, "OP:", 3))
		return 1;
	return 0;
}

static int _is_desc_line(char *str)
{
	if (!strncmp(str, "DESC:", 5))
		return 1;
	return 0;
}

static int _is_autotype_line(char *str)
{
	if (!strncmp(str, "AUTOTYPE:", 6))
		return 1;
	return 0;
}

static int _is_flags_line(char *str)
{
	if (!strncmp(str, "FLAGS:", 6))
		return 1;
	return 0;
}

static int _is_rule_line(char *str)
{
	if (!strncmp(str, "RULE:", 5))
		return 1;
	return 0;
}

static int _is_id_line(char *str)
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

static void _set_pos_def(struct command *cmd, char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int val_enum;
	int i;

	_split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		val_enum = _val_str_to_num(name);

		if (!val_enum) {
			log_error("Parsing command defs: unknown pos arg: %s.", name);
			cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
			return;
		}

		def->val_bits |= val_enum_to_bit(val_enum);

		if ((val_enum == lv_VAL) && strchr(name, '_'))
			def->lvt_bits = _lv_to_bits(cmd, name);

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
static void _set_opt_def(struct cmd_context *cmdtool, struct command *cmd, char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int val_enum;
	int i;

	_split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		val_enum = _val_str_to_num(name);

		if (!val_enum) {
			/* a literal number or string */

			if (isdigit(name[0]))
				val_enum = constnum_VAL;

			else if (isalpha(name[0]))
				val_enum = conststr_VAL;

			else {
				log_error("Parsing command defs: unknown opt arg: %s.", name);
				cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
				return;
			}
		}


		def->val_bits |= val_enum_to_bit(val_enum);

		if (val_enum == constnum_VAL)
			def->num = (uint64_t)atoi(name);

		if (val_enum == conststr_VAL) {
#ifdef MAN_PAGE_GENERATOR
			free((void*)def->str);
#endif
			def->str = dm_pool_strdup(cmdtool->libmem, name);

			if (!def->str) {
				/* FIXME */
				stack;
				return;
			}
		}

		if (val_enum == lv_VAL) {
			if (strchr(name, '_'))
				def->lvt_bits = _lv_to_bits(cmd, name);
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

static void _add_oo_definition_line(const char *name, const char *line)
{
	struct oo_line *oo;
	char *colon;
	char *start;

	oo = &_oo_lines[_oo_line_count++];

	if (!(oo->name = strdup(name))) {
		log_error("Failed to duplicate name %s.", name);
		return; /* FIXME: return code */
	}

	if ((colon = strchr(oo->name, ':')))
		*colon = '\0';
	else {
		log_error("Parsing command defs: invalid OO definition.");
		return;
	}

	start = strchr(line, ':') + 2;
	if (!(oo->line = strdup(start))) {
		log_error("Failed to duplicate line %s.", start);
		return;
	}
}

/* Support OO_FOO: continuing on multiple lines. */

static void _append_oo_definition_line(const char *new_line)
{
	struct oo_line *oo;
	char *old_line;
	char *line;
	int len;

	oo = &_oo_lines[_oo_line_count - 1];

	old_line = oo->line;

	/* +2 = 1 space between old and new + 1 terminating \0 */
	len = strlen(old_line) + strlen(new_line) + 2;
	line = malloc(len);
	if (!line) {
		log_error("Parsing command defs: no memory.");
		return;
	}

	(void) dm_snprintf(line, len, "%s %s", old_line, new_line);
	free(oo->line);
	oo->line = line;
}

/* Find a saved OO_FOO definition. */

#define OO_NAME_LEN 128

static char *_get_oo_line(const char *str)
{
	char *name;
	char *end;
	char str2[OO_NAME_LEN];
	int i;

	dm_strncpy(str2, str, sizeof(str2));
	if ((end = strchr(str2, ':')))
		*end = '\0';
	if ((end = strchr(str2, ',')))
		*end = '\0';

	for (i = 0; i < _oo_line_count; i++) {
		name = _oo_lines[i].name;
		if (!strcmp(name, str2))
			return _oo_lines[i].line;
	}
	return NULL;
}

/*
 * Add optional_opt_args entries when OO_FOO appears on OO: line,
 * i.e. include common options from an OO_FOO definition.
 */

static void _include_optional_opt_args(struct cmd_context *cmdtool, struct command *cmd, const char *str)
{
	char *oo_line;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;

	if (!(oo_line = _get_oo_line(str))) {
		log_error("Parsing command defs: no OO line found for %s.", str);
		cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
		return;
	}

	if (!(line = strdup(oo_line))) {
		cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
		return;
	}

	_split_line(line, &line_argc, line_argv, ' ');
	_add_optional_opt_line(cmdtool, cmd, line_argc, line_argv);
	free(line);
}

/*
 * When an --option is seen, add a new opt_args entry for it.
 * This function sets the opt_args.opt value for it.
 */

static void _add_opt_arg(struct command *cmd, char *str,
			int *takes_arg, int *already, int required)
{
	char *comma;
	int opt;
	int i;

	/* opt_arg.opt set here */
	/* opt_arg.def will be set in _update_prev_opt_arg() if needed */

	if ((comma = strchr(str, ',')))
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

	opt = _opt_str_to_num(cmd, str);

	/* If the binary-search finds uuidstr_ARG switch to uuid_ARG */
	if (opt == uuidstr_ARG)
		opt = uuid_ARG;

	/* Skip adding an optional opt if it is already included. */
	if (already && !required) {
		for (i = 0; i < cmd->oo_count; i++) {
			if (cmd->optional_opt_args[i].opt == opt) {
				*already = 1;
				*takes_arg = opt_names[opt].val_enum ? 1 : 0;
				return;
			}
		}
	}

skip:
	if (required > 0) {
		if (cmd->ro_count >= CMD_RO_ARGS) {
			log_error("Too many args, increase CMD_RO_ARGS.");
			cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
			return;
		}
		cmd->required_opt_args[cmd->ro_count++].opt = opt;
	} else if (!required) {
		if (cmd->oo_count >= CMD_OO_ARGS) {
			log_error("Too many args, increase CMD_OO_ARGS.");
			cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
			return;
		}
		cmd->optional_opt_args[cmd->oo_count++].opt = opt;
	} else if (required < 0) {
		if (cmd->io_count >= CMD_IO_ARGS) {
			log_error("Too many args, increase CMD_IO_ARGS.");
			cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
			return;
		}
		cmd->ignore_opt_args[cmd->io_count++].opt = opt;
	}

	*takes_arg = opt_names[opt].val_enum ? 1 : 0;
}

/*
 * After --option has been seen, this function sets opt_args.def value
 * for the value that appears after --option.
 */

static void _update_prev_opt_arg(struct cmd_context *cmdtool, struct command *cmd, char *str, int required)
{
	struct arg_def def = { 0 };
	char *comma;

	if (str[0] == '-') {
		log_error("Parsing command defs: option %s must be followed by an arg.", str);
		cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
		return;
	}

	/* opt_arg.def set here */
	/* opt_arg.opt was previously set in _add_opt_arg() when --foo was read */

	if ((comma = strchr(str, ',')))
		*comma = '\0';

	_set_opt_def(cmdtool, cmd, str, &def);

	if (required > 0)
		cmd->required_opt_args[cmd->ro_count-1].def = def;
	else if (!required)
		cmd->optional_opt_args[cmd->oo_count-1].def = def;
	else if (required < 0)
		cmd->ignore_opt_args[cmd->io_count-1].def = def;
}

/*
 * When an position arg is seen, add a new pos_args entry for it.
 * This function sets the pos_args.pos and pos_args.def.
 */

static void _add_pos_arg(struct command *cmd, char *str, int required)
{
	struct arg_def def = { 0 };

	/* pos_arg.pos and pos_arg.def are set here */

	_set_pos_def(cmd, str, &def);

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

static void _update_prev_pos_arg(struct command *cmd, char *str, int required)
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
		log_error("Parsing command defs: unknown pos arg: %s.", str);
		cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
		return;
	}
}

/* Process what follows OO:, which are the optional opt args for the cmd def. */

static void _add_optional_opt_line(struct cmd_context *cmdtool, struct command *cmd, int argc, char *argv[])
{
	int takes_arg = 0;
	int already;
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OO:", 3))
			continue;

		already = 0;

		if (_is_opt_name(argv[i]))
			_add_opt_arg(cmd, argv[i], &takes_arg, &already, OPTIONAL);
		else if (!strncmp(argv[i], "OO_", 3))
			_include_optional_opt_args(cmdtool, cmd, argv[i]);
		else if (takes_arg)
			_update_prev_opt_arg(cmdtool, cmd, argv[i], OPTIONAL);
		else {
			log_error("Parsing command defs: can't parse argc %d argv %s%s%s.",
				i, argv[i], (i > 0) ? " prev " : "", (i > 0) ? argv[i - 1] : "");
			cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
			return;
		}

		if (already && takes_arg)
			i++;
	}
}

/* Process what follows IO:, which are the ignore options for the cmd def. */

static void _add_ignore_opt_line(struct cmd_context *cmdtool, struct command *cmd, int argc, char *argv[])
{
	int takes_arg = 0;
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "IO:", 3))
			continue;
		if (_is_opt_name(argv[i]))
			_add_opt_arg(cmd, argv[i], &takes_arg, NULL, IGNORE);
		else if (takes_arg)
			_update_prev_opt_arg(cmdtool, cmd, argv[i], IGNORE);
		else {
			log_error("Parsing command defs: can't parse argc %d argv %s%s%s.",
				  i, argv[i], (i > 0) ? " prev " : "", (i > 0) ? argv[i - 1] : "");
			cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
			return;
		}
	}
}

/* Process what follows OP:, which are optional pos args for the cmd def. */

static void _add_optional_pos_line(struct command *cmd, int argc, char *argv[])
{
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OP:", 3))
			continue;
		if (_is_pos_name(argv[i]))
			_add_pos_arg(cmd, argv[i], OPTIONAL);
		else
			_update_prev_pos_arg(cmd, argv[i], OPTIONAL);
	}
}

static void _add_required_opt_line(struct cmd_context *cmdtool, struct command *cmd, int argc, char *argv[])
{
	int takes_arg = 0;
	int i;

	for (i = 0; i < argc; i++) {
		if (_is_opt_name(argv[i]))
			_add_opt_arg(cmd, argv[i], &takes_arg, NULL, REQUIRED);
		else if (takes_arg)
			_update_prev_opt_arg(cmdtool, cmd, argv[i], REQUIRED);
		else {
			log_error("Parsing command defs: can't parse argc %d argv %s%s%s.",
				  i, argv[i], (i > 0) ? " prev " : "", (i > 0) ? argv[i - 1] : "");
			cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
			return;
		}
	}
}

/*
 * Add to required_opt_args from an OO_FOO definition.
 * (This is the special case of vgchange/lvchange where one
 * optional option is required, and others are then optional.)
 * The set of options from OO_FOO are saved in required_opt_args,
 * and flag CMD_FLAG_ANY_REQUIRED_OPT is set on the cmd indicating
 * this special case.
 */
static void _include_required_opt_args(struct cmd_context *cmdtool, struct command *cmd, char *str)
{
	char *oo_line;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;

	if (!(oo_line = _get_oo_line(str))) {
		log_error("Parsing command defs: no OO line found for %s.", str);
		cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
		return;
	}

	if (!(line = strdup(oo_line))) {
		cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
		return;
	}

	_split_line(line, &line_argc, line_argv, ' ');
	_add_required_opt_line(cmdtool, cmd, line_argc, line_argv);
	free(line);
}

/* Process what follows command_name, which are required opt/pos args. */

static void _add_required_line(struct cmd_context *cmdtool, struct command *cmd, int argc, char *argv[])
{
	int i;
	int takes_arg = 0;
	int prev_was_opt = 0, prev_was_pos = 0;
	int orig_ro_count = 0;

	/* argv[0] is command name */

	for (i = 1; i < argc; i++) {

		if (_is_opt_name(argv[i])) {
			/* add new required_opt_arg */
			_add_opt_arg(cmd, argv[i], &takes_arg, NULL, REQUIRED);
			prev_was_opt = 1;
			prev_was_pos = 0;

		} else if (prev_was_opt && takes_arg) {
			/* set value for previous required_opt_arg */
			_update_prev_opt_arg(cmdtool, cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 0;

		} else if (_is_pos_name(argv[i])) {
			/* add new required_pos_arg */
			_add_pos_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 1;

		} else if (!strncmp(argv[i], "OO_", 3)) {
			/*
			 * the first ro_count entries in required_opt_arg required,
			 * after which one or more of the next any_ro_count entries
			 * in required_opt_arg are required.  required_opt_arg
			 * has a total of ro_count+any_ro_count entries.
			 */
			cmd->cmd_flags |= CMD_FLAG_ANY_REQUIRED_OPT;
			orig_ro_count = cmd->ro_count;

			_include_required_opt_args(cmdtool, cmd, argv[i]);

			cmd->any_ro_count = cmd->ro_count - orig_ro_count;
			cmd->ro_count = orig_ro_count;

		} else if (prev_was_pos) {
			/* set property for previous required_pos_arg */
			_update_prev_pos_arg(cmd, argv[i], REQUIRED);
		} else {
			log_error("Parsing command defs: can't parse argc %d argv %s%s%s.",
				  i, argv[i], (i > 0) ? " prev " : "", (i > 0) ? argv[i - 1] : "");
			cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
			return;
		}
	}
}

static void _add_flags(struct command *cmd, const char *line)
{
	if (strstr(line, "SECONDARY_SYNTAX"))
		cmd->cmd_flags |= CMD_FLAG_SECONDARY_SYNTAX;
	if (strstr(line, "PREVIOUS_SYNTAX"))
		cmd->cmd_flags |= CMD_FLAG_PREVIOUS_SYNTAX;
}

static void _add_autotype(struct cmd_context *cmdtool, struct command *cmd,
			  int line_argc, char *line_argv[])
{
	if (cmd->autotype)
		cmd->autotype2 = dm_pool_strdup(cmdtool->libmem, line_argv[1]);
	else
		cmd->autotype = dm_pool_strdup(cmdtool->libmem, line_argv[1]);
}

static void _add_rule(struct cmd_context *cmdtool, struct command *cmd,
		      int line_argc, char *line_argv[])
{
	struct cmd_rule *rule;
	const char *arg;
	int i, lvt_enum, lvp_enum;
	int check = 0;

	if (cmd->rule_count == CMD_MAX_RULES) {
		log_error("Parsing command defs: too many rules for cmd, increase CMD_MAX_RULES.");
		cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
		return;
	}

	rule = &cmd->rules[cmd->rule_count++];

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
			if (rule->opts_count >= MAX_RULE_OPTS || rule->check_opts_count >= MAX_RULE_OPTS) {
				log_error("Parsing command defs: too many cmd_rule options for cmd, increase MAX_RULE_OPTS.");
				cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
				return;
			}

			if (check)
				rule->check_opts[rule->check_opts_count++] = _opt_str_to_num(cmd, arg);
			else
				rule->opts[rule->opts_count++] = _opt_str_to_num(cmd, arg);
		}

		else if (!strncmp(arg, "LV_", 3)) {
			lvt_enum = _lv_to_enum(cmd, arg);

			if (check)
				rule->check_lvt_bits |= lvt_enum_to_bit(lvt_enum);
			else
				rule->lvt_bits |= lvt_enum_to_bit(lvt_enum);
		}

		else if (!strncmp(arg, "lv_is_", 6)) {
			lvp_enum = _lvp_name_to_enum(cmd, arg);

			if (check)
				rule->check_lvp_bits |= lvp_enum_to_bit(lvp_enum);
			else
				rule->lvp_bits |= lvp_enum_to_bit(lvp_enum);
		}
	}
}

/* The given option is common to all lvm commands (set in lvm_all). */

static int _is_lvm_all_opt(int opt)
{
	int oo;

	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		if (lvm_all.optional_opt_args[oo].opt == opt)
			return 1;
	}
	return 0;
}

/* Find common options for all variants of each command name. */

void factor_common_options(void)
{
	int cn, opt_enum, ci, oo, ro, found;
	struct command *cmd;

	for (cn = 0; cn < LVM_COMMAND_COUNT; ++cn) {

		if (command_names_args[cn].variants)
			return; /* already factored */

		for (ci = 0; ci < COMMAND_COUNT; ci++) {
			cmd = &commands[ci];

			if (cmd->lvm_command_enum != command_names[cn].lvm_command_enum)
				continue;

			command_names_args[cn].variants++;

			if (cmd->ro_count || cmd->any_ro_count)
				command_names_args[cn].variant_has_ro = 1;
			if (cmd->rp_count)
				command_names_args[cn].variant_has_rp = 1;
			if (cmd->oo_count)
				command_names_args[cn].variant_has_oo = 1;
			if (cmd->op_count)
				command_names_args[cn].variant_has_op = 1;

			for (ro = 0; ro < cmd->ro_count + cmd->any_ro_count; ro++) {
				command_names_args[cn].all_options[cmd->required_opt_args[ro].opt] = 1;

				if ((cmd->required_opt_args[ro].opt == size_ARG) && !strncmp(cmd->name, "lv", 2))
					command_names_args[cn].all_options[extents_ARG] = 1;
			}
			for (oo = 0; oo < cmd->oo_count; oo++)
				command_names_args[cn].all_options[cmd->optional_opt_args[oo].opt] = 1;
		}

		for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {

			for (ci = 0; ci < COMMAND_COUNT; ci++) {
				cmd = &commands[ci];

				if (cmd->lvm_command_enum != command_names[cn].lvm_command_enum)
					continue;

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
			command_names_args[cn].common_options[opt_enum] = 1;
 next_opt:
			;
		}
	}
}

/* FIXME: use a flag in command_name struct? */

int command_has_alternate_extents(const struct command_name *cname)
{
	return (cname->flags & ALTERNATIVE_EXTENTS) ? 1 : 0;
}

static int _long_name_compare(const void *on1, const void *on2)
{
	const struct opt_name * const *optname1 = on1;
	const struct opt_name * const *optname2 = on2;

	return strcmp((*optname1)->long_opt + 2, (*optname2)->long_opt + 2);
}

/* Create list of option names for printing alphabetically. */

static void _create_opt_names_alpha(void)
{
	int i;

	for (i = 0; i < ARG_COUNT; i++)
		opt_names_alpha[i] = &opt_names[i];

	qsort(opt_names_alpha, ARG_COUNT, sizeof(long), _long_name_compare);
}

static int _copy_line(const char **line, size_t max_line, int *position)
{
	size_t len;

	*line = _command_input + *position;
	len = strlen(*line);
	*position += len + 1;
	if (len >= max_line)
		return 0;

	return len;
}

int define_commands(struct cmd_context *cmdtool, const char *run_name)
{
	struct command *cmd = NULL;
	const char *line_orig;
	char line[MAX_LINE];
	char *line_argv[MAX_LINE_ARGC];
	const char *name;
	size_t line_orig_len;
	int line_argc;
	int cmd_count = 0;
	int prev_was_oo_def = 0;
	int prev_was_oo = 0;
	int prev_was_op = 0;
	int copy_pos = 0;
	int skip = 0;
	int i;
	int lvm_command_enum;

	memset(&commands, 0, sizeof(commands));

	if (run_name && !strcmp(run_name, "help"))
		run_name = NULL;

	_create_opt_names_alpha();

	/* Process each line of command-lines-input.h (from command-lines.in) */

	while ((line_orig_len = _copy_line(&line_orig, MAX_LINE, &copy_pos)) > 0) {
		if (line_orig[0] == '-' && line_orig[1] == '-' &&
		    (!line_orig[2] || (line_orig[2] == '-' && !line_orig[3])))
			continue; /* "---"  or "--" */

		memcpy(line, line_orig, line_orig_len + 1);
		_split_line(line, &line_argc, line_argv, ' ');

		if (!line_argc)
			continue;

		/* New cmd def begins: command_name <required opt/pos args> */
		if (islower(line_argv[0][0]) && /* All commands are lower-case only */
		    ((lvm_command_enum = _find_lvm_command_enum(line_argv[0])) < LVM_COMMAND_COUNT)) {
			if (cmd_count >= COMMAND_COUNT)
				return 0;

			/*
			 * FIXME: when running one specific command name,
			 * we can optimize by not parsing command defs
			 * that don't start with that command name.
			 */
			cmd = &commands[cmd_count];
			cmd->command_index = cmd_count;
			cmd_count++;
			cmd->lvm_command_enum = lvm_command_enum;
			cmd->name = name = command_names[lvm_command_enum].name;

			if (run_name && strcmp(run_name, name)) {
				skip = 1;
				prev_was_oo_def = 0;
				prev_was_oo = 0;
				prev_was_op = 0;
				continue;
			}
			skip = 0;

			cmd->pos_count = 1;
			_add_required_line(cmdtool, cmd, line_argc, line_argv);

			/* Every cmd gets the OO_ALL options */
			_include_optional_opt_args(cmdtool, cmd, "OO_ALL:");
			continue;
		}

		/*
		 * All other kinds of lines are processed in the
		 * context of the existing command[].
		 */

		if (cmd && !skip && _is_desc_line(line_argv[0])) {
			if (cmd->desc) {
				size_t newlen = strlen(cmd->desc) + line_orig_len + 2;
				char *newdesc = dm_pool_alloc(cmdtool->libmem, newlen);

				if (!newdesc) {
					/* FIXME */
					stack;
					return 0;
				}

				snprintf(newdesc, newlen, "%s %s", cmd->desc, line_orig);
#ifdef MAN_PAGE_GENERATOR
				free((void*)cmd->desc);
#endif
				cmd->desc = newdesc;
			} else if (!(cmd->desc = dm_pool_strdup(cmdtool->libmem, line_orig))) {
				/* FIXME */
				stack;
				return 0;
			}

			continue;
		}

		if (cmd && !skip && _is_autotype_line(line_argv[0])) {
			_add_autotype(cmdtool, cmd, line_argc, line_argv);
			continue;
		}

		if (cmd && !skip && _is_flags_line(line_argv[0])) {
			_add_flags(cmd, line_orig);
			continue;
		}

		if (cmd && !skip && _is_rule_line(line_argv[0])) {
			_add_rule(cmdtool, cmd, line_argc, line_argv);
			continue;
		}

		if (cmd && _is_id_line(line_argv[0])) {
			cmd->command_enum = command_id_to_enum(line_argv[1]);
			if (cmd->command_enum == CMD_NONE) {
				cmd->cmd_flags |= CMD_FLAG_PARSE_ERROR;
				return 0;
			}
			continue;
		}

		/* OO_FOO: ... */
		if (_is_oo_definition(line_argv[0])) {
			_add_oo_definition_line(line_argv[0], line_orig);
			prev_was_oo_def = 1;
			prev_was_oo = 0;
			prev_was_op = 0;
			continue;
		}

		/* OO: ... */
		if (cmd && !skip && _is_oo_line(line_argv[0])) {
			_add_optional_opt_line(cmdtool, cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 1;
			prev_was_op = 0;
			continue;
		}

		/* OP: ... */
		if (cmd && !skip && _is_op_line(line_argv[0])) {
			_add_optional_pos_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 0;
			prev_was_op = 1;
			continue;
		}

		/* IO: ... */
		if (cmd && !skip && _is_io_line(line_argv[0])) {
			_add_ignore_opt_line(cmdtool, cmd, line_argc, line_argv);
			prev_was_oo = 0;
			prev_was_op = 0;
			continue;
		}

		/* handle OO_FOO:, OO:, OP: continuing on multiple lines */

		if (prev_was_oo_def) {
			_append_oo_definition_line(line_orig);
			continue;
		}

		if (prev_was_oo && cmd) {
			_add_optional_opt_line(cmdtool, cmd, line_argc, line_argv);
			continue;
		}

		if (prev_was_op && cmd) {
			_add_optional_pos_line(cmd, line_argc, line_argv);
			continue;
		}

		if (!skip) {
			log_error("Parsing command defs: can't process input line %s.", line_orig);
			return 0;
		}
	}

	for (i = 0; i < COMMAND_COUNT; i++) {
		if (commands[i].cmd_flags & CMD_FLAG_PARSE_ERROR)
			return 0;
	}

	_include_optional_opt_args(cmdtool, &lvm_all, "OO_ALL");

	for (i = 0; i < _oo_line_count; i++) {
		struct oo_line *oo = &_oo_lines[i];
		free(oo->name);
		free(oo->line);
	}
	memset(&_oo_lines, 0, sizeof(_oo_lines));
	_oo_line_count = 0;

	return 1;
}

#ifndef MAN_PAGE_GENERATOR
/*
 * The opt_names[] table describes each option.  It is indexed by the
 * option typedef, e.g. size_ARG.  The size_ARG entry specifies the
 * option name, e.g. --size, and the kind of value it accepts,
 * e.g. sizemb_VAL.
 *
 * The val_names[] table describes each option value type.  It is indexed by
 * the value typedef, e.g. sizemb_VAL.  The sizemb_VAL entry specifies the
 * function used to parse the value, e.g. size_mb_arg(), the string used to
 * refer to the value in the command-lines.in specifications, e.g. SizeMB,
 * and how the value should be displayed in a man page, e.g. Size[m|UNIT].
 *
 * A problem is that these tables are independent of a particular command
 * (they are created at build time), but different commands accept different
 * types of values for the same option, e.g. one command will accept
 * signed size values (ssizemb_VAL), while another does not accept a signed
 * number, (sizemb_VAL).
 *
 * To resolve the issue, at run time command 'reconfigures' its opt_names[]
 * values by querying particular arg_enum for particular command.
 * i.e. it changes size_ARG to accept sizemb_VAL or ssizemb_VAL depending
 * on the command.
 *
 * By default, size_ARG in opt_names[] is set up to accept a standard
 * sizemb_VAL.  The same is done for other opt_names[] entries that
 * take different option values.
 *
 * This function overrides default opt_names[] entries at run time according
 * to the command name, adjusting the value types accepted by various options.
 * So, for lvresize, opt_names[sizemb_VAL] is overridden to accept
 * the relative (+ or -) value type ssizemb_VAL, instead of the default
 * sizemb_VAL.  This way, when lvresize processes the --size value, it
 * will use the ssize_mb_arg() function which accepts relative size values.
 * When lvcreate processes the --size value, it uses size_mb_arg() which
 * rejects signed values.
 *
 * The command defs in commands[] do not need to be overridden because
 * the command-lines.in defs have the context of a command, and are
 * described using the proper value type, e.g. this cmd def already
 * uses the relative size value: "lvresize --size SSizeMB LV",
 * so the commands[] entry for the cmd def already references the
 * correct ssizemb_VAL.
 */
int configure_command_option_values(const struct command_name *cname, int arg_enum, int val_enum)
{
	switch (cname->lvm_command_enum) {
	case lvconvert_COMMAND:
		switch (arg_enum) {
		case mirrors_ARG:		return snumber_VAL;
		}
		break;
	case lvcreate_COMMAND:
		/*
		 * lvcreate is accepts also sizes with + (positive) value,
		 * so we have to recognize it.  But we don't want to show
		 * the + option in man/help as it can be seen confusing,
		 * so there's a special case when printing man/help
		 * output to show sizemb_VAL/extents_VAL rather than
		 * psizemb_VAL/pextents_VAL.)
		 */
		switch (arg_enum) {
		case size_ARG:			return psizemb_VAL;
		case extents_ARG:		return pextents_VAL;
		case poolmetadatasize_ARG:	return psizemb_VAL;
		case mirrors_ARG:		return pnumber_VAL;
		}
		break;
	case lvextend_COMMAND:
		/* relative + allowed */
		switch (arg_enum) {
		case size_ARG:			return psizemb_VAL;
		case extents_ARG:		return pextents_VAL;
		case poolmetadatasize_ARG:	return psizemb_VAL;
		}
		break;
	case lvreduce_COMMAND:
		/* relative - allowed */
		switch (arg_enum) {
		case size_ARG:			return nsizemb_VAL;
		case extents_ARG:		return nextents_VAL;
		}
		break;
	case lvresize_COMMAND:
		switch (arg_enum) {
		case size_ARG:			return ssizemb_VAL;
		case extents_ARG:		return sextents_VAL;
		case poolmetadatasize_ARG:	return psizemb_VAL;
		}
		break;
	}

	return val_enum;
}
#endif

/* type_LVT to "type" */

static void _print_usage_description(struct command *cmd)
{
	const char *desc = cmd->desc;
	char buf[MAX_LINE] = {0};
	unsigned di = 0;
	int bi = 0;

	for (di = 0; desc[di]; di++) {
		if (!strncmp(&desc[di], "DESC:", 5)) {
			if (bi) {
				buf[bi] = '\0';
				printf("  %s\n", buf);
				memset(buf, 0, sizeof(buf));
				bi = 0;
			}
			/* skip DESC: */
			di += 5;
			continue;
		}

		if (!bi && desc[di] == ' ')
			continue;

		if (desc[di] != '\\')
			buf[bi++] = desc[di];

		if (bi == (MAX_LINE - 1))
			break;
	}

	if (bi) {
		buf[bi] = '\0';
		printf("  %s\n", buf);
	}
}

/* Function remappas existing command definitions val_enums for printing
 * within man pages or command's help lines */
static int _update_relative_opt(const char *name, int opt_enum, int val_enum)
{
	/* check for relative sign */
	if (!strcmp(name, "lvconvert"))
		switch (opt_enum) {
		case mirrors_ARG:	return snumber_VAL;
		}
	else if (!strcmp(name, "lvcreate"))
		/*
		 * Suppress the [+] prefix for lvcreate which we have to
		 * accept for backwards compat, but don't want to advertise.
		 * 'command-lines.in' currently uses PNumber in definition
		 */
		switch (opt_enum) {
		case mirrors_ARG:	return number_VAL;
		}
	else if (!strcmp(name, "lvextend"))
		switch (opt_enum) {
		case extents_ARG:	return pextents_VAL;
		case poolmetadatasize_ARG:
		case size_ARG:		return psizemb_VAL;
		}
	else if (!strcmp(name, "lvreduce"))
		switch (opt_enum) {
		case extents_ARG:	return nextents_VAL;
		case size_ARG:		return nsizemb_VAL;
		}
	else if (!strcmp(name, "lvresize"))
		switch (opt_enum) {
		case extents_ARG:	return sextents_VAL;
		case poolmetadatasize_ARG:
					return psizemb_VAL;
		case size_ARG:		return ssizemb_VAL;
		}

	return val_enum;
}

static void _print_val_usage(struct command *cmd, int opt_enum, int val_enum)
{
	val_enum = _update_relative_opt(cmd->name, opt_enum, val_enum);

	if (!val_names[val_enum].usage)
		printf("%s", val_names[val_enum].name);
	else
		printf("%s", val_names[val_enum].usage);
}

static void _print_usage_def(struct command *cmd, int opt_enum, struct arg_def *def)
{
	int val_enum;
	int sep = 0;

	for (val_enum = 0; val_enum < VAL_COUNT; val_enum++) {
		if (def->val_bits & val_enum_to_bit(val_enum)) {

			if (val_enum == conststr_VAL)
				printf("%s", def->str);

			else if (val_enum == constnum_VAL)
				printf("%llu", (unsigned long long)def->num);

			else {
				if (sep) printf("|");
				_print_val_usage(cmd, opt_enum, val_enum);
				sep = 1;
			}

			/* Too many types have made this too long.  man page has this info. */
			/*
			if (val_enum == lv_VAL && def->lvt_bits) {
				int lvt_enum;
				for (lvt_enum = 1; lvt_enum < LVT_COUNT; lvt_enum++) {
					if (lvt_bit_is_set(def->lvt_bits, lvt_enum))
						printf("_%s", _lvt_enum_to_name(lvt_enum));
				}
			}
			*/

			if ((val_enum == vg_VAL) && (def->flags & ARG_DEF_FLAG_NEW_VG))
				printf("_new");
			if ((val_enum == lv_VAL) && (def->flags & ARG_DEF_FLAG_NEW_LV))
				printf("_new");
		}
	}

	if (def->flags & ARG_DEF_FLAG_MAY_REPEAT)
		printf(" ...");
}

void print_usage(struct command *cmd, int longhelp, int desc_first)
{
	const struct command_name *cname = &command_names[cmd->lvm_command_enum];
	const struct command_name_args *cna = &command_names_args[cname->lvm_command_enum];
	int any_req = (cmd->cmd_flags & CMD_FLAG_ANY_REQUIRED_OPT) ? 1 : 0;
	int include_extents = 0;
	int ro, rp, oo, op, opt_enum, first;

	/*
	 * Looks at all variants of each command name and figures out
	 * which options are common to all variants (for compact output)
	 */
	factor_common_options();

	if (desc_first && cmd->desc)
		_print_usage_description(cmd);

	printf("  %s", cmd->name);

	if (any_req) {
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if (opt_names[opt_enum].short_opt)
				printf(" -%c|%s", opt_names[opt_enum].short_opt, opt_names[opt_enum].long_opt);
			else
				printf(" %s", opt_names[opt_enum].long_opt);

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, opt_enum, &cmd->required_opt_args[ro].def);
			}
		}

		/* one required option in a set */
		first = 1;

		/* options with short and long */
		for (ro = cmd->ro_count; ro < cmd->ro_count + cmd->any_ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if (!opt_names[opt_enum].short_opt)
				continue;

			if (first)
				printf("\n\t(");
			else
				printf(",\n\t ");
			first = 0;

			printf(" -%c|%s", opt_names[opt_enum].short_opt, opt_names[opt_enum].long_opt);

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, opt_enum, &cmd->required_opt_args[ro].def);
			}
		}

		/* options with only long */
		for (ro = cmd->ro_count; ro < cmd->ro_count + cmd->any_ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if (opt_names[opt_enum].short_opt)
				continue;

			if ((opt_enum == size_ARG) && command_has_alternate_extents(cname))
				include_extents = 1;

			if (first)
				printf("\n\t(");
			else
				printf(",\n\t ");
			first = 0;

			printf("    %s", opt_names[opt_enum].long_opt);

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, opt_enum, &cmd->required_opt_args[ro].def);
			}
		}

		printf_hyphen(')');
	} else  /* !any_req */
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if ((opt_enum == size_ARG) && command_has_alternate_extents(cname))
				include_extents = 1;

			if (opt_names[opt_enum].short_opt)
				printf(" -%c|%s", opt_names[opt_enum].short_opt, opt_names[opt_enum].long_opt);
			else
				printf(" %s", opt_names[opt_enum].long_opt);

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, opt_enum, &cmd->required_opt_args[ro].def);
			}
		}

	if (cmd->rp_count) {
		if (any_req)
			printf("\t");
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, 0, &cmd->required_pos_args[rp].def);
			}
		}
	}

	if (!longhelp)
		goto done;

	if (cmd->oo_count) {
		if (cmd->autotype) {
			printf("\n\t");
			if (!cmd->autotype2)
				printf("[ --type %s ] (implied)", cmd->autotype);
			else
				printf("[ --type %s|%s ] (implied)", cmd->autotype, cmd->autotype2);
		}

		if (include_extents) {
			printf("\n\t[ -l|--extents ");
			_print_val_usage(cmd, extents_ARG, opt_names[extents_ARG].val_enum);
			printf(" ]");
		}

		/* print optional options with short opts */

		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			if (!opt_names[opt_enum].short_opt)
				continue;

			/*
			 * Skip common lvm options in lvm_all which
			 * are printed at the end under "Common options for lvm"
			 * see print_common_options_lvm()
			 */

			if (_is_lvm_all_opt(opt_enum))
				continue;

			/*
			 * When there is more than one variant,
			 * skip common command options from
			 * cname->common_options (options common
			 * to all variants), which are printed at
			 * the end under "Common options for command"
			 * see print_common_options_cmd()
			 */

			if (cna && (cna->variants > 1) && cna->common_options[opt_enum])
				continue;

			printf("\n\t[");

			printf(" -%c|%s", opt_names[opt_enum].short_opt, opt_names[opt_enum].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, opt_enum, &cmd->optional_opt_args[oo].def);
			}

			printf(" ]");
		}

		/* print optional options without short opts */

		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			if (opt_names[opt_enum].short_opt)
				continue;

			/*
			 * Skip common lvm options in lvm_all which
			 * are printed at the end under "Common options for lvm"
			 * see print_common_options_lvm()
			 */

			if (_is_lvm_all_opt(opt_enum))
				continue;

			/*
			 * When there is more than one variant,
			 * skip common command options from
			 * cname->common_options (options common
			 * to all variants), which are printed at
			 * the end under "Common options for command"
			 * see print_common_options_cmd()
			 */

			if (cna && (cna->variants > 1) && cna->common_options[opt_enum])
				continue;

			printf("\n\t[");

			printf("    %s", opt_names[opt_enum].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, opt_enum, &cmd->optional_opt_args[oo].def);
			}

			printf(" ]");
		}

		printf("\n\t[ COMMON_OPTIONS ]");
	}

	if (cmd->op_count) {
		printf("\n\t[");

		for (op = 0; op < cmd->op_count; op++)
			if (cmd->optional_pos_args[op].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, 0, &cmd->optional_pos_args[op].def);
			}

		printf(" ]");
	}
 done:
	printf("\n");

	if (!desc_first && cmd->desc)
		_print_usage_description(cmd);

	printf("\n");
}

void print_usage_common_lvm(const struct command_name *cname, struct command *cmd)
{
	int oo, opt_enum;

	printf("  Common options for lvm:");

	/* print options with short opts */

	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		opt_enum = lvm_all.optional_opt_args[oo].opt;

		if (!opt_names[opt_enum].short_opt)
			continue;

		printf("\n\t[");

		printf(" -%c|%s", opt_names[opt_enum].short_opt, opt_names[opt_enum].long_opt);
		if (lvm_all.optional_opt_args[oo].def.val_bits) {
			printf(" ");
			_print_usage_def(cmd, opt_enum, &lvm_all.optional_opt_args[oo].def);
		}
		printf(" ]");
	}

	/* print options without short opts */

	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		opt_enum = lvm_all.optional_opt_args[oo].opt;

		if (opt_names[opt_enum].short_opt)
			continue;

		printf("\n\t[");

		printf("    %s", opt_names[opt_enum].long_opt);
		if (lvm_all.optional_opt_args[oo].def.val_bits) {
			printf(" ");
			_print_usage_def(cmd, opt_enum, &lvm_all.optional_opt_args[oo].def);
		}
		printf(" ]");
	}

	printf("\n\n");
}

void print_usage_common_cmd(const struct command_name *cname, struct command *cmd)
{
	const struct command_name_args *cna = &command_names_args[cname->lvm_command_enum];
	int oo, opt_enum;
	int found_common_command = 0;

	/*
	 * when there's more than one variant, options that
	 * are common to all commands with a common name.
	 */

	if (cna->variants < 2)
		return;

	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cna->common_options[opt_enum])
			continue;
		if (_is_lvm_all_opt(opt_enum))
			continue;
		found_common_command = 1;
		break;
	}

	if (!found_common_command)
		return;

	printf("  Common options for command:");

	/* print options with short opts */

	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cna->common_options[opt_enum])
			continue;

		if (_is_lvm_all_opt(opt_enum))
			continue;

		if (!opt_names[opt_enum].short_opt)
			continue;

		printf("\n\t[");

		for (oo = 0; oo < cmd->oo_count; oo++) {
			if (cmd->optional_opt_args[oo].opt != opt_enum)
				continue;

			printf(" -%c|%s", opt_names[opt_enum].short_opt, opt_names[opt_enum].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, opt_enum, &cmd->optional_opt_args[oo].def);
			}
			break;
		}
		printf(" ]");
	}

	/* print options without short opts */

	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cna->common_options[opt_enum])
			continue;

		if (_is_lvm_all_opt(opt_enum))
			continue;

		if (opt_names[opt_enum].short_opt)
			continue;

		printf("\n\t[");

		for (oo = 0; oo < cmd->oo_count; oo++) {
			if (cmd->optional_opt_args[oo].opt != opt_enum)
				continue;

			printf("    %s", opt_names[opt_enum].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				_print_usage_def(cmd, opt_enum, &cmd->optional_opt_args[oo].def);
			}
			break;
		}
		printf(" ]");
	}

	printf("\n\n");
}

void print_usage_notes(const struct command_name *cname)
{
	if (cname && command_has_alternate_extents(cname))
		printf("  Special options for command:\n\t"
		       "[ --extents Number[PERCENT] ]\n\t"
		       "The --extents option can be used in place of --size.\n\t"
		       "The number allows an optional percent suffix.\n"
		       "\n\t");

	if (cname && !strcmp(cname->name, "lvcreate"))
		printf("[ --name String ]\n\t"
		       "The --name option is not required but is typically used.\n\t"
		       "When a name is not specified, a new LV name is generated\n\t"
		       "with the \"lvol\" prefix and a unique numeric suffix.\n"
		       "\n");

	printf("  Common variables for lvm:\n\t"
	       "Variables in option or position args are capitalized,\n\t"
	       "e.g. PV, VG, LV, Size, Number, String, Tag.\n"
	       "\n\t"

	       "PV\n\t"
	       "Physical Volume name, a device path under /dev.\n\t"
	       "For commands managing physical extents, a PV positional arg\n\t"
	       "generally accepts a suffix indicating a range (or multiple ranges)\n\t"
	       "of PEs. When the first PE is omitted, it defaults to the start of\n\t"
	       "the device, and when the last PE is omitted it defaults to the end.\n\t"
	       "PV[:PE-PE]... is start and end range (inclusive),\n\t"
	       "PV[:PE+PE]... is start and length range (counting from 0).\n"
	       "\n\t"

	       "LV\n\t"
	       "Logical Volume name. See lvm(8) for valid names. An LV positional\n\t"
	       "arg generally includes the VG name and LV name, e.g. VG/LV.\n\t"
	       "LV followed by _<type> indicates that an LV of the given type is\n\t"
	       "required. (raid represents raid<N> type).\n\t"
	       "The _new suffix indicates that the LV name is new.\n"
	       "\n\t"

	       "Tag\n\t"
	       "Tag name. See lvm(8) for information about tag names and using\n\t"
	       "tags in place of a VG, LV or PV.\n"
	       "\n\t"

	       "Select\n\t"
	       "Select indicates that a required positional arg can be omitted\n\t"
	       "if the --select option is used. No arg appears in this position.\n"
	       "\n\t"

	       "Size[UNIT]\n\t"
	       "Size is an input number that accepts an optional unit.\n\t"
	       "Input units are always treated as base two values, regardless of\n\t"
	       "capitalization, e.g. 'k' and 'K' both refer to 1024.\n\t"
	       "The default input unit is specified by letter, followed by |UNIT.\n\t"
	       "UNIT represents other possible input units: BbBsSkKmMgGtTpPeE.\n\t"
	       "(This should not be confused with the output control --units, where\n\t"
	       "capital letters mean multiple of 1000.)\n"
	       "\n");
}
