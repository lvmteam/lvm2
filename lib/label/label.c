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

/* Our memory pool */
static void *label_pool = NULL;

/* This is just the "struct label" with the data pointer removed */
struct label_ondisk
{
    uint32_t magic;
    uint32_t checksum;
    uint16_t datalen;

    char     disk_type[32];
    uint32_t version[3];
};

struct filter_private
{
    void *mem;
    char disk_type[32];
    uint32_t version[3];
    int version_match;
};


/* Calculate checksum */
static uint32_t calc_checksum(struct label *label)
{
    uint32_t csum = 0;
    int i;

    csum += label->magic;
    csum += label->version[0];
    csum += label->version[1];
    csum += label->version[2];
    csum += label->datalen;

    for (i=0; i<label->datalen; i++)
    {
	csum += label->data[i];
    }

    for (i=0; i<strlen(label->disk_type); i++)
    {
	csum += label->disk_type[i];
    }

    return csum;
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

    if (!dev_get_size(dev, &size))
	return 0;

    if (!dev_get_sectsize(dev, &sectsize))
	return 0;

    if (!dev_open(dev, O_RDONLY))
	return 0;

    if (label_pool == NULL)
	label_pool = pool_create(512);

    block = pool_alloc(label_pool, sectsize);
    if (!block)
    {
	stack;
	return 0;
    }
    ondisk = (struct label_ondisk *)block;

    /* If the first label is bad then use the second */
    for (iter = 0; iter <= 1; iter++)
    {
	if (iter == 0)
	    status = dev_read(dev, sectsize, sectsize, block);
	else
	    status = dev_read(dev, size*512 - sectsize, sectsize, block);

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
	    incore.magic = xlate32(ondisk->magic);
	    incore.version[0] = xlate32(ondisk->version[0]);
	    incore.version[1] = xlate32(ondisk->version[1]);
	    incore.version[2] = xlate32(ondisk->version[2]);
	    for (i=0; i<strlen(ondisk->disk_type)+1; i++)
		incore.disk_type[i] = ondisk->disk_type[i];
	    incore.checksum = xlate32(ondisk->checksum);
	    incore.datalen = xlate16(ondisk->datalen);
	    incore.data = block + sizeof(struct label_ondisk);

	    /* Make sure datalen is a sensible size too */
	    if (incore.datalen > sectsize)
		continue;

	    /* Check Checksum */
	    if (incore.checksum != calc_checksum(&incore))
	    {
		log_error("Checksum %d on device %s does not match. got %x, expected %x",
			  iter, dev_name(dev), incore.checksum, calc_checksum(&incore));
		continue;
	    }

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
    int       i;

    if (!dev_get_size(dev, &size))
	return 0;

    if (!dev_get_sectsize(dev, &sectsize))
	return 0;

    /* Can the metata fit in the remaining space ? */
    if (label->datalen > sectsize - sizeof(struct label_ondisk))
	return 0;

    if (label_pool == NULL)
	label_pool = pool_create(512);

    block = pool_alloc(label_pool, sizeof(struct label_ondisk) + label->datalen);
    if (!block)
    {
	stack;
	return 0;
    }

    ondisk = (struct label_ondisk *)block;

    /* Make into ondisk format */
    label->magic = LABEL_MAGIC;
    ondisk->magic = xlate32(LABEL_MAGIC);

    ondisk->version[0] = xlate32(label->version[0]);
    ondisk->version[1] = xlate32(label->version[1]);
    ondisk->version[2] = xlate32(label->version[2]);
    for (i=0; i<strlen(label->disk_type)+1; i++)
	ondisk->disk_type[i] = label->disk_type[i];
    ondisk->datalen = xlate16(label->datalen);
    ondisk->checksum = xlate32(calc_checksum(label));
    memcpy(block+sizeof(struct label_ondisk), label->data, label->datalen);

    /* Write metadata to disk */
    if (!dev_open(dev, O_RDWR))
    {
	pool_free(label_pool, block);
	return 0;
    }

    status1 = dev_write(dev, sectsize, sizeof(struct label_ondisk) + label->datalen, block);

    /* Write another at the end of the device */
    status2 = dev_write(dev, size*512 - sectsize, sizeof(struct label_ondisk) + label->datalen, block);

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

    if (!dev_get_size(dev, &size))
	return 0;

    if (!dev_get_sectsize(dev, &sectsize))
	return 0;

    if (label_pool == NULL)
	label_pool = pool_create(512);

    /* ALlocate some space for the blocks we are going to read in */
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

    /* Fetch em */
    if (!dev_open(dev, O_RDONLY))
	goto finish;

    if (!dev_read(dev, sectsize, sectsize, block1))
	goto finish;

    if (!dev_read(dev, size*512 - sectsize, sectsize, block2))
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

