/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
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

#ifndef _LVM_COMMAND_H
#define _LVM_COMMAND_H

struct cmd_context;
struct logical_volume;

/* old per-command-name function */
typedef int (*command_fn) (struct cmd_context *cmd, int argc, char **argv);

struct command_function {
	int command_enum;
	command_fn fn; /* new style */
};

struct command_name {
	const char *name;
	const char *desc; /* general command description from commands.h */
	unsigned int flags;
	command_fn fn; /* old style */
	uint16_t lvm_command_enum; /* as declared in commands.h with _COMMAND */
};

struct command_name_args {
	uint16_t num_args;
	uint16_t variants;        /* number of command defs with this command name */
	/* union of {required,optional}_opt_args for all commands with this name */
	uint16_t valid_args[ARG_COUNT]; /* used for getopt, store option */

	/* the following are for generating help and man page output */
	uint8_t common_options[ARG_COUNT]; /* options common to all defs  0/1 */
	uint8_t all_options[ARG_COUNT];    /* union of options from all defs 0/1 */
	uint8_t variant_has_ro;  /* do variants use required_opt_args ? */
	uint8_t variant_has_rp;  /* do variants use required_pos_args ? */
	uint8_t variant_has_oo;  /* do variants use optional_opt_args ? */
	uint8_t variant_has_op;  /* do variants use optional_pos_args ? */
};

/*
 * Command definition
 *
 * A command is defined in terms of a command name,
 * required options (+args), optional options (+args),
 * required positional args, optional positional args.
 *
 * A positional arg always has non-zero pos_arg.def.types.
 * The first positional arg has pos_arg.pos of 1.
 */

/* arg_def flags */
#define ARG_DEF_FLAG_NEW_VG             1 << 0
#define ARG_DEF_FLAG_NEW_LV             1 << 1
#define ARG_DEF_FLAG_MAY_REPEAT         1 << 2

static inline int val_bit_is_set(uint64_t val_bits, int val_enum)
{
	return (val_bits & (1ULL << val_enum)) ? 1 : 0;
}

static inline uint64_t val_enum_to_bit(int val_enum)
{
	return (1ULL << val_enum);
}

static inline int lvp_bit_is_set(uint64_t lvp_bits, int lvp_enum)
{
	return (lvp_bits & (1ULL << lvp_enum)) ? 1 : 0;
}

static inline uint64_t lvp_enum_to_bit(int lvp_enum)
{
	return (1ULL << lvp_enum);
}

static inline int lvt_bit_is_set(uint64_t lvt_bits, int lvt_enum)
{
	return (lvt_bits & (1ULL << lvt_enum)) ? 1 : 0;
}

static inline uint64_t lvt_enum_to_bit(int lvt_enum)
{
	return (1ULL << lvt_enum);
}

/* Description a value that follows an option or exists in a position. */

struct arg_def {
	uint64_t val_bits;   /* bits of x_VAL, can be multiple for pos_arg */
	uint64_t lvt_bits;   /* lvt_enum_to_bit(x_LVT) for lv_VAL, can be multiple */
	const char *str;     /* a literal string for constnum_VAL */
	uint32_t flags;      /* ARG_DEF_FLAG_ */
	uint16_t num;        /* a literal number for conststr_VAL */
};

/* Description of an option and the value that follows it. */

struct opt_arg {
	int opt;             /* option, e.g. foo_ARG */
	struct arg_def def;  /* defines accepted values */
};

/* Description of a position and the value that exists there. */

struct pos_arg {
	int pos;             /* position, e.g. first is 1 */
	struct arg_def def;  /* defines accepted values */
};

/*
 * Commands using a given command definition must follow a set
 * of rules.  If a given command+LV matches the conditions in
 * opts/lvt_bits/lvp_bits, then the checks are applied.
 * If one condition is not met, the checks are not applied.
 * If no conditions are set, the checks are always applied.
 */

#define RULE_INVALID 1
#define RULE_REQUIRE 2
#define MAX_RULE_OPTS 8

struct cmd_rule {
	uint64_t lvt_bits;		/* if LV has one of these types (lvt_enum_to_bit), the check may apply */
	uint64_t lvp_bits;		/* if LV has all of these properties (lvp_enum_to_bit), the check may apply */

	uint64_t check_lvt_bits;	/* LV must [not] have one of these type */
	uint64_t check_lvp_bits;	/* LV must [not] have all of these properties */

	uint16_t opts[MAX_RULE_OPTS];	/* if any option in this list is set, the check may apply */
	uint16_t check_opts[MAX_RULE_OPTS];/* used options must [not] be in this list */

