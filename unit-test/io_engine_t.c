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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "bcache.h"
#include "framework.h"
#include "units.h"

//----------------------------------------------------------------

#define SECTOR_SIZE 512
#define BLOCK_SIZE_SECTORS 64

struct fixture {
	struct io_engine *e;
	void *data;

	char fname[64];
	int fd;
};

static void *_fix_init(void)
{
        struct fixture *f = malloc(sizeof(*f));

        T_ASSERT(f);
        f->e = create_async_io_engine();
        T_ASSERT(f->e);
        f->data = aligned_alloc(4096, SECTOR_SIZE * BLOCK_SIZE_SECTORS);
        T_ASSERT(f->data);

        snprintf(f->fname, sizeof(f->fname), "unit-test-XXXXXX");
	f->fd = mkostemp(f->fname, O_RDWR | O_CREAT | O_EXCL);
	T_ASSERT(f->fd >= 0);

	memset(f->data, 0, SECTOR_SIZE * BLOCK_SIZE_SECTORS);
	write(f->fd, f->data, SECTOR_SIZE * BLOCK_SIZE_SECTORS);
	lseek(f->fd, 0, SEEK_SET);
        return f;
}

static void _fix_exit(void *fixture)
{
        struct fixture *f = fixture;

	close(f->fd);
	unlink(f->fname);
        free(f->data);
        f->e->destroy(f->e);
        free(f);
}

static void _test_create(void *fixture)
{
	// empty
}

struct io {
	bool completed;
	int error;
};

static void _io_init(struct io *io)
{
	io->completed = false;
	io->error = 0;
}

static void _complete_io(void *context, int io_error)
{
	struct io *io = context;
	io->completed = true;
	io->error = io_error;
}

static void _test_read(void *fixture)
{
	struct fixture *f = fixture;

	struct io io;

	_io_init(&io);
	T_ASSERT(f->e->issue(f->e, DIR_READ, f->fd, 0, BLOCK_SIZE_SECTORS, f->data, &io));
	T_ASSERT(f->e->wait(f->e, _complete_io));
	T_ASSERT(io.completed);
	T_ASSERT(!io.error);
}

static void _test_write(void *fixture)
{
	struct fixture *f = fixture;

	struct io io;

	_io_init(&io);
	T_ASSERT(f->e->issue(f->e, DIR_WRITE, f->fd, 0, BLOCK_SIZE_SECTORS, f->data, &io));
	T_ASSERT(f->e->wait(f->e, _complete_io));
	T_ASSERT(io.completed);
	T_ASSERT(!io.error);
}

//----------------------------------------------------------------

#define T(path, desc, fn) register_test(ts, "/base/device/bcache/io-engine/" path, desc, fn)

static struct test_suite *_tests(void)
{
        struct test_suite *ts = test_suite_create(_fix_init, _fix_exit);
        if (!ts) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }

        T("create-destroy", "simple create/destroy", _test_create);
        T("create-read", "read sanity check", _test_read);
        T("create-write", "write sanity check", _test_write);

        return ts;
}

void io_engine_tests(struct dm_list *all_tests)
{
	dm_list_add(all_tests, &_tests()->list);
}

