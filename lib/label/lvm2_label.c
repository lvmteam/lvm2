/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "device.h"
#include "dev-cache.h"
#include "log.h"
#include "filter.h"
#include "label.h"
#include "pool.h"
#include "xlate.h"

/* Label Magic is "LnXl" - error: imagination failure */
#define LABEL_MAGIC 0x6c586e4c

/* Size of blocks that dev_get_size() returns the number of */
#define BLOCK_SIZE 512

/* Our memory pool */
static void *label_pool = NULL;

/* This is just the "struct label" with the data pointer removed */
struct label_ondisk
{
    uint32_t magic;
    uint32_t crc;
    uint64_t label1_loc;
    uint64_t label2_loc;
    uint16_t datalen;
    uint16_t pad;

    uint32_t version[3];
    char     disk_type[32];
};

struct filter_private
{
    void *mem;
    char disk_type[32];
    uint32_t version[3];
    int version_match;
};


/* Calculate CRC32 of a buffer */
static uint32_t crc32(uint32_t initial, const unsigned char *databuf, size_t datalen)
{
    uint32_t idx, crc = initial;
    static const u_int crctab[] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };

    for (idx = 0; idx < datalen; idx++) {
        crc ^= *databuf++;
        crc = (crc >> 4) ^ crctab[crc & 0xf];
        crc = (crc >> 4) ^ crctab[crc & 0xf];
    }
    return crc;
}

/* Calculate crc */
static uint32_t calc_crc(struct label *label)
{
    uint32_t crcval = 0xffffffff;

    crcval = crc32(crcval, (char *)&label->magic, sizeof(label->magic));
    crcval = crc32(crcval, (char *)&label->datalen, sizeof(label->datalen));
    crcval = crc32(crcval, (char *)label->disk_type, strlen(label->disk_type));
    crcval = crc32(crcval, (char *)&label->version, sizeof(label->version));
    crcval = crc32(crcval, (char *)label->data, label->datalen);

    return crcval;
}

/* Calculate the locations we should find the labels in */
static inline void get_label_locations(uint64_t size, uint32_t sectsize, long *first, long *second)
{
    *first  = sectsize;
    *second = size*BLOCK_SIZE - sectsize;
}

/* Read a label off disk - the data area is allocated
   from the pool in label->pool and should be freed by
   the caller */
int label_read(struct device *dev, struct label *label)
{
    uint64_t size;
    uint32_t sectsize;
    char     *block;
    struct   label_ondisk *ondisk;
    int      status;
    int      iter;
    long     offset[2];

    if (!dev_get_size(dev, &size))
	return 0;

    if (!dev_get_sectsize(dev, &sectsize))
	return 0;

    if (!dev_open(dev, O_RDONLY))
	return 0;

    if (label_pool == NULL)
	label_pool = pool_create(BLOCK_SIZE);

    block = pool_alloc(label_pool, sectsize);
    if (!block)
    {
	stack;
	return 0;
    }
    ondisk = (struct label_ondisk *)block;
    get_label_locations(size, sectsize, &offset[0], &offset[1]);

    /* If the first label is bad then use the second */
    for (iter = 0; iter <= 1; iter++)
    {
	status = dev_read(dev, offset[iter], sectsize, block);
	if (status)
	{
	    struct label incore;
	    int i;
	    int found_nul;

	    /* If the MAGIC doesn't match there's no point in
	       carrying on */
	    if (xlate32(ondisk->magic) != LABEL_MAGIC)
		continue;

	    /* Look for a NUL in the disk_type string so we don't
	       SEGV is something has gone horribly wrong */
	    found_nul = 0;
	    for (i=0; i<sizeof(ondisk->disk_type); i++)
		if (ondisk->disk_type[i] == '\0')
		    found_nul = 1;

	    if (!found_nul)
		continue;

            /* Copy and convert endianness */
	    strncpy(incore.disk_type, ondisk->disk_type, sizeof(incore.disk_type));
	    incore.magic = xlate32(ondisk->magic);
	    incore.version[0] = xlate32(ondisk->version[0]);
	    incore.version[1] = xlate32(ondisk->version[1]);
	    incore.version[2] = xlate32(ondisk->version[2]);
	    incore.label1_loc = xlate64(ondisk->label1_loc);
	    incore.label2_loc = xlate64(ondisk->label2_loc);
	    incore.datalen = xlate16(ondisk->datalen);
	    incore.data = block + sizeof(struct label_ondisk);
	    incore.crc = xlate32(ondisk->crc);

	    /* Make sure datalen is a sensible size too */
	    if (incore.datalen > sectsize)
		continue;

	    /* Check Crc */
	    if (incore.crc != calc_crc(&incore))
	    {
		log_error("Crc %d on device %s does not match. got %x, expected %x",
			  iter, dev_name(dev), incore.crc, calc_crc(&incore));
		continue;
	    }

	    /* Check label locations match our view of the device */
	    if (incore.label1_loc != offset[0])
		log_error("Label 1 location is wrong in label %d - check block size of the device\n",
			  iter);
	    if (incore.label2_loc != offset[1])
		log_error("Label 2 location is wrong in label %d - the size of the device must have changed\n",
			  iter);

	    /* Copy to user's data area */
	    *label = incore;
	    label->data = pool_alloc(label_pool, incore.datalen);
	    if (!label->data)
	    {
		stack;
		return 0;
	    }
	    memcpy(label->data, incore.data, incore.datalen);

	    pool_free(label_pool, block);
	    dev_close(dev);
	    return 1;
	}
    }

    pool_free(label_pool, block);
    dev_close(dev);

    return 0;
}

