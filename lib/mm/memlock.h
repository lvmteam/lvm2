/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef LVM_MEMLOCK_H
#define LVM_MEMLOCK_H

struct cmd_context;

void memlock_inc(void);
void memlock_dec(void);
int memlock(void);
void memlock_init(struct cmd_context *cmd);

#endif
