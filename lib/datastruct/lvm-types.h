/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_TYPES_H
#define _LVM_TYPES_H

#include "list.h"

#include <sys/types.h>
#include <inttypes.h>

/* Define some portable printing types */
#if __WORDSIZE == 64
#define PRIsize_t "lu"
#else
#define PRIsize_t "u"
#endif

struct str_list {
	struct list list;
	char *str;
};

#endif
