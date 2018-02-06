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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>

#include "bcache.h"

#define SHOW_MOCK_CALLS 0

/*----------------------------------------------------------------
 * Assertions
 *--------------------------------------------------------------*/

static jmp_buf _test_k;
#define TEST_FAILED 1

static void _fail(const char *fmt, ...)
	__attribute__((format (printf, 1, 2)));


static void _fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");

	longjmp(_test_k, TEST_FAILED);
}

#define T_ASSERT(e) if (!(e)) {_fail("assertion failed: '%s'", # e);}

/*----------------------------------------------------------------
 * Mock engine
 *--------------------------------------------------------------*/
struct mock_engine {
	struct io_engine e;
	struct dm_list expected_calls;
	struct dm_list issued_io;
	unsigned max_io;
};

enum method {
	E_DESTROY,
	E_ISSUE,
	E_WAIT,
	E_MAX_IO
};

struct mock_call {
	struct dm_list list;
	enum method m;
};

struct mock_io {
	struct dm_list list;
	int fd;
	sector_t sb;
	sector_t se;
	void *data;
	void *context;
};

static const char *_show_method(enum method m)
{
	switch (m) {
	case E_DESTROY:
		return "destroy()";
	case E_ISSUE:
		return "issue()";
	case E_WAIT:
		return "wait()";
	case E_MAX_IO:
		return "max_io()";
	}

	return "<unknown>";
}

static void _expect(struct mock_engine *e, enum method m)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = m;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_read(struct mock_engine *e)
{
	// FIXME: finish
	_expect(e, E_ISSUE);
}

static void _expect_write(struct mock_engine *e)
{
	// FIXME: finish
	_expect(e, E_ISSUE);
}

static void _match(struct mock_engine *e, enum method m)
{
	struct mock_call *mc;

	if (dm_list_empty(&e->expected_calls))
		_fail("unexpected call to method %s\n", _show_method(m));

	mc = dm_list_item(e->expected_calls.n, struct mock_call);
	dm_list_del(&mc->list);

	if (mc->m != m)
		_fail("expected %s, but got %s\n", _show_method(mc->m), _show_method(m));
#if SHOW_MOCK_CALLS
	else
		fprintf(stderr, "%s called (expected)\n", _show_method(m));
#endif

	free(mc);
}

static void _no_outstanding_expectations(struct mock_engine *e)
{
	struct mock_call *mc;

	if (!dm_list_empty(&e->expected_calls)) {
		fprintf(stderr, "unsatisfied expectations:\n");
		dm_list_iterate_items (mc, &e->expected_calls)
			fprintf(stderr, "  %s\n", _show_method(mc->m));
	}
	T_ASSERT(dm_list_empty(&e->expected_calls));
}

static struct mock_engine *_to_mock(struct io_engine *e)
{
	return container_of(e, struct mock_engine, e);
}

static void _mock_destroy(struct io_engine *e)
{
	struct mock_engine *me = _to_mock(e);

	_match(me, E_DESTROY);
	T_ASSERT(dm_list_empty(&me->issued_io));
	T_ASSERT(dm_list_empty(&me->expected_calls));
	free(_to_mock(e));
}

static bool _mock_issue(struct io_engine *e, enum dir d, int fd,
	      		sector_t sb, sector_t se, void *data, void *context)
{
	struct mock_io *io;
	struct mock_engine *me = _to_mock(e);

	_match(me, E_ISSUE);
	io = malloc(sizeof(*io));
	if (!io)
		abort();

	io->fd = fd;
	io->sb = sb;
	io->se = se;
	io->data = data;
	io->context = context;

	dm_list_add(&me->issued_io, &io->list);
	return true;
}

static bool _mock_wait(struct io_engine *e, io_complete_fn fn)
{
	struct mock_io *io;
	struct mock_engine *me = _to_mock(e);
	_match(me, E_WAIT);

	// FIXME: provide a way to control how many are completed and whether
	// they error.
	T_ASSERT(!dm_list_empty(&me->issued_io));
	io = dm_list_item(me->issued_io.n, struct mock_io);
	dm_list_del(&io->list);
	fn(io->context, 0);
	return true;
}

static unsigned _mock_max_io(struct io_engine *e)
{
	struct mock_engine *me = _to_mock(e);
	_match(me, E_MAX_IO);
	return me->max_io;
}

static struct mock_engine *_mock_create(unsigned max_io)
{
	struct mock_engine *m = malloc(sizeof(*m));

	m->e.destroy = _mock_destroy;
	m->e.issue = _mock_issue;
	m->e.wait = _mock_wait;
	m->e.max_io = _mock_max_io;

	m->max_io = max_io;
	dm_list_init(&m->expected_calls);
	dm_list_init(&m->issued_io);

	return m;
}

