/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_FILTER_REGEX_H
#define _LVM_FILTER_REGEX_H

#include "config.h"
#include "dev-cache.h"

/*
 * patterns must be an array of strings of the form:
 * [ra]<sep><regex><sep>, eg,
 * r/cdrom/          - reject cdroms
 * a|loop/[0-4]|     - accept loops 0 to 4
 * r|.*|             - reject everything else
 */

struct dev_filter *regex_filter_create(struct config_value *patterns);

#endif
