/*
 * Copyright (C) 2003 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_STR_LIST_H
#define _LVM_STR_LIST_H

#include "pool.h"

int str_list_add(struct pool *mem, struct list *sl, const char *str);
int str_list_del(struct list *sl, const char *str);

#endif
