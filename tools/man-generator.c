/*
 * Copyright (C) 2024 Red Hat, Inc. All rights reserved.
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
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>


#define stack

static const char _OPTION_PREFIX[] = "O_";
static const char _TAB_NAME[] = "TT";
static const char _2TAB_NAME[] = "DTT";

struct cmd_context {
	void *libmem;
};

#define log_print log_error
#define log_error(fmt, args...) \
do { \
	printf(fmt "\n", ##args); \
} while (0)

#define dm_snprintf snprintf

static int dm_strncpy(char *dest, const char *src, size_t n)
{
	if (memccpy(dest, src, 0, n))
		return 1;

	if (n > 0)
		dest[n - 1] = '\0';

	return 0;
}

static inline int _dm_strncpy(char *dest, const char *src, size_t n) {
	return dm_strncpy(dest, src, n);
}

static char *dm_pool_strdup(void *p, const char *str)
{
	return strdup(str);
}

static void *dm_pool_alloc(void *p, size_t size)
{
	return malloc(size);
}

/* needed to include args.h */
#define ARG_COUNTABLE 0x00000001
#define ARG_GROUPABLE 0x00000002
#define ARG_NONINTERACTIVE 0x00000004
#define ARG_LONG_OPT  0x00000008
struct arg_values;

