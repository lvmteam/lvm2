/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "lvm-types.h"
#include "dbg_malloc.h"

#include <stdarg.h>

#ifdef DEBUG_MEM

struct memblock {
	struct memblock *prev, *next;	/* All allocated blocks are linked */
	size_t length;		/* Size of the requested block */
	int id;			/* Index of the block */
	const char *file;	/* File that allocated */
	int line;		/* Line that allocated */
	void *magic;		/* Address of this block */
};

static struct {
	unsigned block_serialno;/* Non-decreasing serialno of block */
	unsigned blocks_allocated; /* Current number of blocks allocated */
	unsigned blocks_max;	/* Max no of concurrently-allocated blocks */
	unsigned int bytes, mbytes;

} _mem_stats = {
0, 0, 0, 0, 0};

static struct memblock *_head = 0;
static struct memblock *_tail = 0;

void *malloc_aux(size_t s, const char *file, int line)
{
	struct memblock *nb;
	size_t tsize = s + sizeof(*nb) + sizeof(unsigned long);

	if (s > 50000000) {
		log_error("Huge memory allocation (size %" PRIuPTR
			  ") rejected - metadata corruption?", s);
		return 0;
	}

	if (!(nb = malloc(tsize))) {
		log_error("couldn't allocate any memory, size = %" PRIuPTR, s);
		return 0;
	}

	/* set up the file and line info */
	nb->file = file;
	nb->line = line;

#ifdef BOUNDS_CHECK
	bounds_check();
#endif

	/* setup fields */
	nb->magic = nb + 1;
	nb->length = s;
	nb->id = ++_mem_stats.block_serialno;
	nb->next = 0;
	nb->prev = _tail;

	/* link to tail of the list */
	if (!_head)
		_head = _tail = nb;
	else {
		_tail->next = nb;
		_tail = nb;
	}

	/* stomp a pretty pattern across the new memory
	   and fill in the boundary bytes */
	{
		char *ptr = (char *) (nb + 1);
		size_t i;
		for (i = 0; i < s; i++)
			*ptr++ = i & 0x1 ? (char) 0xba : (char) 0xbe;

		for (i = 0; i < sizeof(unsigned long); i++)
			*ptr++ = (char) nb->id;
	}

	_mem_stats.blocks_allocated++;
	if (_mem_stats.blocks_allocated > _mem_stats.blocks_max)
		_mem_stats.blocks_max = _mem_stats.blocks_allocated;

	_mem_stats.bytes += s;
	if (_mem_stats.bytes > _mem_stats.mbytes)
		_mem_stats.mbytes = _mem_stats.bytes;

	return nb + 1;
}

void free_aux(void *p)
{
	char *ptr;
	size_t i;
	struct memblock *mb = ((struct memblock *) p) - 1;
	if (!p)
		return;

#ifdef BOUNDS_CHECK
	bounds_check();
#endif

	/* sanity check */
	assert(mb->magic == p);

	/* check data at the far boundary */
	ptr = ((char *) mb) + sizeof(struct memblock) + mb->length;
	for (i = 0; i < sizeof(unsigned long); i++)
		if (*ptr++ != (char) mb->id)
			assert(!"Damage at far end of block");

	/* have we freed this before ? */
	assert(mb->id != 0);
	mb->id = 0;

	/* stomp a different pattern across the memory */
	ptr = ((char *) mb) + sizeof(struct memblock);
	for (i = 0; i < mb->length; i++)
		*ptr++ = i & 1 ? (char) 0xde : (char) 0xad;

	/* unlink */
	if (mb->prev)
		mb->prev->next = mb->next;
	else
		_head = mb->next;

	if (mb->next)
		mb->next->prev = mb->prev;
	else
		_tail = mb->prev;

	assert(_mem_stats.blocks_allocated);
	_mem_stats.blocks_allocated--;
	_mem_stats.bytes -= mb->length;

	/* free the memory */
	free(mb);
}

void *realloc_aux(void *p, unsigned int s, const char *file, int line)
{
	void *r;
	struct memblock *mb = ((struct memblock *) p) - 1;

	r = malloc_aux(s, file, line);

	if (p) {
		memcpy(r, p, mb->length);
		free_aux(p);
	}

	return r;
}

int dump_memory(void)
{
	unsigned long tot = 0;
	struct memblock *mb;
	char str[32];
	size_t c;

	if (_head)
		log_very_verbose("You have a memory leak:");

	for (mb = _head; mb; mb = mb->next) {
		for (c = 0; c < sizeof(str) - 1; c++) {
			if (c >= mb->length)
				str[c] = ' ';
			else if (*(char *)(mb->magic + c) < ' ')
				str[c] = '?';
			else
				str[c] = *(char *)(mb->magic + c);
		}
		str[sizeof(str) - 1] = '\0';

		print_log(_LOG_INFO, mb->file, mb->line,
			  "block %d at %p, size %" PRIdPTR "\t [%s]",
			  mb->id, mb->magic, mb->length, str);
		tot += mb->length;
	}

	if (_head)
		log_very_verbose("%ld bytes leaked in total", tot);

	return 1;
}

void bounds_check(void)
{
	struct memblock *mb = _head;
	while (mb) {
		size_t i;
		char *ptr = ((char *) (mb + 1)) + mb->length;
		for (i = 0; i < sizeof(unsigned long); i++)
			if (*ptr++ != (char) mb->id)
				assert(!"Memory smash");

		mb = mb->next;
	}
}

#else

void *malloc_aux(size_t s, const char *file, int line)
{
	if (s > 50000000) {
		log_error("Huge memory allocation (size %" PRIuPTR
			  ") rejected - metadata corruption?", s);
		return 0;
	}

	return malloc(s);
}

#endif
