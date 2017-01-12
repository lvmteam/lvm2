
#ifndef __CCMD_H__
#define __CCMD_H__

/* needed to include args.h */
#define ARG_COUNTABLE 0x00000001
#define ARG_GROUPABLE 0x00000002
struct cmd_context;
struct arg_values;

/* needed to include args.h */
static inline int yes_no_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int activation_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int cachemode_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int discards_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int mirrorlog_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_kb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_mb_arg_with_percent(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int uint32_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg_with_sign(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg_with_sign_and_percent(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int major_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int minor_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int string_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int tag_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int permission_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int metadatatype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int units_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int segtype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int alloc_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int locktype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int readahead_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int vgmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int pvmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int metadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int polloperation_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int writemostly_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int syncaction_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int reportformat_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int configreport_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int configtype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }

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
	const char *desc;
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
#define arg(a, b, c, d, e, f, g) a ,
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

/* one for each command name, see cmd_names[] */

#define MAX_CMD_NAMES 128
struct cmd_name {
	const char *name;
	const char *desc;
	int common_options[ARG_COUNT + 1]; /* options common to all defs */
	int all_options[ARG_COUNT + 1];    /* union of options from all defs */
	int variants;        /* number of command defs with this command name */
	int variant_has_ro;  /* do variants use required_opt_args ? */
	int variant_has_rp;  /* do variants use required_pos_args ? */
	int variant_has_oo;  /* do variants use optional_opt_args ? */
	int variant_has_op;  /* do variants use optional_pos_args ? */
};

/* struct command */

#include "command.h"

/* one for each command defininition (command-lines.in as struct command) */

#define MAX_CMDS 256
int cmd_count;
struct command cmd_array[MAX_CMDS];

#define MAX_LINE 1024
#define MAX_LINE_ARGC 256

#define DESC_LINE 256

#endif

