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

#ifndef _LVM_LVM_H
#define _LVM_LVM_H

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>

#include "pool.h"
#include "dbg_malloc.h"
#include "list.h"
#include "log.h"
#include "metadata.h"
#include "config.h"
#include "dev-cache.h"
#include "device.h"
#include "display.h"
#include "errors.h"
#include "filter.h"
#include "filter-persistent.h"
#include "filter-composite.h"
#include "filter-regex.h"
#include "format1.h"
#include "toollib.h"
#include "activate.h"

#define CMD_LEN 256
#define MAX_ARGS 64

/* command functions */
typedef int (*command_fn)(int argc, char **argv);

#define xx(a, b...) int a(int argc, char **argv);
#include "commands.h"
#undef xx

/* define the enums for the command line switches */
enum {
#define xx(a, b, c, d) a ,
#include "args.h"
#undef xx
};

typedef enum {
	SIGN_NONE = 0,
	SIGN_PLUS = 1,
	SIGN_MINUS = 2
} sign_t;
	
/* a global table of possible arguments */
struct arg {
	char short_arg;
	char *long_arg;
	int (*fn)(struct arg *a);
	int count;
	char *value;
	uint32_t i_value;
	sign_t sign;
};

extern struct arg the_args[ARG_COUNT + 1];

/* a register of the lvm commands */
struct command {
        const char *name;
        const char *desc;
        const char *usage;
        command_fn fn;

        int num_args;
        int *valid_args;
};

extern struct command *the_command;

void usage(const char *name);

/* the argument verify/normalise functions */
int yes_no_arg(struct arg *a);
int size_arg(struct arg *a);
int int_arg(struct arg *a);
int int_arg_with_sign(struct arg *a);
int string_arg(struct arg *a);
int permission_arg(struct arg *a);

char yes_no_prompt(const char *prompt, ...);

/* we use the enums to access the switches */
static inline int arg_count(int a) {
	return the_args[a].count;
}

static inline char *arg_value(int a) {
	return the_args[a].value;
}

static inline char *arg_str_value(int a, char *def)
{
	return arg_count(a) ? the_args[a].value : def;
}

static inline uint32_t arg_int_value(int a, uint32_t def)
{
	return arg_count(a) ? the_args[a].i_value : def;
}

static inline sign_t arg_sign_value(int a, sign_t def)
{
	return arg_count(a) ? the_args[a].sign : def;
}

static inline int arg_count_increment(int a)
{
	return the_args[a].count++;
}

static inline const char *command_name(void)
{
	return the_command->name;
}

extern struct format_instance *fid;

#endif