/* needed to include vals.h */
static inline int yes_no_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int activation_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int cachemetadataformat_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int cachemode_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int discards_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int mirrorlog_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_kb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int ssize_kb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int ssize_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int psize_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int nsize_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int uint32_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg_with_sign(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg_with_plus(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int extents_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int sextents_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int pextents_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int nextents_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int string_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int tag_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int permission_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int metadatatype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int segtype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int alloc_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int locktype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int readahead_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int regionsize_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int vgmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int pvmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int metadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int polloperation_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int writemostly_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int syncaction_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int reportformat_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int configreport_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int configtype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int repairtype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int dumptype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int headings_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }

#define MAN_PAGE_GENERATOR
#include "command.h"
#include "command.c"

/*
 * .TP (#size)
 *	section has 1st. line finished with '\n', than follows
 *	indented paragraph. When a new line is needed within such
 *	paragraph use  .br
 *
 * .IP (bullet) (#size)
 *      indented paragraph usually with bullet points.
 *
 * .P
 *	paragraph (should not proceed or follow .SH.)
 *
 * .RS ... .RE
 *	indented section.
 *
 * .na ... .ad
 *	used within paragraph to disable adjusting line
 *	filling, so the line is not filled with spaces.
 *
 *	note: With .TP section this MUST be used after the 1st. line
 *
 * .nh ... .hy
 *	used within paragraph to disable hyphenation,
 *	which may create confising syntax with dashes.
 *
 * .nf ... .fi
 *	used to disable filling or adjusting within paragraph
 *	usually comes with non-propotional font.
 *
 * .ns ... .rs
 *	used to disable/enable space mode.
 *
 * .in
 *      indent next lines with spaces
 *
 * .sp
 *      separate space and preserve paragraph
 *
 * \0
 *	is a space with width of '0' letter for nicer postscript rendering.
 *
 * \:
 *	is marking hyphenation within long words (i.e. long list arguments
 *	without spaces in between thus not easy to split otherwise).
 * \c
 *	continue with line (avoid emitting after space character).
 *
 *
 * Validate generated man pages with these tools:
 *
 * $ mandoc -T lint  command.#NUM
 *
 * $ groff -mandoc -t -K utf8 -rF0 -rHY=0 -rCHECKSTYLE=10 -ww -z command.#NUM
 */

static const size_t _LONG_LINE = 15; /* length of line that needed .nh .. .hy */

static const char *_lvt_enum_to_name(int lvt_enum)
{
	return _lv_types[lvt_enum].name;
}

static int _get_val_enum(const struct command_name *cname, int opt_enum)
{
	return _update_relative_opt(cname->name, opt_enum, opt_names[opt_enum].val_enum);
}
/*
 * FIXME: this just replicates the val usage strings
 * that officially lives in vals.h.  Should there
 * be some programmatic way to add man markup to
 * the strings in vals.h without replicating it?
 * Otherwise, this function has to be updated in
 * sync with any string changes in vals.h
 */
static void _print_val_man(const struct command_name *cname, int opt_enum, int val_enum)
{
	const char *str;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;
	int i;

	_was_hyphen = 0;

	switch (val_enum) {
	case sizemb_VAL:
		printf("\\fISize\\fP[m|\\:UNIT]");
		return;
	case ssizemb_VAL:
		printf("[\\fB+\\fP|\\fB-\\fP]\\fISize\\fP[m|\\:UNIT]");
		return;
	case psizemb_VAL:
		printf("[\\fB+\\fP]\\fISize\\fP[m|\\:UNIT]");
		return;
	case nsizemb_VAL:
		printf("[\\fB-\\fP]\\fISize\\fP[m|\\:UNIT]");
		return;
	case extents_VAL:
		printf("\\fINumber\\fP[PERCENT]");
		return;
	case sextents_VAL:
		printf("[\\fB+\\fP|\\fB-\\fP]\\fINumber\\fP[PERCENT]");
		return;
	case pextents_VAL:
		printf("[\\fB+\\fP]\\fINumber\\fP[PERCENT]");
		return;
	case nextents_VAL:
		printf("[\\fB-\\fP]\\fINumber\\fP[PERCENT]");
		return;
	case sizekb_VAL:
		printf("\\fISize\\fP[k|\\:UNIT]");
		return;
	case ssizekb_VAL:
		printf("[\\fB+\\fP|\\fB-\\fP]\\fISize\\fP[k|\\:UNIT]");
		return;
	case regionsizemb_VAL:
		printf("\\fISize\\fP[m|\\:UNIT]");
		return;
	case snumber_VAL:
		printf("[\\fB+\\fP|\\fB-\\fP]\\fINumber\\fP");
		return;
	case pnumber_VAL:
		printf("[\\fB+\\fP]\\fINumber\\fP");
		return;
	}

	str = val_names[val_enum].usage;
	if (!str)
		str = val_names[val_enum].name;

	if (!strcmp(str, "PV[:t|n|y]")) {
		printf("\\fIPV\\fP[\\fB:t\\fP|\\fBn\\fP|\\fBy\\fP]");
		return;
	}

	if (!strcmp(str, "Number") ||
	    !strcmp(str, "String") ||
	    !strncmp(str, "VG", 2) ||
	    !strncmp(str, "LV", 2) ||
	    !strncmp(str, "PV", 2) ||
	    !strcmp(str, "Tag")) {
		printf("\\fI%s\\fP", str);
		return;
	}

	if (!strchr(str, '|')) {
		printf("\\fB%s\\fP", str);
		return;
	}

	/* emit  | elements */
	if (!(line = strdup(str)))
		return;

	if ((_was_hyphen = (strlen(line) > _LONG_LINE)))
		printf("\\%%");

	_split_line(line, &line_argc, line_argv, '|');

	for (i = 0; i < line_argc; i++) {
		if (i)
			printf("|%s", _was_hyphen ? "\\:\\\n" : "");

		if (strncmp(line_argv[i], "[Number]", 8) == 0) {
			printf("[\\fINumber\\fP]");
			line_argv[i] += 8;
		}

		if (strstr(line_argv[i], "Number"))
			printf("\\fI%s\\fP", line_argv[i]);
		else
			printf("\\fB%s\\fP", line_argv[i]);
	}

	free(line);

	_was_hyphen = 0;
}

static void _print_ds_opt_name(int opt_enum)
{
	printf(".%s%s\n", _OPTION_PREFIX, opt_names[opt_enum].long_opt + 2);
}

static void _print_bracket_ds_opt_name(int opt_enum)
{
	printf("[\n");
	_print_ds_opt_name(opt_enum);
	printf("]\n");
}

static void _print_def_man(const struct command_name *cname, int opt_enum,
			   const struct arg_def *def, int usage,
			   uint64_t *lv_type_bits)
{
	int val_enum, tmp_val;
	int sep = 0;

	if (lv_type_bits)
		*lv_type_bits = 0;

	for (val_enum = 0; val_enum < VAL_COUNT; val_enum++) {
		if (def->val_bits & val_enum_to_bit(val_enum)) {

			if (val_enum == conststr_VAL)
				printf("\\fB%s\\fP", def->str);

			else if (val_enum == constnum_VAL)
				printf("\\fB%llu\\fP", (unsigned long long)def->num);

			else {
				if (sep) printf("|");

				if (!usage || !val_names[val_enum].usage) {
					if (_was_hyphen) {
						printf("\\:\\c\n");
						_was_hyphen = 0;
					}

					/* special case to print LV1 instead of LV */
					if ((val_enum == lv_VAL) && def->lvt_bits && lv_type_bits) {
						printf("\\fILV1\\fP");
						*lv_type_bits = def->lvt_bits;
					} else {
						printf("\\fI%s\\fP", val_names[val_enum].name);
					}
				} else {
					tmp_val = _update_relative_opt(cname->name, opt_enum, val_enum);
					_print_val_man(cname, opt_enum, tmp_val);
				}

				sep = 1;
			}

			if (((val_enum == vg_VAL) && (def->flags & ARG_DEF_FLAG_NEW_VG)) ||
			    ((val_enum == lv_VAL) && (def->flags & ARG_DEF_FLAG_NEW_LV)))
				printf("\\fI_new\\fP");
		}
	}

	if (def->flags & ARG_DEF_FLAG_MAY_REPEAT)
		printf("\\ .\\|.\\|.\\&");
}

#define	LONG_OPT_NAME_LEN	64
static const char *_man_long_opt_name(const char *cmdname, int opt_enum)
{
	static char _long_opt_name[LONG_OPT_NAME_LEN];
	const char *long_opt;
	unsigned i;

	memset(_long_opt_name, 0, sizeof(_long_opt_name));

	switch (opt_enum) {
	case syncaction_ARG:
		long_opt = "--[raid]syncaction";
		break;
	case writemostly_ARG:
		long_opt = "--[raid]writemostly";
		break;
	case minrecoveryrate_ARG:
		long_opt = "--[raid]minrecoveryrate";
		break;
	case maxrecoveryrate_ARG:
		long_opt = "--[raid]maxrecoveryrate";
		break;
	case writebehind_ARG:
		long_opt = "--[raid]writebehind";
		break;
	case vgmetadatacopies_ARG:
		if (!strncmp(cmdname, "vg", 2))
			long_opt = "--[vg]metadatacopies";
		else
			long_opt = "--vgmetadatacopies";
		break;
	case pvmetadatacopies_ARG:
		if (!strncmp(cmdname, "pv", 2))
			long_opt = "--[pv]metadatacopies";
		else
			long_opt = "--pvmetadatacopies";
		break;
	default:
		long_opt = opt_names[opt_enum].long_opt;
		break;
	}

	if (strchr(long_opt, '[')) {
		for (i = 0; *long_opt && i < sizeof(_long_opt_name) - 1; ++long_opt, ++i) {
			if (i < (sizeof(_long_opt_name) - 8))
				switch(*long_opt) {
				case '[':
					memcpy(_long_opt_name + i, "\\fP[\\fB", 7);
					i += 6; // 6 + 1
					continue;
				case ']':
					memcpy(_long_opt_name + i, "\\fP]\\fB", 7);
					i += 6; // 6 + 1
					continue;
				}
			_long_opt_name[i] = *long_opt;
		}
		_long_opt_name[i] = 0;
		return _long_opt_name;
	}

	return long_opt;
}

/* indent adds spaces for '-X|' when short option is missing */
static void _print_man_option(const char *name, int opt_enum)
{
	int short_opt = opt_names[opt_enum].short_opt;

	if (short_opt)
		printf("\\fB-%c\\fP|", short_opt);

	printf("\\fB%s\\fP", _man_long_opt_name(name, opt_enum));
}

/*
 * Prepare list of all option '.ds OPTION_PREFIX_name ...'
 * then through the whole man page this can be referenced
 * via \*[OPTION_PREFIX_name]
 */

static void _print_man_all_options_list_string(const struct command_name *cname)
{
	const struct command_name_args *cna;
	int opt_enum, val_enum;
	int i;

	cna = &command_names_args[cname->lvm_command_enum];

	printf(".\n.\\\"List of all options as O_string.\n.\n");

	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cna->all_options[opt_enum])
			continue;

		printf(".de %s%s\n", _OPTION_PREFIX, opt_names[opt_enum].long_opt + 2);

		val_enum = _get_val_enum(cname, opt_enum);

		if (val_names[val_enum].fn ||
		    (opt_names[opt_enum].flags & ARG_COUNTABLE))
			printf(".OPA ");
		else
			printf(".OPS ");

		if (opt_names[opt_enum].short_opt)
			printf("%c ", opt_names[opt_enum].short_opt);

		printf("%s\n", _man_long_opt_name(cname->name, opt_enum) + 2);

		if (val_names[val_enum].fn) {
			if (val_names[val_enum].usage)
				_print_val_man(cname, opt_enum, val_enum);
			else
				printf("\\fI%s\\fP", val_names[val_enum].name);
			printf("\n");
		}

		if (opt_names[opt_enum].flags & ARG_COUNTABLE)
			printf("\\&\\.\\|.\\|.\\&\n");

		printf("..\n");
	}
}