/* Write a label to a device */
int label_write(struct device *dev, struct label *label)
{
    uint64_t size;
    uint32_t sectsize;
    char     *block;
    struct label_ondisk *ondisk;
    int       status1, status2;
    long      offset[2];

    if (!dev_get_size(dev, &size))
	return 0;

    if (!dev_get_sectsize(dev, &sectsize))
	return 0;

    /* Can the metata fit in the remaining space ? */
    if (label->datalen > sectsize - sizeof(struct label_ondisk))
	return 0;

    if (label_pool == NULL)
	label_pool = pool_create(BLOCK_SIZE);

    block = pool_alloc(label_pool, sizeof(struct label_ondisk) + label->datalen);
    if (!block)
    {
	stack;
	return 0;
    }
    ondisk = (struct label_ondisk *)block;

    /* Make sure the label has the right magic number in it */
    label->magic = LABEL_MAGIC;
    label->label1_loc = offset[0];
    label->label2_loc = offset[1];

    get_label_locations(size, sectsize, &offset[0], &offset[1]);

    /* Make into ondisk format */
    ondisk->magic = xlate32(LABEL_MAGIC);
    ondisk->version[0] = xlate32(label->version[0]);
    ondisk->version[1] = xlate32(label->version[1]);
    ondisk->version[2] = xlate32(label->version[2]);
    ondisk->label1_loc = xlate64(offset[0]);
    ondisk->label2_loc = xlate64(offset[1]);
    ondisk->datalen = xlate16(label->datalen);
    strncpy(ondisk->disk_type, label->disk_type, sizeof(ondisk->disk_type));
    memcpy(block+sizeof(struct label_ondisk), label->data, label->datalen);
    ondisk->crc = xlate32(calc_crc(label));

    /* Write metadata to disk */
    if (!dev_open(dev, O_RDWR))
    {
	pool_free(label_pool, block);
	return 0;
    }

    status1 = dev_write(dev, offset[0], sizeof(struct label_ondisk) + label->datalen, block);
    if (!status1)
	log_error("Error writing label 1\n");

    /* Write another at the end of the device */
    status2 = dev_write(dev, offset[1], sizeof(struct label_ondisk) + label->datalen, block);
    if (!status2)
    {
	char zerobuf[sizeof(struct label_ondisk)];
	log_error("Error writing label 2\n");

	/* Wipe the first label so it doesn't get confusing */
	memset(zerobuf, 0, sizeof(struct label_ondisk));
	if (!dev_write(dev, offset[0], sizeof(struct label_ondisk), zerobuf))
	    log_error("Error erasing label 1\n");
    }

    pool_free(label_pool, block);
    dev_close(dev);

    return ((status1 != 0) && (status2 != 0));
}



/* Return 1 for Yes, 0 for No */
int is_labelled(struct device *dev)
{
    struct label l;
    int status;

    status = label_read(dev, &l);
    if (status) label_free(&l);

    return status;
}

/* Check the device is labelled and has the right format_type */
static int _accept_format(struct dev_filter *f, struct device *dev)
{
    struct label l;
    int status;
    struct filter_private *fp = (struct filter_private *) f->private;

    status = label_read(dev, &l);
    if (status) label_free(&l);

    if (status)
    {
	if (strcmp(l.disk_type, fp->disk_type) == 0)
	{
	    switch (fp->version_match)
	    {
	    case VERSION_MATCH_EQUAL:
		if (l.version[0] == fp->version[0] &&
		    l.version[1] == fp->version[1] &&
		    l.version[2] == fp->version[2])
		    return 1;
		break;

	    case VERSION_MATCH_LESSTHAN:
		if (l.version[0] == fp->version[0] &&
		    l.version[1] <  fp->version[1])
		    return 1;
		break;

	    case VERSION_MATCH_LESSEQUAL:
		if (l.version[0] == fp->version[0] &&
		    l.version[1] <=  fp->version[1])
		    return 1;
		break;

	    case VERSION_MATCH_ANY:
		return 1;
	    }
	}
    }
    return 0;
}

