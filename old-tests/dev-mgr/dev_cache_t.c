/*
 * Copyright (C) 2001 Sistina Software (UK) Limited
 *
 * This file is released under the GPL.
 */

#include "dev-cache.h"
#include "log.h"

#include <stdio.h>

int main(int argc, char **argv)
{
	int i;
	struct device *dev;
	struct dev_iter *iter;

	init_log();
	if (!dev_cache_init()) {
		log_error("couldn't initialise dev_cache_init failed\n");
		exit(1);
	}

	for (i = 1; i < argc; i++) {
		if (!dev_cache_add_dir(argv[i])) {
			log_error("couldn't add '%s' to dev_cache\n");
			exit(1);
		}
	}

	if (!(iter = dev_iter_create(NULL))) {
		log_error("couldn't create iterator\n");
		exit(1);
	}

	while ((dev = dev_iter_next(iter)))
		printf("%s\n", dev->name);

	dev_iter_destroy(iter):
	dev_cache_exit();

	dump_memory();
	fin_log();
	return 0;
}
