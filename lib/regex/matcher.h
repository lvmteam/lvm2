/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_MATCHER_H
#define _LVM_MATCHER_H

#include "pool.h"

struct matcher;
struct matcher *matcher_create(struct pool *mem,
			       const char **patterns, int num);

int matcher_run(struct matcher *m, const char *begin, const char *end);

#endif
