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

#include "ccmd.h"

struct cmd_name *find_command_name(const char *str);
int is_lvm_all_opt(int opt);

extern struct val_name val_names[VAL_COUNT + 1];
extern struct opt_name opt_names[ARG_COUNT + 1];
extern struct lvp_name lvp_names[LVP_COUNT + 1];
extern struct lvt_name lvt_names[LVT_COUNT + 1];
extern struct cmd_name cmd_names[MAX_CMD_NAMES];
extern struct command lvm_all;

/*
 * modifies buf, replacing the sep characters with \0
 * argv pointers point to positions in buf
 */

char *split_line(char *buf, int *argc, char **argv, char sep)
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

int val_str_to_num(char *str)
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

int opt_str_to_num(char *str)
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

/* "lv_is_prop" to is_prop_LVP */

int lvp_name_to_enum(char *str)
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

const char *lvt_enum_to_name(int lvt_enum)
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

/* output for struct/count */

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

static void print_usage_common(struct command *cmd)
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

static void print_usage(struct command *cmd)
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

void print_header_struct(void)
{
	struct command *cmd;
	int i, j, ro, rp, oo, op, ru, ruo, io;

	printf("/* Do not edit. This file is generated by tools/create-commands */\n");
	printf("/* using command definitions from tools/command-lines.in */\n");
	printf("\n");

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];

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

void print_header_count(void)
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

struct cmd_pair {
	int i, j;
};

void print_ambiguous(void)
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

