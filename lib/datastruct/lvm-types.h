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
#define PRIsize_t "Zu"

struct str_list {
	struct list list;
	const char *str;
};

#endif
