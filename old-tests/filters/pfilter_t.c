/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "filter-persistent.h"
#include "log.h"
#include "dbg_malloc.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	struct dev_filter *filter;
	struct dev_iter *iter;
	struct device *dev;

	if (argc > 2) {
		fprintf(stderr, "Usage : %s <file>\n", argv[0]);
		exit(1);
	}

	init_log(stderr);
	init_debug(_LOG_DEBUG);

	if (!dev_cache_init()) {
		fprintf(stderr, "couldn't initialise dev_cache_init failed\n");
		exit(1);
	}

	if (!dev_cache_add_dir("/dev")) {
		fprintf(stderr, "couldn't add '/dev' to dev_cache\n");
		exit(1);
	}

	if (!(filter = persistent_filter_create("./pfilter.cfg", 1))) {
		fprintf(stderr, "couldn't build filter\n");
		exit(1);
	}

	if (!(iter = dev_iter_create(filter))) {
		log_err("couldn't create iterator");
		exit(1);
	}

	while ((dev = dev_iter_get(iter)))
		printf("%s\n", dev->name);

	dev_iter_destroy(iter);
	filter->destroy(filter);
	dev_cache_exit();

	dump_memory();
	fin_log();
	return 0;
}