/* We just want to know if it's labelled or not */
static int _accept_label(struct dev_filter *f, struct device *dev)
{
    return is_labelled(dev);
}

static void _destroy(struct dev_filter *f)
{
    struct filter_private *fp = (struct filter_private *) f->private;
    pool_destroy(fp->mem);
}

/* A filter to find devices with a particular label type on them */
struct dev_filter *label_format_filter_create(char *disk_type, uint32_t version[3], int match_type)
{
        struct pool *mem;
	struct filter_private *fp;
	struct dev_filter *f;

	/* Validate the match type */
	if (match_type != VERSION_MATCH_EQUAL &&
	    match_type != VERSION_MATCH_LESSTHAN &&
	    match_type != VERSION_MATCH_LESSEQUAL &&
	    match_type != VERSION_MATCH_ANY)
	    return 0;

	mem = pool_create(10 * 1024);
	if (!mem) {
		stack;
		return NULL;
	}

	if (!(f = pool_zalloc(mem, sizeof(*f)))) {
	    stack;
	    goto bad;
	}

	if (!(fp = pool_zalloc(mem, sizeof(*fp)))) {
	    stack;
	    goto bad;
	}

	fp->mem = mem;
	strcpy(fp->disk_type, disk_type);
	fp->version[0] = version[0];
	fp->version[1] = version[1];
	fp->version[2] = version[2];
	fp->version_match = match_type;
	f->passes_filter = _accept_format;
	f->destroy = _destroy;
	f->private = fp;

	return f;

 bad:
	pool_destroy(mem);
	return NULL;
}

/* A filter to find devices with any label on them */
struct dev_filter *label_filter_create()
{
	struct pool *mem = pool_create(10 * 1024);
	struct filter_private *fp;
	struct dev_filter *f;

	if (!mem) {
		stack;
		return NULL;
	}

	if (!(f = pool_zalloc(mem, sizeof(*f)))) {
	    stack;
	    goto bad;
	}

	if (!(fp = pool_zalloc(mem, sizeof(*fp)))) {
	    stack;
	    goto bad;
	}

	fp->mem = mem;
	f->passes_filter = _accept_label;
	f->destroy = _destroy;
	f->private = fp;

	return f;

 bad:
	pool_destroy(mem);
	return NULL;
}

/* Return 1 if both labels are identical, 0 if not or there was an error */
int labels_match(struct device *dev)
{
    uint64_t size;
    uint32_t sectsize;
    char     *block1;
    char     *block2;
    struct   label_ondisk *ondisk1;
    struct   label_ondisk *ondisk2;
    int      status = 0;
    long     offset[2];

    if (!dev_get_size(dev, &size))
	return 0;

    if (!dev_get_sectsize(dev, &sectsize))
	return 0;

    if (label_pool == NULL)
	label_pool = pool_create(BLOCK_SIZE);

/* Allocate some space for the blocks we are going to read in */
    block1 = pool_alloc(label_pool, sectsize);
    if (!block1)
    {
	stack;
	return 0;
    }

    block2 = pool_alloc(label_pool, sectsize);
    if (!block2)
    {
	stack;
	pool_free(label_pool, block1);
	return 0;
    }
    ondisk1 = (struct label_ondisk *)block1;
    ondisk2 = (struct label_ondisk *)block2;

    get_label_locations(size, sectsize, &offset[0], &offset[1]);

    /* Fetch em */
    if (!dev_open(dev, O_RDONLY))
	goto finish;

    if (!dev_read(dev, offset[0], sectsize, block1))
	goto finish;

    if (!dev_read(dev, offset[1], sectsize, block2))
	goto finish;

    dev_close(dev);

    /* Is it labelled? */
    if (xlate32(ondisk1->magic) != LABEL_MAGIC)
	goto finish;

    /* Compare the whole structs */
    if (memcmp(ondisk1, ondisk2, sizeof(struct label_ondisk)) != 0)
	goto finish;

    /* OK, check the data area */
    if (memcmp(block1 + sizeof(struct label_ondisk),
	       block2 + sizeof(struct label_ondisk),
	       xlate16(ondisk1->datalen)) != 0)
	goto finish;

    /* They match !! */
    status = 1;

 finish:
    pool_free(label_pool, block2);
    pool_free(label_pool, block1);

    return status;
}

/* Free data area allocated by label_read() */
void label_free(struct label *label)
{
    if (label->data)
	pool_free(label_pool, label->data);
}