/*----------------------------------------------------------------
 * Fixtures
 *--------------------------------------------------------------*/
struct fixture {
	struct mock_engine *me;
	struct bcache *cache;
};

static struct fixture *_fixture_init(unsigned nr_cache_blocks)
{
	struct fixture *f = malloc(sizeof(*f));

	f->me = _mock_create(16);
	T_ASSERT(f->me);

	_expect(f->me, E_MAX_IO);
	f->cache = bcache_create(128, nr_cache_blocks, &f->me->e);
	T_ASSERT(f->cache);

	return f;
}

static void _fixture_exit(struct fixture *f)
{
	_expect(f->me, E_DESTROY);
	bcache_destroy(f->cache);

	free(f);
}

static void *_small_fixture_init(void)
{
	return _fixture_init(16);
}

static void _small_fixture_exit(void *context)
{
	_fixture_exit(context);
}

static void *_large_fixture_init(void)
{
	return _fixture_init(1024);
}

static void _large_fixture_exit(void *context)
{
	_fixture_exit(context);
}

/*----------------------------------------------------------------
 * Tests
 *--------------------------------------------------------------*/
#define MEG 2048
#define SECTOR_SHIFT 9

static void good_create(sector_t block_size, unsigned nr_cache_blocks)
{
	struct bcache *cache;
	struct mock_engine *me = _mock_create(16);

	_expect(me, E_MAX_IO);
	cache = bcache_create(block_size, nr_cache_blocks, &me->e);
	T_ASSERT(cache);

	_expect(me, E_DESTROY);
	bcache_destroy(cache);
}

static void bad_create(sector_t block_size, unsigned nr_cache_blocks)
{
	struct bcache *cache;
	struct mock_engine *me = _mock_create(16);

	_expect(me, E_MAX_IO);
	cache = bcache_create(block_size, nr_cache_blocks, &me->e);
	T_ASSERT(!cache);

	_expect(me, E_DESTROY);
	me->e.destroy(&me->e);
}

static void test_create(void *fixture)
{
	good_create(8, 16);
}

static void test_nr_cache_blocks_must_be_positive(void *fixture)
{
	bad_create(8, 0);
}

static void test_block_size_must_be_positive(void *fixture)
{
	bad_create(0, 16);
}

static void test_block_size_must_be_multiple_of_page_size(void *fixture)
{
	static unsigned _bad_examples[] = {3, 9, 13, 1025};

	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(_bad_examples); i++)
		bad_create(_bad_examples[i], 16);

	for (i = 1; i < 1000; i++)
		good_create(i * 8, 16);
}

static void test_get_triggers_read(void *context)
{
	struct fixture *f = context;

	int fd = 17;   // arbitrary key
	struct block *b;

	_expect(f->me, E_ISSUE);
	_expect(f->me, E_WAIT);
	T_ASSERT(bcache_get(f->cache, fd, 0, 0, &b));
	bcache_put(b);
}

static void test_repeated_reads_are_cached(void *context)
{
	struct fixture *f = context;

	int fd = 17;   // arbitrary key
	unsigned i;
	struct block *b;

	_expect(f->me, E_ISSUE);
	_expect(f->me, E_WAIT);
	for (i = 0; i < 100; i++) {
		T_ASSERT(bcache_get(f->cache, fd, 0, 0, &b));
		bcache_put(b);
	}
}

static void test_block_gets_evicted_with_many_reads(void *context)
{
	struct fixture *f = context;

	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	const unsigned nr_cache_blocks = 16;

	int fd = 17;   // arbitrary key
	unsigned i;
	struct block *b;

	for (i = 0; i < nr_cache_blocks; i++) {
		_expect(me, E_ISSUE);
		_expect(me, E_WAIT);
		T_ASSERT(bcache_get(cache, fd, i, 0, &b));
		bcache_put(b);
	}

	// Not enough cache blocks to hold this one
	_expect(me, E_ISSUE);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, fd, nr_cache_blocks, 0, &b));
	bcache_put(b);

	// Now if we run through we should find one block has been
	// evicted.  We go backwards because the oldest is normally
	// evicted first.
	_expect(me, E_ISSUE);
	_expect(me, E_WAIT);
	for (i = nr_cache_blocks; i; i--) {
		T_ASSERT(bcache_get(cache, fd, i - 1, 0, &b));
		bcache_put(b);
	}
}

static void test_prefetch_issues_a_read(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	const unsigned nr_cache_blocks = 16;

	int fd = 17;   // arbitrary key
	unsigned i;
	struct block *b;

	for (i = 0; i < nr_cache_blocks; i++) {
		// prefetch should not wait
		_expect(me, E_ISSUE);
		bcache_prefetch(cache, fd, i);
	}


	for (i = 0; i < nr_cache_blocks; i++) {
		_expect(me, E_WAIT);
		T_ASSERT(bcache_get(cache, fd, i, 0, &b));
		bcache_put(b);
	}
}

