/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

extern dm_log_fn _log;
#define log(msg, x...) _log(1, __FILE__, __LINE__, msg, ## x)

extern struct target *create_target(uint64_t start,
                                     uint64_t len,
                                     const char *type, const char *params);

int add_dev_node(const char *dev_name, dev_t dev);
int rm_dev_node(const char *dev_name);