	uint16_t rule;			/* RULE_INVALID, RULE_REQUIRE: check values must [not] be true */
	uint16_t opts_count;		/* entries in opts[] */
	uint16_t check_opts_count;	/* entries in check_opts[] */
};

/*
 * Array sizes
 *
 * CMD_RO_ARGS needs to accommodate a list of options,
 * of which one is required after which the rest are
 * optional.
 */
#define CMD_RO_ARGS 32          /* required opt args */
#define CMD_OO_ARGS 56          /* optional opt args */
#define CMD_RP_ARGS 8           /* required positional args */
#define CMD_OP_ARGS 8           /* optional positional args */
#define CMD_IO_ARGS 8           /* ignore opt args */
#define CMD_MAX_RULES 12        /* max number of rules per command def */

/*
 * one or more from required_opt_args is required,
 * then the rest are optional.
 *
 * CMD_FLAG_ANY_REQUIRED_OPT: for lvchange/vgchange special case.
 * The first ro_count entries of required_opt_args must be met
 * (ro_count may be 0.)  After this, one or more options must be
 * set from the remaining required_opt_args.  So, the first
 * ro_count options in required_opt_args must match, and after
 * that one or more of the remaining options in required_opt_args
 * must match.
 */
#define CMD_FLAG_ANY_REQUIRED_OPT   1
#define CMD_FLAG_SECONDARY_SYNTAX   2  /* allows syntax variants to be suppressed in certain output */
#define CMD_FLAG_PREVIOUS_SYNTAX    4  /* allows syntax variants to not be advertised in output */
#define CMD_FLAG_PARSE_ERROR        8  /* error parsing command-lines.in def */

/* a register of the lvm commands */
struct command {
	const char *name;
	const char *desc; /* specific command description from command-lines.in */
	uint16_t command_enum; /* <command_id>_CMD */
	uint16_t command_index; /* position in commands[] */

	uint16_t lvm_command_enum; /* position in command_names[] */
	uint16_t cmd_flags; /* CMD_FLAG_ */

	/* definitions of opt/pos args */

	/* required args following an --opt */
	struct opt_arg required_opt_args[CMD_RO_ARGS];

	/* optional args following an --opt */
	struct opt_arg optional_opt_args[CMD_OO_ARGS];

	/* required positional args */
	struct pos_arg required_pos_args[CMD_RP_ARGS];

	/* optional positional args */
	struct pos_arg optional_pos_args[CMD_OP_ARGS];

	/* unused opt args, are ignored instead of causing an error */
	struct opt_arg ignore_opt_args[CMD_IO_ARGS];

	struct cmd_rule rules[CMD_MAX_RULES];

	/* usually only one autotype, in one case there are two */
	char *autotype;
	char *autotype2;

	uint16_t any_ro_count;

	uint16_t ro_count;
	uint16_t oo_count;
	uint16_t rp_count;
	uint16_t op_count;
	uint16_t io_count;
	uint16_t rule_count;

	uint16_t pos_count; /* temp counter used by create-command */
};

/* see global opt_names[] */

struct opt_name {
	const char *desc;
	const char long_opt[27];/* --foo */
	const char short_opt;   /* -f */
	uint16_t opt_enum;      /* foo_ARG */
	uint16_t val_enum;	/* xyz_VAL when --foo takes a val like "--foo xyz" */
	uint16_t flags;
	uint16_t prio;
};

/* see global val_names[] */

struct val_name {
	int (*fn) (struct cmd_context *cmd, struct arg_values *av); /* foo_arg() */
	const char *usage;
	const char name[30];    /* FooVal */
	uint16_t name_len;      /* sizeof(FooVal) - 1 */
	uint16_t val_enum;      /* foo_VAL */
};

/* see global lv_props[] */

struct lv_prop {
	const char name[30];   /* "lv_is_foo" */
	uint16_t lvp_enum;     /* is_foo_LVP */
};

/* see global lv_types[] */

struct lv_type {
	const char name[30];   /* "foo" */
	uint16_t lvt_enum;     /* foo_LVT */
};


int define_commands(struct cmd_context *cmdtool, const char *run_name);
unsigned command_id_to_enum(const char *str);
const char *command_enum(unsigned command_enum);
void print_usage(struct command *cmd, int longhelp, int desc_first);
void print_usage_common_cmd(const struct command_name *cname, struct command *cmd);
void print_usage_common_lvm(const struct command_name *cname, struct command *cmd);
void print_usage_notes(const struct command_name *cname);
void factor_common_options(void);
int command_has_alternate_extents(const struct command_name *cname);
int configure_command_option_values(const struct command_name *cname, int arg_enum, int val_enum);
const struct command_name *find_command_name(const char *name);

#endif
