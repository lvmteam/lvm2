/*
 * dbg_malloc.c
 *
 * Copyright (C) 2000, 2001 Sistina Software
 *
 * lvm is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * lvm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Changelog
 *
 *    9/11/2000 - First version by Joe Thornber
 *
 * TODO:
 *
 *  Thread safety seems to have fallen out, put lock back in.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "dbg_malloc.h"
#include "log/log.h"

struct memblock {
	struct memblock *prev, *next; /* All allocated blocks are linked */
	size_t length;                /* Size of the requested block */
	int id;                       /* Index of the block */
	const char *file;             /* File that allocated */
	int line;                     /* Line that allocated */
	void *magic;                  /* Address of this block */
};

static struct {
	unsigned int blocks, mblocks;
	unsigned int bytes, mbytes;

} _mem_stats = {0, 0, 0, 0};

static struct memblock *_head = 0;
static struct memblock *_tail = 0;

void *malloc_aux(size_t s, const char *file, int line)
{
	struct memblock *nb;
	size_t tsize = s + sizeof(*nb) + sizeof(unsigned long);

	if (!(nb = malloc(tsize))) {
		log_error("couldn't allocate any memory, size = %u", s);
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
	nb->id = ++_mem_stats.blocks;
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
		int i;
		for (i = 0; i < s; i++)
			*ptr++ = i & 0x1 ? (char) 0xba : (char) 0xbe;

		for (i = 0; i < sizeof(unsigned long); i++)
			*ptr++ = (char) nb->id;
	}

	if (_mem_stats.blocks > _mem_stats.mblocks)
		_mem_stats.mblocks = _mem_stats.blocks;

	_mem_stats.bytes += s;
	if (_mem_stats.bytes > _mem_stats.mbytes)
		_mem_stats.mbytes = _mem_stats.bytes;

	return nb + 1;
}

void free_aux(void *p)
{
	char *ptr;
	int i;
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
		if(*ptr++ != (char) mb->id)
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

	assert(_mem_stats.blocks);
	_mem_stats.blocks--;
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

#ifdef DEBUG_MEM
int dump_memory(void)
{
	unsigned long tot = 0;
	struct memblock *mb;
	if (_head)
		log_info("you have a memory leak:");

	for (mb = _head; mb; mb = mb->next) {
		print_log(_LOG_INFO, mb->file, mb->line, 
			  "block %d at %p, size %d",
			  mb->id, mb->magic, mb->length);
		tot += mb->length;
	}

	if (_head)
		log_info("%ld bytes leaked in total", tot);

	return 1;
}

void bounds_check(void)
{
	struct memblock *mb = _head;
	while (mb) {
		int i;
		char *ptr = ((char *) (mb + 1)) + mb->length;
		for (i = 0; i < sizeof(unsigned long); i++)
			if (*ptr++ != (char) mb->id)
				assert(!"Memory smash");

		mb = mb->next;
	}
}
#endif

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
