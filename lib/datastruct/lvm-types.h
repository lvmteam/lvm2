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
#if (SIZE_MAX == UINT64_MAX)
#define PRIsize_t PRIu64
#elif (SIZE_MAX == UINT32_MAX)
#define PRIsize_t PRIu32
#endif

struct str_list {
	struct list list;
	char *str;
};

#endif
