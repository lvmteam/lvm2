/*
 * Copyright (C) 2001 Sistina Software (UK) Limited
 *
 * This file is released under the GPL.
 */

/*
 * Random number generator snarfed from the
 * Stanford Graphbase.
 */

#ifndef _LVM_RANDOM_H
#define _LVM_RANDOM_H

#include "lvm-types.h"

void rand_init(int32_t seed);
int32_t rand_get(void);

/*
 * Note this will reset the seed.
 */
int rand_check(void);

#endif