static void _print_man_usage(char *lvmname, struct command *cmd)
{
	const struct command_name *cname = &command_names[cmd->lvm_command_enum];
	const struct command_name_args *cna = &command_names_args[cmd->lvm_command_enum];
	int any_req = (cmd->cmd_flags & CMD_FLAG_ANY_REQUIRED_OPT) ? 1 : 0;
	int ro, rp, oo, op, opt_enum, sep, short_opts, indented = 0;
	int include_extents = 0;
	int lvt_enum;
	uint64_t lv_type_bits = 0;

	_was_hyphen = 0;
	printf(".B %s\n", lvmname);

	if (!any_req)
		goto ro_normal;

	/*
	 * required options that follow command name, all required
	 */
	if (cmd->ro_count) {
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if ((opt_enum == size_ARG) && command_has_alternate_extents(cname))
				include_extents = 1;

			_print_ds_opt_name(opt_enum);
			//printf("\n");
		}
	}

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

	if (cmd->any_ro_count) {
		printf(".RS\n"
		       "(\n");

		indented = 1;
		sep = 0;
		for (short_opts = 1; short_opts >= 0; --short_opts)
			for (ro = cmd->ro_count; ro < cmd->ro_count + cmd->any_ro_count; ro++) {
				opt_enum = cmd->required_opt_args[ro].opt;

				/* 1st. pass print required options with a short opt */
				/* 2nd. pass print required options without a short opt */
				if ((short_opts && !opt_names[opt_enum].short_opt) ||
				    (!short_opts && opt_names[opt_enum].short_opt))
					continue;

				if (sep++) {
					if (sep == 2)
						printf(".in +2n\n"); /* works here also as .br */
					else
						printf(".br\n");
				}

				_print_ds_opt_name(opt_enum);
			}

		printf(")\n"
		       ".in\n");
	}

	/* print required position args on a new line after the any_req set */
	if (cmd->rp_count) {
		sep = 0;
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
                                if (sep++)
					printf(" ");
				_print_def_man(cname, 0, &cmd->required_pos_args[rp].def, 1, NULL);
			}
		}

		if (sep)
			printf("\n.br\n");
	}

	goto oo_count;

 ro_normal:

	/*
	 * all are required options, print as:
	 * -a|--aaa <val> -b|--bbb <val>
	 */
	if (cmd->ro_count) {
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if ((opt_enum == size_ARG) &&
			    command_has_alternate_extents(cname))
				include_extents = 1;

			switch (opt_enum) {
			case type_ARG:
			case name_ARG:
			case thinpool_ARG:
			case cachepool_ARG:
			case vdopool_ARG:
				/* Specifications of the argument for:
				 *   --type to select some specific type(s)
				 *   --name|{cache|thin|vdo}pool option than can specify LV_new name
				 * TODO: Are there any more option with arg specifications ?
				 */
				_print_man_option(cmd->name, opt_enum);

				if (cmd->required_opt_args[ro].def.val_bits) {
					printf(" ");
					_print_def_man(cname, opt_enum, &cmd->required_opt_args[ro].def, 1,
						       lv_type_bits ? NULL : &lv_type_bits);
				}
				printf("\n");
				break;
			default:
				_print_ds_opt_name(opt_enum);
			}

			/* avoid long line wrapping */
			if ((ro == 1) && (cmd->ro_count > 2) &&
			    (!indented++))
				/* .RS makes also .br here
				 * and indent only the next line by 2 spaces
				 *  Also avoid using .in +2n - as it can make
				 *  2 line space in html rendering.
				 */
				printf(".RS\n"
				       "\\ \\&\n");
			/* no .br since this should fit into 1 or 2 lines */
		}
	}

	/* print required position args on the same line as the required options */
	if (cmd->rp_count) {
		sep = 0;
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				if (sep++)
					printf(" ");
				/* Only print lv_type_bits for one LV arg (no cases exist with more) */
				_print_def_man(cname, 0, &cmd->required_pos_args[rp].def, 1,
					       lv_type_bits ? NULL : &lv_type_bits);
			}
		}

		if (sep) {
			/* Finish line and if we are already in 'section' do a .br
			 * Otherwise OO_COUNT well open section anyway and does implicit .br */
			printf("\n");
			if (indented)
				printf(".br\n");
		}
	}

 oo_count: /* OO_COUNT */
	if (!indented++)
		printf(".RS\n");

	if (cmd->oo_count) {
		if (cmd->autotype) {
			printf("[ \\fB--type %s\\fP", cmd->autotype);

			if (cmd->autotype2)
				printf("|\\fB%s\\fP", cmd->autotype2);

			printf(" ] (implied)\n"
			       ".br\n");
		}

		if (include_extents) {
			/*
			 * NB we don't just pass extents_VAL here because the
			 * actual val type for extents_ARG has been adjusted
			 * in opt_names[] according to the command name.
			 */
			_print_bracket_ds_opt_name(extents_ARG);
			printf(".br\n");
		}

		for (short_opts = 1; short_opts >= 0; --short_opts)
			for (oo = 0; oo < cmd->oo_count; oo++) {
				opt_enum = cmd->optional_opt_args[oo].opt;

				/* 1st. pass print required options with a short opt */
				/* 2nd. pass print required options without a short opt */
				if ((short_opts && !opt_names[opt_enum].short_opt) ||
				    (!short_opts && opt_names[opt_enum].short_opt))
					continue;

				if ((cna->variants > 1) && cna->common_options[opt_enum])
					continue;

				if (_is_lvm_all_opt(opt_enum))
					continue;

				_print_bracket_ds_opt_name(opt_enum);
				printf(".br\n");
			}


		printf("[ COMMON_OPTIONS ]\n");
	}

	/* OP_COUNT */
	if (cmd->op_count) {
		printf(".br\n"
		       "[");

		for (op = 0; op < cmd->op_count; op++) {
			if (cmd->optional_pos_args[op].def.val_bits) {
				printf(" ");
				_print_def_man(cname, 0, &cmd->optional_pos_args[op].def, 1, NULL);
			}
		}

		printf_hyphen(']');
	}

	if (lv_type_bits) {
		printf(".sp\n"
		       "LV1 types:\n");

		for (lvt_enum = 1; lvt_enum < LVT_COUNT; lvt_enum++) {
			if (lvt_bit_is_set(lv_type_bits, lvt_enum))
				printf("%s\n", _lvt_enum_to_name(lvt_enum));
		}
	}

	if (indented)
		printf(".RE\n");
}

