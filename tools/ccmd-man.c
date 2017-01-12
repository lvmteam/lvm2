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

char *split_line(char *buf, int *argc, char **argv, char sep);
struct cmd_name *find_command_name(const char *str);
int is_lvm_all_opt(int opt);
const char *lvt_enum_to_name(int lvt_enum);

extern struct val_name val_names[VAL_COUNT + 1];
extern struct opt_name opt_names[ARG_COUNT + 1];
extern struct lvp_name lvp_names[LVP_COUNT + 1];
extern struct lvt_name lvt_names[LVT_COUNT + 1];
extern struct cmd_name cmd_names[MAX_CMD_NAMES];
extern struct opt_name *opt_names_alpha[ARG_COUNT + 1];
extern struct command lvm_all;

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

/*
 * common options listed in the usage section.
 *
 * For commands with only one variant, this is only
 * the options which are common to all lvm commands
 * (in lvm_all, see is_lvm_all_opt).
 *
 * For commands with more than one variant, this
 * is the set of options common to all variants
 * (in cname->common_options), (which obviously
 * includes the options common to all lvm commands.)
 *
 * List ordering:
 * options with short+long names, alphabetically,
 * then options with only long names, alphabetically
 */

void print_man_usage_common(struct command *cmd)
{
	struct cmd_name *cname;
	int i, sep, ro, rp, oo, op, opt_enum;

	if (!(cname = find_command_name(cmd->name)))
		return;

	sep = 0;

	printf(".RS 4\n");
	printf("[");

	/* print those with short opts */
	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cname->common_options[opt_enum])
			continue;

		if (!opt_names[opt_enum].short_opt)
			continue;

		if ((cname->variants < 2) && !is_lvm_all_opt(opt_enum))
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
	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cname->common_options[opt_enum])
			continue;

		if (opt_names[opt_enum].short_opt)
			continue;

		if ((cname->variants < 2) && !is_lvm_all_opt(opt_enum))
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

	printf(" ]\n");
	return;
}

/*
 * Format of description, when different command names have
 * different descriptions:
 *
 * "#cmdname1"
 * "text foo goes here"
 * "a second line of text."
 * "#cmdname2"
 * "text bar goes here"
 * "another line of text."
 *
 * When called for cmdname2, this function should just print:
 *
 * "text bar goes here"
 * "another line of text."
 */

static void print_man_option_desc(struct cmd_name *cname, int opt_enum)
{
	const char *desc = opt_names[opt_enum].desc;
	char buf[DESC_LINE];
	int started_cname = 0;
	int line_count = 0;
	int di, bi = 0;

	if (desc[0] != '#') {
		printf("%s", desc);
		return;
	}

	for (di = 0; di < strlen(desc); di++) {
		buf[bi++] = desc[di];

		if (bi == DESC_LINE) {
			printf("print_man_option_desc line too long\n");
			return;
		}

		if (buf[bi-1] != '\n')
			continue;

		if (buf[0] != '#') {
			if (started_cname) {
				printf("%s", buf);
				line_count++;
			}

			memset(buf, 0, sizeof(buf));
			bi = 0;
			continue;
		}

		/* Line starting with #cmdname */

		/*
		 * Must be starting a new command name.
		 * If no lines have been printed, multiple command names
		 * are using the same text. If lines have been printed,
		 * then the start of a new command name means the end
		 * of text for the current command name.
		 */
		if (line_count && started_cname)
			return;

		if (!strncmp(buf + 1, cname->name, strlen(cname->name))) {
			/* The start of our command name. */
			started_cname = 1;
			memset(buf, 0, sizeof(buf));
			bi = 0;
		} else {
			/* The start of another command name. */
			memset(buf, 0, sizeof(buf));
			bi = 0;
		}
	}

	if (bi && started_cname)
		printf("%s", buf);
}

/*
 * Print a list of all options names for a given
 * command name, listed by:
 * options with short+long names, alphabetically,
 * then options with only long names, alphabetically
 */

void print_man_all_options_list(struct cmd_name *cname)
{
	int opt_enum, val_enum;
	int sep = 0;
	int i;

	/* print those with both short and long opts */
	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;


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
	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

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

/*
 * All options used for a given command name, along with descriptions.
 * listed in order of:
 * 1. options that are not common to all lvm commands, alphabetically
 * 2. options common to all lvm commands, alphabetically
 */

void print_man_all_options_desc(struct cmd_name *cname)
{
	int opt_enum, val_enum;
	int print_common = 0;
	int sep = 0;
	int i;

 again:
	/*
	 * Loop 1: print options that are not common to all lvm commands.
	 * Loop 2: print options common to all lvm commands (lvm_all)
	 */

	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cname->all_options[opt_enum])
			continue;

		if (!print_common && is_lvm_all_opt(opt_enum))
			continue;

		if (print_common && !is_lvm_all_opt(opt_enum))
			continue;

		if (sep)
			printf("\n.br\n");

		printf("\n.TP\n");

		if (opt_names[opt_enum].short_opt) {
			printf("\\fB-%c\\fP|\\fB%s\\fP",
			       opt_names[opt_enum].short_opt,
			       man_long_opt_name(cname->name, opt_enum));
		} else {
			printf("\\fB%s\\fP", man_long_opt_name(cname->name, opt_enum));
		}

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

		if (opt_names[opt_enum].desc) {
			printf("\n");
			printf(".br\n");
			print_man_option_desc(cname, opt_enum);
		}

		sep = 1;
	}

	if (!print_common) {
		print_common = 1;
		goto again;
	}
}

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

void print_man(char *man_command_name, int include_primary, int include_secondary)
{
	struct cmd_name *cname;
	struct command *cmd, *prev_cmd = NULL;
	const char *desc;
	int i, j, ro, rp, oo, op;

	printf(".TH %s 8 \"LVM TOOLS #VERSION#\" \"Sistina Software UK\"\n",
		man_command_name ? upper_command_name(man_command_name) : "LVM_COMMANDS");

	for (i = 0; i < cmd_count; i++) {

		cmd = &cmd_array[i];

		if (prev_cmd && strcmp(prev_cmd->name, cmd->name)) {
			printf("Common options:\n");
			printf(".\n");
			print_man_usage_common(prev_cmd);

			printf("\n");
			printf(".SH OPTIONS\n");
			printf(".br\n");
			print_man_all_options_desc(cname);

			prev_cmd = NULL;
		}

		if ((cmd->cmd_flags & CMD_FLAG_SECONDARY_SYNTAX) && !include_secondary)
			continue;

		if (!(cmd->cmd_flags & CMD_FLAG_SECONDARY_SYNTAX) && !include_primary)
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
				print_man_all_options_list(cname);
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

			printf("\n");
			printf(".SH OPTIONS\n");
			printf(".br\n");
			print_man_all_options_desc(cname);

		}

		printf("\n");
		continue;
	}
}

