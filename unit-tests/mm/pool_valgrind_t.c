#include "libdevmapper.h"

#include <assert.h>

/*
 * Checks that valgrind is picking up unallocated pool memory as
 * uninitialised, even if the chunk has been recycled.
 *
 *     $ valgrind --track-origins=yes ./pool_valgrind_t
 *
 *     ==7023== Memcheck, a memory error detector
 *     ==7023== Copyright (C) 2002-2009, and GNU GPL'd, by Julian Seward et al.
 *     ==7023== Using Valgrind-3.6.0.SVN-Debian and LibVEX; rerun with -h for copyright info
 *     ==7023== Command: ./pool_valgrind_t
 *     ==7023==
 *     first branch worked (as expected)
 *     ==7023== Conditional jump or move depends on uninitialised value(s)
 *     ==7023==    at 0x4009AC: main (in /home/ejt/work/lvm2/unit-tests/mm/pool_valgrind_t)
 *     ==7023==  Uninitialised value was created by a client request
 *     ==7023==    at 0x4E40CB8: dm_pool_free (in /home/ejt/work/lvm2/libdm/ioctl/libdevmapper.so.1.02)
 *     ==7023==    by 0x4009A8: main (in /home/ejt/work/lvm2/unit-tests/mm/pool_valgrind_t)
 *     ==7023==
 *     second branch worked (valgrind should have flagged this as an error)
 *     ==7023==
 *     ==7023== HEAP SUMMARY:
 *     ==7023==     in use at exit: 0 bytes in 0 blocks
 *     ==7023==   total heap usage: 2 allocs, 2 frees, 2,104 bytes allocated
 *     ==7023==
 *     ==7023== All heap blocks were freed -- no leaks are possible
 *     ==7023==
 *     ==7023== For counts of detected and suppressed errors, rerun with: -v
 *     ==7023== ERROR SUMMARY: 1 errors from 1 contexts (suppressed: 4 from 4)
 */

#define COUNT 10

static void check_free()
{
        int i;
        char *blocks[COUNT];
        struct dm_pool *p = dm_pool_create("blah", 1024);

        for (i = 0; i < COUNT; i++)
                blocks[i] = dm_pool_alloc(p, 37);

        /* check we can access the last block */
        blocks[COUNT - 1][0] = 'E';
        if (blocks[COUNT - 1][0] == 'E')
                printf("first branch worked (as expected)\n");

        dm_pool_free(p, blocks[5]);

        if (blocks[COUNT - 1][0] == 'E')
                printf("second branch worked (valgrind should have flagged this as an error)\n");

        dm_pool_destroy(p);
}

/* Checks that freed chunks are marked NOACCESS */
static void check_free2()
{
	struct dm_pool *p = dm_pool_create("", 900); /* 900 will get
						      * rounded up to 1024,
						      * 1024 would have got
						      * rounded up to
						      * 2048 */
	char *data1, *data2;

	assert(p);
	data1 = dm_pool_alloc(p, 123);
	assert(data1);

	data1 = dm_pool_alloc(p, 1024);
	assert(data1);

	data2 = dm_pool_alloc(p, 123);
	assert(data2);

	data2[0] = 'A';		/* should work fine */

	dm_pool_free(p, data1);

	/*
	 * so now the first chunk is active, the second chunk has become
	 * the free one.
	 */
	data2[0] = 'B';		/* should prompt an invalid write error */

	dm_pool_destroy(p);
}

static void check_alignment()
{
	/*
	 * Pool always tries to allocate blocks with particular alignment.
	 * So there are potentially small gaps between allocations.  This
	 * test checks that valgrind is spotting illegal accesses to these
	 * gaps.
	 */

	int i, sum;
	struct dm_pool *p = dm_pool_create("blah", 1024);
	char *data1, *data2;
	char buffer[16];


	data1 = dm_pool_alloc_aligned(p, 1, 4);
	assert(data1);
	data2 = dm_pool_alloc_aligned(p, 1, 4);
	assert(data1);

	snprintf(buffer, sizeof(buffer), "%c", *(data1 + 1)); /* invalid read size 1 */
	dm_pool_destroy(p);
}

/*
 * Looking at the code I'm not sure allocations that are near the chunk
 * size are working.  So this test is trying to exhibit a specific problem.
 */
static void check_allocation_near_chunk_size()
{
	int i;
	char *data;
	struct dm_pool *p = dm_pool_create("", 900);

	/*
	 * allocate a lot and then free everything so we know there
	 * is a spare chunk.
	 */
	for (i = 0; i < 1000; i++) {
		data = dm_pool_alloc(p, 37);
		memset(data, 0, 37);
		assert(data);
	}

	dm_pool_empty(p);

	/* now we allocate something close to the chunk size ... */
	data = dm_pool_alloc(p, 1020);
	assert(data);
	memset(data, 0, 1020);

	dm_pool_destroy(p);
}

/* FIXME: test the dbg_malloc at exit (this test should be in dbg_malloc) */
static void check_leak_detection()
{
	int i;
	struct dm_pool *p = dm_pool_create("", 1024);

	for (i = 0; i < 10; i++)
		dm_pool_alloc(p, (i + 1) * 37);
}

/* we shouldn't get any errors from this one */
static void check_object_growth()
{
	int i;
	struct dm_pool *p = dm_pool_create("", 32);
	char data[100];
	void *obj;

	memset(data, 0, sizeof(data));

	dm_pool_begin_object(p, 43);
	for (i = 1; i < 100; i++)
		dm_pool_grow_object(p, data, i);
	obj = dm_pool_end_object(p);

	dm_pool_destroy(p);
}

int main(int argc, char **argv)
{
	check_free();
	check_free2();
	check_alignment();
	check_allocation_near_chunk_size();
	check_leak_detection();
	check_object_growth();
        return 0;
}
