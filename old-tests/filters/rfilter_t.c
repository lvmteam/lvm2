/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "filter-regex.h"
#include "config.h"
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
	struct config_file *cf;
	struct config_node *cn;
	struct dev_filter *filter;
	struct dev_iter *iter;
	struct device *dev;

	if (argc < 2) {
		fprintf(stderr, "Usage : %s <config_file>\n", argv[0]);
		exit(1);
	}

	init_log(stderr);
	init_debug(_LOG_DEBUG);

	if (!(cf = create_config_file())) {
		fprintf(stderr, "couldn't create config file\n");
		exit(1);
	}

	if (!read_config(cf, argv[1])) {
		fprintf(stderr, "couldn't read config file\n");
		exit(1);
	}

	if (!(cn = find_config_node(cf->root, "/devices/filter", '/'))) {
		fprintf(stderr, "couldn't find filter section\n");
		exit(1);
	}

	if (!dev_cache_init()) {
		fprintf(stderr, "couldn't initialise dev_cache_init failed\n");
		exit(1);
	}

	if (!dev_cache_add_dir("/dev")) {
		fprintf(stderr, "couldn't add '/dev' to dev_cache\n");
		exit(1);
	}

	if (!(filter = regex_filter_create(cn->v))) {
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
	destroy_config_file(cf);

	dump_memory();
	fin_log();
	return 0;
}