/*
 * common options listed in the usage section.
 *
 * For commands with only one variant, this is only
 * the options which are common to all lvm commands
 * (in lvm_all, see _is_lvm_all_opt).
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

static void _print_man_usage_common_lvm_or_cmd(const struct command *cmd,
					       const struct command_name_args *cna,
					       const char *options_type)
{
	int i, oo, opt_enum, short_opts, sep = 0;

	printf(".P\n"
	       "Common options for %s:\n"
	       ".RS\n", options_type);

	for (short_opts = 1; short_opts >= 0; --short_opts)
		for (i = 0; i < ARG_COUNT; i++) {
			opt_enum = opt_names_alpha[i]->opt_enum;

			/* 1st. pass print those with short opts */
			/* 2nd. pass print those without short opts */
			if ((short_opts && !opt_names[opt_enum].short_opt) ||
			    (!short_opts && opt_names[opt_enum].short_opt))
				continue;

			if (cna) {
				if (!cna->common_options[opt_enum])
					continue;
				if (_is_lvm_all_opt(opt_enum))
					continue;
			} else if (!_is_lvm_all_opt(opt_enum))
				continue;

			for (oo = 0; oo < cmd->oo_count; oo++) {
				if (cmd->optional_opt_args[oo].opt != opt_enum)
					continue;

				if (sep)
					printf(".br\n");

				_print_bracket_ds_opt_name(opt_enum);
				sep = 1;
				break;
			}
		}

	printf(".RE\n");
}

static void _print_man_usage_common_lvm(const struct command *cmd)
{
	_print_man_usage_common_lvm_or_cmd(cmd, NULL, "lvm");
}

static void _print_man_usage_common_cmd(const struct command *cmd)
{
	const struct command_name_args *cna = &command_names_args[cmd->lvm_command_enum];
	int opt_enum;

	/* common cmd options only used with variants */
	if (cna->variants < 2)
		return;

	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cna->common_options[opt_enum])
			continue;

		if (_is_lvm_all_opt(opt_enum))
			continue;

		/* found option common for command */
		_print_man_usage_common_lvm_or_cmd(cmd, cna, "command");
		break;
	}
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
 *
 * Supports also 'prefix' for all commands before the first '#cmdname'.
 * "#\n" is restorting printing for all commands.
 */
static void _print_man_option_desc(const struct command_name *cname, int opt_enum)
{
	const char *desc = opt_names[opt_enum].desc;
	size_t clen = strlen(cname->name);
	char buf[DESC_LINE];
	int check_for_new_section = 1;
	int print_section = 1; /* initial description without cmdname is printed */
	unsigned bi = 0;

	while (*desc) {
		buf[bi++] = *desc;

		if (bi == DESC_LINE) {
			log_error("Parsing command defs: print_man_option_desc line too long.");
			exit(EXIT_FAILURE);
		}

		if (*desc++ != '\n' && *desc)
			continue;  /* read until '\n' or end of description */

		/* Line could be either new cmdname or a regular text description
		 * either for all commands or for the matching cmdname.
		 * Line starting with #cmdname starts a new 'text section'.
		 * Multiple command names can use the same text */
		if (buf[0] == '#') {
			if (check_for_new_section) {
				check_for_new_section = 0;
				print_section = 0;
			}
			if (bi > 2)
				bi -= 2;
			else
				bi = 0;
			if (!bi ||  /* empty cmd resets section to all commands */
			    ((bi == clen) && !strncmp(buf + 1, cname->name, clen))) {
				print_section = 1;
			}
		} else if (print_section) {
			/* Printable text 'splits' individual 'cmdname' section */
			check_for_new_section = 1;
			if (bi) {
				buf[bi] = 0;
				printf("%s", buf);
			}
		}

		bi = 0;
	}
}

/*
 * Print a list of all options names for a given command name.
 */

static void _print_man_all_options_list(const struct command_name *cname)
{
	const struct command_name_args *cna;
	int opt_enum;
	int i;
	int adl = 0;

	cna = &command_names_args[cname->lvm_command_enum];

	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cna->all_options[opt_enum])
			continue;

		if (!adl) {
			adl = 1;
			printf(".RS 5\n");
			/* Optionally we can use different alignment for
			 * postscript/pdf adn ascii renderer
			 */
			printf(".if t .ta 3nR +1uL \\\" PostScript/PDF\n");
			printf(".PD 0\n");
		} else
			printf(".br\n");
			//printf(".HP\n");
		_print_ds_opt_name(opt_enum);
	}

	if (adl) {
		printf(".PD\n");
		printf(".if t .ta\n");  /* reset tabbing */
		printf(".RE\n");
	}
}

/*
 * All options used for a given command name, along with descriptions.
 */

