/*
 * Copyright (C) 2003 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_STR_LIST_H
#define _LVM_STR_LIST_H

#include "pool.h"

struct list *str_list_create(struct pool *mem);
int str_list_add(struct pool *mem, struct list *sll, const char *str);
int str_list_del(struct list *sll, const char *str);
int str_list_match_item(struct list *sll, const char *str);
int str_list_match_list(struct list *sll, struct list *sll2);

#endif