static void test_too_many_prefetches_does_not_trigger_a_wait(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const unsigned nr_cache_blocks = 16;
	int fd = 17;   // arbitrary key
	unsigned i;

	for (i = 0; i < 10 * nr_cache_blocks; i++) {
		// prefetch should not wait
		if (i < nr_cache_blocks)
			_expect(me, E_ISSUE);
		bcache_prefetch(cache, fd, i);
	}

	// Destroy will wait for any in flight IO triggered by prefetches.
	for (i = 0; i < nr_cache_blocks; i++)
		_expect(me, E_WAIT);
}

static void test_dirty_data_gets_written_back(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const unsigned nr_cache_blocks = 16;
	int fd = 17;   // arbitrary key
	struct block *b;

	// FIXME: be specific about the IO direction
	// Expect the read
	_expect(me, E_ISSUE);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, fd, 0, GF_DIRTY, &b));
	bcache_put(b);

	// Expect the write
	_expect(me, E_ISSUE);
	_expect(me, E_WAIT);
}

static void test_zeroed_data_counts_as_dirty(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const unsigned nr_cache_blocks = 16;
	int fd = 17;   // arbitrary key
	struct block *b;

	// No read
	T_ASSERT(bcache_get(cache, fd, 0, GF_ZERO, &b));
	bcache_put(b);

	// Expect the write
	_expect(me, E_ISSUE);
	_expect(me, E_WAIT);
}

static void test_flush_waits_for_all_dirty(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const unsigned count = 16;
	int fd = 17;   // arbitrary key
	unsigned i;
	struct block *b;

	for (i = 0; i < count; i++) {
		if (i % 2) {
			T_ASSERT(bcache_get(cache, fd, i, GF_ZERO, &b));
		} else {
			_expect_read(me);
			_expect(me, E_WAIT);
			T_ASSERT(bcache_get(cache, fd, i, 0, &b));
		}
		bcache_put(b);
	}

	for (i = 0; i < count; i++) {
		if (i % 2)
			_expect_write(me);
	}

	for (i = 0; i < count; i++) {
		if (i % 2)
			_expect(me, E_WAIT);
	}

	bcache_flush(cache);
	_no_outstanding_expectations(me);
}

// Tests to be written
// Open multiple files and prove the blocks are coming from the correct file
// show invalidate works
// show invalidate_fd works
// show writeback is working
// check zeroing

struct test_details {
	const char *name;
	void (*fn)(void *);
	void *(*fixture_init)(void);
	void (*fixture_exit)(void *);
};

#define TEST(name, fn) {name, fn, NULL, NULL}
#define TEST_S(name, fn) {name, fn, _small_fixture_init, _small_fixture_exit}
#define TEST_L(name, fn) {name, fn, _large_fixture_init, _large_fixture_exit}

int main(int argc, char **argv)
{
	static struct test_details _tests[] = {
		TEST("simple create/destroy", test_create),
		TEST("nr cache blocks must be positive", test_nr_cache_blocks_must_be_positive),
		TEST("block size must be positive", test_block_size_must_be_positive),
		TEST("block size must be a multiple of page size", test_block_size_must_be_multiple_of_page_size),
		TEST_S("bcache_get() triggers read", test_get_triggers_read),
		TEST_S("repeated reads are cached", test_repeated_reads_are_cached),
		TEST_S("block get evicted with many reads", test_block_gets_evicted_with_many_reads),
		TEST_S("prefetch issues a read", test_prefetch_issues_a_read),
		TEST_S("too many prefetches does not trigger a wait", test_too_many_prefetches_does_not_trigger_a_wait),
		TEST_S("dirty data gets written back", test_dirty_data_gets_written_back),
		TEST_S("zeroed data counts as dirty", test_zeroed_data_counts_as_dirty),
		TEST_L("flush waits for all dirty", test_flush_waits_for_all_dirty),
	};

	// We have to declare these as volatile because of the setjmp()
	volatile unsigned i = 0, passed = 0;

	for (i = 0; i < DM_ARRAY_SIZE(_tests); i++) {
		void *fixture;
		struct test_details *t = _tests + i;
		fprintf(stderr, "[RUN    ] %s\n", t->name);

		if (setjmp(_test_k))
			fprintf(stderr, "[   FAIL] %s\n", t->name);
		else {
			if (t->fixture_init)
				fixture = t->fixture_init();
			else
				fixture = NULL;

			t->fn(fixture);

			if (t->fixture_exit)
				t->fixture_exit(fixture);

			passed++;
			fprintf(stderr, "[     OK] %s\n", t->name);
		}
	}

	fprintf(stderr, "\n%u/%lu tests passed\n", passed, DM_ARRAY_SIZE(_tests));

	return 0;
}
