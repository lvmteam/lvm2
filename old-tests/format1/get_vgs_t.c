/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "log.h"
#include "format1.h"
#include "dbg_malloc.h"
#include "pool.h"
#include "pretty_print.h"
#include "list.h"

#include <stdio.h>

int main(int argc, char **argv)
{
	struct io_space *ios;
	struct list_head *vgs;
	struct pool *mem;

	init_log(stderr);
	init_debug(_LOG_INFO);

	if (!dev_cache_init()) {
		fprintf(stderr, "init of dev-cache failed\n");
		exit(1);
	}

	if (!dev_cache_add_dir("/dev/loop")) {
		fprintf(stderr, "couldn't add /dev to dir-cache\n");
		exit(1);
	}

	if (!(mem = pool_create(10 * 1024))) {
		fprintf(stderr, "couldn't create pool\n");
		exit(1);
	}

	ios = create_lvm1_format("/dev", mem, NULL);

	if (!ios) {
		fprintf(stderr, "failed to create io_space for format1\n");
		exit(1);
	}

	vgs = ios->get_vgs(ios);

	if (!vgs) {
		fprintf(stderr, "couldn't read vg names\n");
		exit(1);
	}

	dump_vg_names(vgs, stdout);
	ios->destroy(ios);

	pool_destroy(mem);
	dev_cache_exit();
	dump_memory();
	fin_log();
	return 0;
}