static void _print_man_all_options_desc(const struct command_name *cname)
{
	const struct command_name_args *cna;
	int opt_enum;
	int i;

	cna = &command_names_args[cname->lvm_command_enum];

	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cna->all_options[opt_enum])
			continue;

		printf(".\n.TP\n");

		_print_ds_opt_name(opt_enum);

		if (opt_names[opt_enum].desc)
			_print_man_option_desc(cname, opt_enum);
	}
}

static void _print_man_all_positions_desc(const struct command_name *cname)
{
	struct command *cmd;
	int ci, rp, op;
	int has_vg_val = 0;
	int has_lv_val = 0;
	int has_pv_val = 0;
	int has_tag_val = 0;
	int has_select_val = 0;
	int has_lv_type = 0;

	for (ci = 0; ci < COMMAND_COUNT; ci++) {
		cmd = &commands[ci];

		if (strcmp(cmd->name, cname->name))
			continue;

		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits & val_enum_to_bit(vg_VAL))
				has_vg_val = 1;

			if (cmd->required_pos_args[rp].def.val_bits & val_enum_to_bit(lv_VAL)) {
				has_lv_val = 1;
				if (cmd->required_pos_args[rp].def.lvt_bits)
					has_lv_type = 1;
			}

			if (cmd->required_pos_args[rp].def.val_bits & val_enum_to_bit(pv_VAL))
				has_pv_val = 1;

			if (cmd->required_pos_args[rp].def.val_bits & val_enum_to_bit(tag_VAL))
				has_tag_val = 1;

			if (cmd->required_pos_args[rp].def.val_bits & val_enum_to_bit(select_VAL))
				has_select_val = 1;
		}

		for (op = 0; op < cmd->op_count; op++) {
			if (cmd->optional_pos_args[op].def.val_bits & val_enum_to_bit(vg_VAL))
				has_vg_val = 1;

			if (cmd->optional_pos_args[op].def.val_bits & val_enum_to_bit(lv_VAL)) {
				has_lv_val = 1;
				if (cmd->optional_pos_args[op].def.lvt_bits)
					has_lv_type = 1;
			}

			if (cmd->optional_pos_args[op].def.val_bits & val_enum_to_bit(pv_VAL))
				has_pv_val = 1;

			if (cmd->optional_pos_args[op].def.val_bits & val_enum_to_bit(tag_VAL))
				has_tag_val = 1;

			if (cmd->optional_pos_args[op].def.val_bits & val_enum_to_bit(select_VAL))
				has_select_val = 1;
		}
	}

	if (has_vg_val) {
		printf(".\n.TP\n");

		printf(".I %s\n", val_names[vg_VAL].name);
		printf("Volume Group name.  See \\fBlvm\\fP(8) for valid names.\n");

		if (!strcmp(cname->name, "lvcreate"))
			printf("For lvcreate, the required VG positional arg may be\n"
			       "omitted when the VG name is included in another option,\n"
			       "e.g. --name VG/LV.\n");
	}

	if (has_lv_val) {
		printf(".\n.TP\n");

		printf(".I %s\n", val_names[lv_VAL].name);
		printf("Logical Volume name.  See \\fBlvm\\fP(8) for valid names.\n"
		       "An LV positional arg generally includes the VG name and LV name, e.g. VG/LV.\n");

		if (has_lv_type)
			printf("LV1 indicates the LV must have a specific type, where the\n"
			       "accepted LV types are listed. (raid represents raid<N> type).\n");

	}

	if (has_pv_val) {
		printf(".\n.TP\n");

		printf(".I %s\n", val_names[pv_VAL].name);
		printf("Physical Volume name, a device path under /dev.\n"
		       "For commands managing physical extents, a PV positional arg\n"
		       "generally accepts a suffix indicating a range (or multiple ranges)\n"
		       "of physical extents (PEs). When the first PE is omitted, it defaults\n"
		       "to the start of the device, and when the last PE is omitted it defaults to end.\n"
                       ".br\n"
		       "Start and end range (inclusive):\n"
		       "\\fIPV\\fP[\\fB:\\fP\\fIPE\\fP\\fB-\\fP\\fIPE\\fP]\\ .\\|.\\|.\\&\n"
		       ".br\n"
		       "Start and length range (counting from 0):\n"
		       "\\fIPV\\fP[\\fB:\\fP\\fIPE\\fP\\fB+\\fP\\fIPE\\fP]\\ .\\|.\\|.\\&\n");
	}

	if (has_tag_val) {
		printf(".\n.TP\n");

		printf(".I %s\n", val_names[tag_VAL].name);
		printf("Tag name.  See \\fBlvm\\fP(8) for information about tag names and using tags\n"
		       "in place of a VG, LV or PV.\n");
	}

	if (has_select_val) {
		printf(".\n.TP\n");

		printf(".I %s\n", val_names[select_VAL].name);
		printf("Select indicates that a required positional parameter can\n"
		       "be omitted if the \\fB--select\\fP option is used.\n"
		       "No arg appears in this position.\n");
	}

	/* Every command uses a string arg somewhere. */

	printf(".\n.TP\n");
	printf(".I %s\n", val_names[string_VAL].name);
	printf("See the option description for information about the string content.\n");

	/*
	 * We could possibly check if the command accepts any option that
	 * uses Size, and only print this in those cases, but this seems
	 * so common that we should probably always print it.
	 */

	printf(".\n.TP\n");
	printf(".IR Size [UNIT]\n");
	printf("Size is an input number that accepts an optional unit.\n"
	       "Input units are always treated as base two values, regardless of\n"
	       "capitalization, e.g. 'k' and 'K' both refer to 1024.\n"
	       "The default input unit is specified by letter, followed by |UNIT.\n"
	       "UNIT represents other possible input units:\n"
	       ".BR b | B\nis bytes,\n.BR s | S\nis sectors of 512 bytes,\n"
	       ".BR k | K\nis KiB,\n.BR m | M\nis MiB,\n.BR g | G\nis GiB,\n"
	       ".BR t | T\nis TiB,\n.BR p | P\nis PiB,\n.BR e | E\nis EiB.\n"
	       "(This should not be confused with the output control --units,\n"
	       "where capital letters mean multiple of 1000.)\n");

	printf(".\n.SH ENVIRONMENT VARIABLES\n.\n");
	printf("See \\fBlvm\\fP(8) for information about environment variables used by lvm.\n"
	       "For example, \\fBLVM_VG_NAME\\fP can generally be substituted\n"
	       "for a required VG parameter.\n");
}

