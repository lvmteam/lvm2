/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_UUID_H
#define _LVM_UUID_H

#include "lvm-types.h"

#define ID_LEN 32

struct id {
	uint8_t uuid[ID_LEN];
};

int id_create(struct id *id);
int id_valid(struct id *id);
int id_cmp(struct id *lhs, struct id *rhs);

#endif
