/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef LIB_DMTARGETS_H
#define LIB_DMTARGETS_H

#include "libdevmapper.h"

struct target {
	uint64_t start;
	uint64_t length;
	char *type;
	char *params;

	struct target *next;
};

struct dm_task {
	int type;
	char *dev_name;

	struct target *head, *tail;

	int read_only;
	int minor;
	struct dm_ioctl *dmi;
	char *newname;

	char *uuid;
};

#endif

