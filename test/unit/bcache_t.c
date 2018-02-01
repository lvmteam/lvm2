/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "units.h"
#include "bcache.h"

#define MEG 2048
#define SECTOR_SHIFT 9

static const char *_test_path = "test.bin";

int bcache_init(void)
{
	return 0;
}

int bcache_fini(void)
{
	return 0;
}

static int open_file(const char *path)
{
	return open(path, O_EXCL | O_RDWR | O_DIRECT, 0666);
}

static int _prep_file(const char *path)
{
	int fd, r;

	fd = open(path, O_CREAT | O_TRUNC | O_EXCL | O_RDWR | O_DIRECT, 0666);
	if (fd < 0)
		return -1;

	r = fallocate(fd, FALLOC_FL_ZERO_RANGE, 0, (16 * MEG) << SECTOR_SHIFT);
	if (r) {
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


static int test_init(void)
{
	unlink(_test_path);
	return _prep_file(_test_path);
}

static int test_exit(void)
{
	unlink(_test_path);
	return 0;
}

static void test_create(void)
{
	struct bcache *cache = bcache_create(8, 16);
	CU_ASSERT_PTR_NOT_NULL(cache);
	bcache_destroy(cache);
}

static void test_nr_cache_blocks_must_be_positive(void)
{
	struct bcache *cache = bcache_create(8, 0);
	CU_ASSERT_PTR_NULL(cache);
}

static void test_block_size_must_be_positive(void)
{
	struct bcache *cache = bcache_create(0, 16);
	CU_ASSERT_PTR_NULL(cache);
}

static void test_block_size_must_be_multiple_of_page_size(void)
{
	unsigned i;
	struct bcache *cache;

	{
		static unsigned _bad_examples[] = {3, 9, 13, 1025};

		for (i = 0; i < DM_ARRAY_SIZE(_bad_examples); i++) {
			cache = bcache_create(_bad_examples[i], 16);
			CU_ASSERT_PTR_NULL(cache);
		}
	}

	{
		// Only testing a few sizes because io_destroy is seriously
		// slow.
		for (i = 1; i < 25; i++) {
			cache = bcache_create(8 * i, 16);
			CU_ASSERT_PTR_NOT_NULL(cache);
			bcache_destroy(cache);
		}
	}
}

static void test_reads_work(void)
{
	int fd;

	// FIXME: add fixtures.
	test_init();
	fd = open_file("./test.bin");
	CU_ASSERT(fd >= 0);

	{
		int i;
		struct block *b;
		struct bcache *cache = bcache_create(8, 16);

		CU_ASSERT(bcache_get(cache, fd, 0, 0, &b));
		for (i = 0; i < 8 << SECTOR_SHIFT; i++)
			CU_ASSERT(((unsigned char *) b->data)[i] == 0);
		bcache_put(b);

		bcache_destroy(cache);
	}

	close(fd);

	test_exit();
}

static void test_prefetch_works(void)
{
	int fd;

	// FIXME: add fixtures.
	test_init();
	fd = open_file("./test.bin");
	CU_ASSERT(fd >= 0);

	{
		int i;
		struct block *b;
		struct bcache *cache = bcache_create(8, 16);

		for (i = 0; i < 16; i++)
			bcache_prefetch(cache, fd, i);

		for (i = 0; i < 16; i++) {
			CU_ASSERT(bcache_get(cache, fd, i, 0, &b));
			bcache_put(b);
		}

		bcache_destroy(cache);
	}

	close(fd);

	test_exit();
}

CU_TestInfo bcache_list[] = {
	{ (char*)"create", test_create },
	{ (char*)"nr cache block must be positive", test_nr_cache_blocks_must_be_positive },
	{ (char*)"block size must be positive", test_block_size_must_be_positive },
	{ (char*)"block size must be multiple of page size", test_block_size_must_be_multiple_of_page_size },
	{ (char*)"reads work", test_reads_work },
	{ (char*)"prefetch works", test_prefetch_works },
	CU_TEST_INFO_NULL
};
