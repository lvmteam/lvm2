/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
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
    uint32_t format_type;
    uint32_t checksum;
    uint16_t datalen;
    uint16_t pad;
};

struct filter_private
{
    void *mem;
    uint32_t format_type;
};


/* Calculate checksum */
static uint32_t calc_checksum(struct label *label)
{
    uint32_t csum = 0;
    int i;

    csum += label->magic;
    csum += label->format_type;
    csum += label->datalen;

    for (i=0; i<label->datalen; i++)
    {
	csum += label->data[i];
    }

    return csum;
}

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

    if (dev_open(dev, O_RDWR))
	return 0;

    if (label_pool == NULL)
	label_pool = pool_create(512);

    block = pool_alloc(label_pool, sectsize);
    if (!block)
    {
	stack;
	return 1;
    }
    ondisk = (struct label_ondisk *)block;

    status = dev_read(dev, sectsize*2, sectsize, block);

    /* If the first label is bad then use the second */
    for (iter = 0; iter <= 1; iter++)
    {
	if (iter == 0)
	    status = dev_read(dev, sectsize*2, sectsize, block);
	else
	    status = dev_read(dev, size*512 - sectsize, sizeof(struct label_ondisk) + label->datalen, block);

	if (!status)
	{
	    struct label incore;

	    /* Copy and convert endianness */
	    incore.magic = xlate32(ondisk->magic);
	    incore.checksum = xlate32(ondisk->checksum);
	    incore.datalen = xlate16(ondisk->datalen);
	    incore.data = block + sizeof(struct label_ondisk);

	    if (incore.magic != LABEL_MAGIC)
		continue;

	    /* Check Checksum */
	    if (incore.checksum != calc_checksum(&incore))
	    {
		log_error("Checksum %d does not match\n", iter);
		continue;
	    }
	    *label = incore;
	    label->data = pool_alloc(label_pool, incore.datalen);
	    if (!label->data)
	    {
		stack;
		return 1;
	    }
	    label->pool = label_pool;
	    if (label->data) memcpy(label->data, incore.data, incore.datalen);
	    pool_free(label_pool, block);
	    dev_close(dev);
	    return 1;
	}
    }

    pool_free(label_pool, block);
    dev_close(dev);

    return 0;
}

int label_write(struct device *dev, struct label *label)
{
    uint64_t size;
    uint32_t sectsize;
    char     *block;
    struct label_ondisk *ondisk;
    int       status1, status2;

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
	return 1;
    }

    ondisk = (struct label_ondisk *)block;

    /* Make into ondisk format */
    ondisk->magic = xlate32(LABEL_MAGIC);
    ondisk->format_type = xlate32(label->format_type);
    ondisk->datalen = xlate16(label->datalen);
    ondisk->checksum = xlate32(calc_checksum(label));
    memcpy(block+sizeof(struct label_ondisk), label->data, label->datalen);

    /* Write metadata to disk */
    if (!dev_open(dev, O_RDWR))
    {
	pool_free(label_pool, block);
	return 0;
    }

    status1 = dev_write(dev, sectsize*2, sizeof(struct label_ondisk) + label->datalen, block);

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
    pool_free(l.pool, l.data);

    return 1-status;
}

/* Check the device is labelled and has the right format_type */
static int _accept_format(struct dev_filter *f, struct device *dev)
{
    struct label l;
    int status;

    status = label_read(dev, &l);
    pool_free(l.pool, l.data);

    if (status && l.format_type == (uint32_t)f->private)
	return 1;

    else
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
struct dev_filter *label_format_filter_create(uint32_t format_type)
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
	fp->format_type = format_type;
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