static void _print_desc_man(const char *desc)
{
	char buf[DESC_LINE] = {0};
	unsigned di;
	int bi = 0;

	for (di = 0; desc[di]; di++) {
		if (desc[di] == '\n')
			continue;

		if (!strncmp(&desc[di], "DESC:", 5)) {
			if (bi) {
				printf("%s\n", buf);
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

	if (bi)
		printf("%s\n", buf);
}

static const char *_upper_command_name(char *str)
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

#define MAX_MAN_DESC (1024 * 1024)

static int _include_description_file(char *name, char *des_file)
{
	char *buf;
	int fd, r = 0;
	ssize_t sz;
	struct stat statbuf = { 0 };

	if ((fd = open(des_file, O_RDONLY)) < 0) {
		log_error("Failed to open description file %s.", des_file);
		return 0;
	}

	if (fstat(fd, &statbuf) < 0) {
		log_error("Failed to stat description file %s.", des_file);
		goto out_close;
	}

	if (statbuf.st_size > MAX_MAN_DESC) {
		log_error("Description file %s is too large.", des_file);
		goto out_close;
	}

	if (!(buf = malloc(statbuf.st_size + 1))) {
		log_error("Failed to allocate buffer for description file %s.", des_file);
		goto out_close;
	}

	if ((sz = read(fd, buf, statbuf.st_size)) < 0) {
		log_error("Failed to read description file %s.", des_file);
		goto out_free;
	}

	buf[sz] = '\0';
	printf(".\n.SH DESCRIPTION\n"
	       ".\n%s", buf);
	r = 1;

out_free:
	free(buf);
out_close:
	(void) close(fd);

	return r;
}

static void _print_separate_section(void)
{
	printf(".\n.P\n"
	       "\\(em\n"
	       ".P\n.\n");
}

static void _print_cmd_usage_option(const struct command_name *cname,
				    struct command *cmd)
{
	printf("\\(em\n");

	_print_man_usage_common_cmd(cmd);
	_print_man_usage_common_lvm(cmd);

	printf(".hy\n"
	       ".ad\n"
	       ".\n.SH OPTIONS\n");
	_print_man_all_options_desc(cname);
	printf(".\n.SH VARIABLES\n");
	_print_man_all_positions_desc(cname);
}

static int _print_man(char *name, char *des_file, int secondary)
{
	const struct command_name *cname;
	const struct command_name_args *cna;
	struct command *cmd, *prev_cmd = NULL;
	char *lvmname = name;
	int i, sep = 0;

	if (!strncmp(name, "lvm-", 4)) {
		name[3] = ' ';
		name += 4;
	}

	cname = find_command_name(name);

	printf(".TH %s 8 \"LVM TOOLS #VERSION#\" \"Red Hat, Inc.\"\n.\n",
	       _upper_command_name(lvmname));

	/* Postscript rendering is using tabulator with '.ta'
	 * This works only in 'copy-in' mode otherwise \t does NOT work
	 * as tabulator and to achieve this use of .ds. */

	/* Use \0 to make a space of width of symbol '0'
	 * (use to make space for option without 'short' option.) */
	printf(".ie t \\{\\\n"
	       ".\\\" PostScript/PDF with tabs\n"
	       ". ds %s \\t\n"
	       ". ds %s \\t\\t\n"
	       ".\\}\n"
	       ".el \\{\\\n"
	       ". ds %s \\&\n"
	       ". ds %s \\0\\0\\0\n"
	       ".\\}\n",
	       _TAB_NAME, _2TAB_NAME,
	       _TAB_NAME, _2TAB_NAME);

	/* Note: using double '\\' for lazy evaluation (\\*[name])
	 * This way we easily remove tabs from option description later */
	printf(".\n"
	       ".de OPT\n"
	       ".ie \\\\n(.$>1 \\\\*[%s]\\fB-\\\\$1\\fP|"
	       "\\\\*[%s]\\fB--\\\\$2\\fP\\c\n"
	       ".el \\\\*[%s]\\fB--\\\\$1\\fP\\c\n"
	       "..\n", _TAB_NAME, _TAB_NAME, _2TAB_NAME);

	printf(".\n"
	       ".de OPA\n"
	       ".OPT \\\\$*\n"
	       "\\ \\c\n"
	       "..\n"
	       ".de OPS\n"
	       ".OPT \\\\$*\n"
	       "\\&\n"
	       "..\n"
	       ".\n");

	for (i = 0; i < COMMAND_COUNT; i++) {

		cmd = &commands[i];

		if (prev_cmd && cname && strcmp(prev_cmd->name, cmd->name)) {
			printf(".P\n");
			_print_cmd_usage_option(cname, prev_cmd);
			prev_cmd = NULL;
		}

		if (cmd->cmd_flags & CMD_FLAG_PREVIOUS_SYNTAX)
			continue;

		if ((cmd->cmd_flags & CMD_FLAG_SECONDARY_SYNTAX) && !secondary)
			continue;

		if (strcmp(name, cmd->name))
			continue;

		if (!prev_cmd || strcmp(prev_cmd->name, cmd->name)) {
			if (cname)
				_print_man_all_options_list_string(cname);

			printf(".\n.SH NAME\n.\n");
			if (cname && cname->desc)
				printf("%s \\(em %s\n", lvmname, cname->desc);
			else
				printf("%s\n", lvmname);

			printf(".\n.SH SYNOPSIS\n.\n"
			       ".nh\n"
			       ".TP\n");
			prev_cmd = cmd;

			cname = &command_names[cmd->lvm_command_enum];
			cna = &command_names_args[cmd->lvm_command_enum];

			printf("\\fB%s\\fP", lvmname);

			if (cna->variant_has_ro)
				printf(" \\fIoption_args\\fP");

			if (cna->variant_has_rp)
				printf(" \\fIposition_args\\fP");

			printf("\n");

			if (cna->variant_has_oo)
				printf("[ \\fIoption_args\\fP ]\n");

			if (cna->variant_has_op) {
				if (cna->variant_has_oo)
					printf(".br\n");

				printf("[ \\fIposition_args\\fP ]\n");
			}

			/* listing them all when there's only 1 or 2 is just repetitive */
			if (cname && cna->variants > 2) {
				printf(".P\n"
				       ".na\n");
				_print_man_all_options_list(cname);
				printf(".ad\n");
			}
			printf(".hy\n");

			/* remove spacing/tabbing from option */
			printf(".\n.ds %s \\&\n", _TAB_NAME);
			printf(".ds %s \\&\n.\n", _2TAB_NAME);

			if (des_file && !_include_description_file(lvmname, des_file))
				return 0;

			printf(".\n.SH USAGE\n.\n"
			       ".nh\n"
			       ".na\n");
		}

		if (sep)
			_print_separate_section();

		if (cmd->desc) {
			_print_desc_man(cmd->desc);
			printf(".P\n");
		}

		_print_man_usage(lvmname, cmd);

		if (cname && (i == (COMMAND_COUNT - 1))) {
			_print_cmd_usage_option(cname, cmd);
		} else {
			if (cna->variants > 1)
				sep = 1;
		}
	}

	return 1;
}

static void _print_man_secondary(char *name)
{
	struct command *cmd;
	char *lvmname = name;
	int header = 0;
	int i;

	if (!strncmp(name, "lvm-", 4))
		name += 4;

	for (i = 0; i < COMMAND_COUNT; i++) {

		cmd = &commands[i];

		if (cmd->cmd_flags & CMD_FLAG_PREVIOUS_SYNTAX)
			continue;

		if (!(cmd->cmd_flags & CMD_FLAG_SECONDARY_SYNTAX))
			continue;

		if (strcmp(name, cmd->name))
			continue;

		if (!header) {
			printf(".\n.SH ADVANCED USAGE\n.\n");
			printf("Alternate command forms, advanced command usage,\n"
			       "and listing of all valid syntax for completeness.\n"
			       ".P\n");
			header = 1;
		} else
			_print_separate_section();

		if (cmd->desc) {
			_print_desc_man(cmd->desc);
			printf(".P\n");
		}

		_print_man_usage(lvmname, cmd);
	}
}

static void _print_opt_list(const char *prefix, int *opt_list, int opt_count)
{
	int i;
	int opt_enum;

	printf("%s ", prefix);
	for (i = 0; i < opt_count; i++) {
		opt_enum = opt_list[i];
		printf(" %s", opt_names[opt_enum].long_opt);
	}
	printf("\n");
}

/* return 1 if the lists do not match, 0 if they match */
static int _compare_opt_lists(int *list1, int count1, int *list2, int count2, const char *type1_str, const char *type2_str)
{
	int i, j;

	if (count1 != count2)
		return 1;

	for (i = 0; i < count1; i++) {
		for (j = 0; j < count2; j++) {

			/* lists do not match if one has --type foo and the other --type bar */
			if ((list1[i] == type_ARG) && (list2[j] == type_ARG) &&
			    type1_str && type2_str && strcmp(type1_str, type2_str)) {
				return 1;
			}

			if (list1[i] == list2[j])
				goto next;
		}
		return 1;
 next:
		;
	}

	return 0;
}

static int _compare_cmds(struct command *cmd1, struct command *cmd2, int *all_req_opts)
{
	const char *cmd1_type_str = NULL;
	const char *cmd2_type_str = NULL;
	int opt_list_1[ARG_COUNT] = { 0 };
	int opt_list_2[ARG_COUNT] = { 0 };
	int opt_count_1 = 0;
	int opt_count_2 = 0;
	int i, j;
	int r = 1;

	/* different number of required pos items means different cmds */
	if (cmd1->rp_count != cmd2->rp_count)
		return 1;

	/* different types of required pos items means different cmds */
	for (i = 0; i < cmd1->rp_count; i++) {
		if (cmd1->required_pos_args[i].def.val_bits != cmd2->required_pos_args[i].def.val_bits)
			return 1;
	}

	/* create opt list from cmd1 */
	for (i = 0; i < cmd1->ro_count; i++) {
		if (!all_req_opts[cmd1->required_opt_args[i].opt])
			continue;

		opt_list_1[opt_count_1++] = cmd1->required_opt_args[i].opt;

		if (cmd1->required_opt_args[i].opt == type_ARG)
			cmd1_type_str = cmd1->required_opt_args[i].def.str;
	}

	/* create opt list from cmd2 */
	for (i = 0; i < cmd2->ro_count; i++) {
		if (!all_req_opts[cmd2->required_opt_args[i].opt])
			continue;

		opt_list_2[opt_count_2++] = cmd2->required_opt_args[i].opt;

		if (cmd2->required_opt_args[i].opt == type_ARG)
			cmd2_type_str = cmd2->required_opt_args[i].def.str;
	}

	/* "--type foo" and "--type bar" are different */
	if (cmd1_type_str && cmd2_type_str && strcmp(cmd1_type_str, cmd2_type_str))
		return 1;

	/* compare opt_list_1 and opt_list_2 */
	if (!_compare_opt_lists(opt_list_1, opt_count_1, opt_list_2, opt_count_2, NULL, NULL)) {
		log_error("Repeated commands %s %s",
			  command_enum(cmd1->command_enum),
			  command_enum(cmd2->command_enum));
		log_error("cmd1: %s", cmd1->desc);
		log_error("cmd2: %s", cmd2->desc);
		_print_opt_list("cmd1 options: ", opt_list_1, opt_count_1);
		_print_opt_list("cmd2 options: ", opt_list_2, opt_count_2);
		printf("\n");
		r = 0;
	}

	/* check if cmd1 matches cmd2 + one of its oo */
	for (i = 0; i < cmd2->oo_count; i++) {
		/* for each cmd2 optional_opt_arg, add it to opt_list_2
		   and compare opt_list_1 and opt_list_2 again */

		/* cmd1 "--type foo" and cmd2 OO "--type bar" are different */
		if (cmd2->optional_opt_args[i].opt == type_ARG) {
			if (cmd2->optional_opt_args[i].def.str && cmd1_type_str &&
			    strcmp(cmd2->optional_opt_args[i].def.str, cmd1_type_str))
				return 1;
		}

		opt_list_2[opt_count_2] = cmd2->optional_opt_args[i].opt;

		if (!_compare_opt_lists(opt_list_1, opt_count_1, opt_list_2, opt_count_2+1, NULL, NULL)) {
			log_error("Repeated commands %s %s",
				  command_enum(cmd1->command_enum),
				  command_enum(cmd2->command_enum));
			log_error("cmd1: %s", cmd1->desc);
			log_error("cmd2: %s", cmd2->desc);
			log_error("Included cmd2 OO: %s", opt_names[cmd2->optional_opt_args[i].opt].long_opt);
			_print_opt_list("cmd1 options: ", opt_list_1, opt_count_1);
			_print_opt_list("cmd2 options: ", opt_list_2, opt_count_2+1);
			printf("\n");
			r = 0;
		}
	}

	/* check if cmd1 + an oo matches cmd2 + an oo */

	if (!cmd1_type_str) {
		for (i = 0; i < cmd1->oo_count; i++) {
			if (cmd1->optional_opt_args[i].opt == type_ARG)
				cmd1_type_str = cmd1->optional_opt_args[i].def.str;
		}
	}
	if (!cmd2_type_str) {
		for (j = 0; j < cmd2->oo_count; j++) {
			if (cmd2->optional_opt_args[j].opt == type_ARG)
				cmd2_type_str = cmd2->optional_opt_args[j].def.str;
		}
	}

	for (i = 0; i < cmd1->oo_count; i++) {

		for (j = 0; j < cmd2->oo_count; j++) {
			if (cmd1->optional_opt_args[i].opt == cmd2->optional_opt_args[j].opt)
				continue;

			opt_list_1[opt_count_1] = cmd1->optional_opt_args[i].opt;
			opt_list_2[opt_count_2] = cmd2->optional_opt_args[j].opt;

			if (!_compare_opt_lists(opt_list_1, opt_count_1+1, opt_list_2, opt_count_2+1, cmd1_type_str, cmd2_type_str)) {
				log_error("Repeated commands %s %s",
					  command_enum(cmd1->command_enum),
					  command_enum(cmd2->command_enum));
				log_error("cmd1: %s", cmd1->desc);
				log_error("cmd2: %s", cmd2->desc);
				log_error("Included cmd1 OO: %s and cmd2 OO: %s",
					  opt_names[cmd1->optional_opt_args[i].opt].long_opt,
					  opt_names[cmd2->optional_opt_args[j].opt].long_opt);
				_print_opt_list("cmd1 options: ", opt_list_1, opt_count_1+1);
				_print_opt_list("cmd2 options: ", opt_list_2, opt_count_2+1);
				printf("\n");
				r = 0;
			}
		}
	}
	return r;
}

static int _check_overlap(void)
{
	int all_req_opts[ARG_COUNT] = { 0 };
	struct command *cmd1, *cmd2;
	int i, j, k;
	int r = 1;

	for (i = 0; i < COMMAND_COUNT; i++) {
		cmd1 = &commands[i];
		for (j = 0; j < cmd1->ro_count; j++)
			all_req_opts[cmd1->required_opt_args[j].opt] = 1;
	}

	for (i = 0; i < COMMAND_COUNT; i++) {

		cmd1 = &commands[i];

		if (cmd1->any_ro_count) {
			for (j = 0; j < cmd1->oo_count; j++) {
				for (k = 0; k < cmd1->any_ro_count; k++) {
					if (cmd1->optional_opt_args[j].opt ==
					    cmd1->required_opt_args[k + cmd1->ro_count].opt) {
						log_error("Option %s in command %s is required and optional!",
							  opt_names[cmd1->optional_opt_args[j].opt].long_opt,
							  command_enum(cmd1->command_enum));
						r = 0;
					}
				}
			}
			continue;
		}

		for (j = 0; j < COMMAND_COUNT; j++) {
			if (i == j)
				continue;

			cmd2 = &commands[j];

			if (cmd2->any_ro_count)
				continue;

			if (strcmp(cmd1->name, cmd2->name))
				continue;

			if (!_compare_cmds(cmd1, cmd2, all_req_opts))
				r = 0;
		}
	}
	return r;
}

#define STDOUT_BUF_SIZE	 (MAX_MAN_DESC + 4 * 1024)

int main(int argc, char *argv[])
{
	struct cmd_context cmdtool = { 0 };
	char *cmdname = NULL;
	char *desfile = NULL;
	char *stdout_buf;
	int primary = 0;
	int secondary = 0;
	int check = 0;
	int r = 0;
	size_t sz = STDOUT_BUF_SIZE;

	static struct option long_options[] = {
		{"primary", no_argument, 0, 'p' },
		{"secondary", no_argument, 0, 's' },
		{"check", no_argument, 0, 'c' },
		{0, 0, 0, 0 }
	};

	if (!(stdout_buf = malloc(sz)))
		log_error("Failed to allocate stdout buffer; carrying on with default buffering.");
	else
		setbuffer(stdout, stdout_buf, sz);

	while (1) {
		int c;
		int option_index = 0;

		c = getopt_long(argc, argv, "psc", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case '0':
			break;
		case 'p':
			primary = 1;
			break;
		case 's':
			secondary = 1;
			break;
		case 'c':
			check = 1;
			break;
		}
	}

	if (!primary && !secondary && !check) {
		log_error("Usage: %s --primary|--secondary|--check <command> [/path/to/description-file].", argv[0]);
		goto out_free;
	}

	if (optind < argc) {
		if (!(cmdname = strdup(argv[optind++]))) {
			log_error("Out of memory.");
			goto out_free;
		}
	} else if (!check) {
		log_error("Missing command name.");
		goto out_free;
	}

	if (optind < argc)
		desfile = argv[optind++];

	if (!define_commands(&cmdtool, NULL))
		goto out_free;

	factor_common_options();

	if (primary && cmdname)
		r = _print_man(cmdname, desfile, secondary);
	else if (secondary && cmdname) {
		r = 1;
		_print_man_secondary(cmdname);
	} else if (check) {
		r = _check_overlap();
	}

out_free:
	if (stdout_buf) {
		(void) fflush(stdout);
		setlinebuf(stdout);
		free(stdout_buf);
	}

	exit(r ? EXIT_SUCCESS: EXIT_FAILURE);
}
