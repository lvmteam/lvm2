/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_TOOLS_H
#define _LVM_TOOLS_H

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "configure.h"
#include <assert.h>
#include "libdevmapper.h"

#include "lvm-types.h"
#include "lvm-logging.h"
#include "activate.h"
#include "archiver.h"
#include "lvmcache.h"
#include "config.h"
#include "defaults.h"
#include "dev-cache.h"
#include "device.h"
#include "display.h"
#include "errors.h"
#include "filter.h"
#include "filter-composite.h"
#include "filter-persistent.h"
#include "filter-regex.h"
#include "metadata-exported.h"
#include "locking.h"
#include "lvm-exec.h"
#include "lvm-file.h"
#include "lvm-string.h"
#include "segtype.h"
#include "str_list.h"
#include "toolcontext.h"
#include "toollib.h"

#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/types.h>

#define CMD_LEN 256
#define MAX_ARGS 64

/* command functions */
typedef int (*command_fn) (struct cmd_context * cmd, int argc, char **argv);

#define xx(a, b...) int a(struct cmd_context *cmd, int argc, char **argv);
#include "commands.h"
#undef xx

/* define the enums for the command line switches */
enum {
#define arg(a, b, c, d, e) a ,
#include "args.h"
#undef arg
};

typedef enum {
	SIGN_NONE = 0,
	SIGN_PLUS = 1,
	SIGN_MINUS = 2
} sign_t;

typedef enum {
	PERCENT_NONE = 0,
	PERCENT_VG,
	PERCENT_FREE,
	PERCENT_LV,
	PERCENT_PVS,
	PERCENT_ORIGIN
} percent_type_t;

#define ARG_COUNTABLE 0x00000001	/* E.g. -vvvv */
#define ARG_GROUPABLE 0x00000002	/* E.g. --addtag */

struct arg_values {
	unsigned count;
	char *value;
	int32_t i_value;
	uint32_t ui_value;
	int64_t i64_value;
	uint64_t ui64_value;
	sign_t sign;
	percent_type_t percent;
/*	void *ptr; // Currently not used. */
};

/* a global table of possible arguments */
struct arg_props {
	const char short_arg;
	char _padding[7];
	const char *long_arg;

	int (*fn) (struct cmd_context *cmd, struct arg_values *av);
	uint32_t flags;
};

struct arg_value_group_list {
        struct dm_list list;
        struct arg_values arg_values[0];
};

#define CACHE_VGMETADATA	0x00000001
#define PERMITTED_READ_ONLY 	0x00000002

/* a register of the lvm commands */
struct command {
	const char *name;
	const char *desc;
	const char *usage;
	command_fn fn;

	unsigned flags;

	int num_args;
	int *valid_args;
};

void usage(const char *name);

/* the argument verify/normalise functions */
int yes_no_arg(struct cmd_context *cmd, struct arg_values *av);
int yes_no_excl_arg(struct cmd_context *cmd, struct arg_values *av);
int size_kb_arg(struct cmd_context *cmd, struct arg_values *av);
int size_mb_arg(struct cmd_context *cmd, struct arg_values *av);
int int_arg(struct cmd_context *cmd, struct arg_values *av);
int int_arg_with_sign(struct cmd_context *cmd, struct arg_values *av);
int int_arg_with_sign_and_percent(struct cmd_context *cmd, struct arg_values *av);
int major_arg(struct cmd_context *cmd, struct arg_values *av);
int minor_arg(struct cmd_context *cmd, struct arg_values *av);
int string_arg(struct cmd_context *cmd, struct arg_values *av);
int tag_arg(struct cmd_context *cmd, struct arg_values *av);
int permission_arg(struct cmd_context *cmd, struct arg_values *av);
int metadatatype_arg(struct cmd_context *cmd, struct arg_values *av);
int units_arg(struct cmd_context *cmd, struct arg_values *av);
int segtype_arg(struct cmd_context *cmd, struct arg_values *av);
int alloc_arg(struct cmd_context *cmd, struct arg_values *av);
int readahead_arg(struct cmd_context *cmd, struct arg_values *av);
int metadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av);

/* we use the enums to access the switches */
unsigned arg_count(const struct cmd_context *cmd, int a);
unsigned arg_is_set(const struct cmd_context *cmd, int a);
const char *arg_value(struct cmd_context *cmd, int a);
const char *arg_str_value(struct cmd_context *cmd, int a, const char *def);
int32_t arg_int_value(struct cmd_context *cmd, int a, const int32_t def); 
uint32_t arg_uint_value(struct cmd_context *cmd, int a, const uint32_t def);
int64_t arg_int64_value(struct cmd_context *cmd, int a, const int64_t def);
uint64_t arg_uint64_value(struct cmd_context *cmd, int a, const uint64_t def);
const void *arg_ptr_value(struct cmd_context *cmd, int a, const void *def);
sign_t arg_sign_value(struct cmd_context *cmd, int a, const sign_t def);
percent_type_t arg_percent_value(struct cmd_context *cmd, int a, const percent_type_t def);
int arg_count_increment(struct cmd_context *cmd, int a);

unsigned grouped_arg_count(const struct arg_values *av, int a);
unsigned grouped_arg_is_set(const struct arg_values *av, int a);
const char *grouped_arg_str_value(const struct arg_values *av, int a, const char *def);

const char *command_name(struct cmd_context *cmd);

int pvmove_poll(struct cmd_context *cmd, const char *pv, unsigned background);
int lvconvert_poll(struct cmd_context *cmd, struct logical_volume *lv, unsigned background);

int mirror_remove_missing(struct cmd_context *cmd,
			  struct logical_volume *lv, int force);

#endif
