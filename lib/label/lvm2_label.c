/*
 * Copyright (C) 2001-2002 Sistina Software
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
#include "pool.h"
#include "dbg_malloc.h"
#include "filter.h"
#include "label.h"
#include "lvm2_label.h"
#include "xlate.h"

/* Label Magic is "LnXl" - error: imagination failure */
#define LABEL_MAGIC 0x6c586e4c

/* Size of blocks that dev_get_size() returns the number of */
#define BLOCK_SIZE 512

/* This is just the "struct lvm2_label" with the data pointer removed */
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
    static const u_int crctab[] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };
    uint32_t idx, crc = initial;

    for (idx = 0; idx < datalen; idx++) {
        crc ^= *databuf++;
        crc = (crc >> 4) ^ crctab[crc & 0xf];
        crc = (crc >> 4) ^ crctab[crc & 0xf];
    }
    return crc;
}

/* Calculate crc */
static uint32_t calc_crc(struct label_ondisk *label, char *data)
{
    uint32_t crcval = 0xffffffff;

    crcval = crc32(crcval, (char *)&label->magic, sizeof(label->magic));
    crcval = crc32(crcval, (char *)&label->label1_loc, sizeof(label->label1_loc));
    crcval = crc32(crcval, (char *)&label->label2_loc, sizeof(label->label2_loc));
    crcval = crc32(crcval, (char *)&label->datalen, sizeof(label->datalen));
    crcval = crc32(crcval, (char *)&label->version, sizeof(label->version));
    crcval = crc32(crcval, (char *)label->disk_type, strlen(label->disk_type));
    crcval = crc32(crcval, (char *)data, label->datalen);

    return crcval;
}

/* Calculate the locations we should find the labels in */
static inline void get_label_locations(uint64_t size, uint32_t sectsize, long *first, long *second)
{
    *first  = sectsize;
    *second = size*BLOCK_SIZE - sectsize;
}

/* Read a label off disk */
static int lvm2_label_read(struct labeller *l, struct device *dev, struct label **label)
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

    block = dbg_malloc(sectsize);
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
	    struct label *incore;
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

	    incore = dbg_malloc(sizeof(struct label));
	    if (incore == NULL)
	    {
		return 0;
	    }

            /* Copy and convert endianness */
	    strncpy(incore->volume_type, ondisk->disk_type, sizeof(incore->volume_type));
	    incore->version[0] = xlate32(ondisk->version[0]);
	    incore->version[1] = xlate32(ondisk->version[1]);
	    incore->version[2] = xlate32(ondisk->version[2]);
	    incore->extra_len  = xlate16(ondisk->datalen);
	    incore->extra_info = block + sizeof(struct label_ondisk);

	    /* Make sure datalen is a sensible size too */
	    if (incore->extra_len > sectsize)
		continue;

	    /* Check Crc */
	    if (xlate32(ondisk->crc) != calc_crc(ondisk, incore->extra_info))
	    {
		log_error("Crc %d on device %s does not match. got %x, expected %x",
			  iter, dev_name(dev), xlate32(ondisk->crc), calc_crc(ondisk, incore->extra_info));
		continue;
	    }

	    /* Check label locations match our view of the device */
	    if (xlate64(ondisk->label1_loc) != offset[0])
		log_error("Label 1 location is wrong in label %d - check block size of the device\n",
			  iter);
	    if (xlate64(ondisk->label2_loc) != offset[1])
		log_error("Label 2 location is wrong in label %d - the size of the device must have changed\n",
			  iter);

	    /* Copy to user's data area */
	    *label = incore;
	    incore->extra_info = dbg_malloc(incore->extra_len);
	    if (!incore->extra_info)
	    {
		stack;
		return 0;
	    }
	    memcpy(incore->extra_info, block + sizeof(struct label_ondisk), incore->extra_len);

	    dbg_free(block);
	    dev_close(dev);
	    return 1;
	}
    }

    dbg_free(block);
    dev_close(dev);

    return 0;
}

/* Write a label to a device */
static int lvm2_label_write(struct labeller *l, struct device *dev, struct label *label)
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
    if (label->extra_len > sectsize - sizeof(struct label_ondisk))
	return 0;

    block = dbg_malloc(sizeof(struct label_ondisk) + label->extra_len);
    if (!block)
    {
	stack;
	return 0;
    }
    ondisk = (struct label_ondisk *)block;

    get_label_locations(size, sectsize, &offset[0], &offset[1]);

    /* Make into ondisk format */
    ondisk->magic = xlate32(LABEL_MAGIC);
    ondisk->version[0] = xlate32(label->version[0]);
    ondisk->version[1] = xlate32(label->version[1]);
    ondisk->version[2] = xlate32(label->version[2]);
    ondisk->label1_loc = xlate64(offset[0]);
    ondisk->label2_loc = xlate64(offset[1]);
    ondisk->datalen = xlate16(label->extra_len);
    strncpy(ondisk->disk_type, label->volume_type, sizeof(ondisk->disk_type));
    memcpy(block+sizeof(struct label_ondisk), label->extra_info, label->extra_len);
    ondisk->crc = xlate32(calc_crc(ondisk, label->extra_info));

    /* Write metadata to disk */
    if (!dev_open(dev, O_RDWR))
    {
	dbg_free(block);
	return 0;
    }

    status1 = dev_write(dev, offset[0], sizeof(struct label_ondisk) + label->extra_len, block);
    if (!status1)
	log_error("Error writing label 1\n");

    /* Write another at the end of the device */
    status2 = dev_write(dev, offset[1], sizeof(struct label_ondisk) + label->extra_len, block);
    if (!status2)
    {
	char zerobuf[sizeof(struct label_ondisk)];
	log_error("Error writing label 2\n");

	/* Wipe the first label so it doesn't get confusing */
	memset(zerobuf, 0, sizeof(struct label_ondisk));
	if (!dev_write(dev, offset[0], sizeof(struct label_ondisk), zerobuf))
	    log_error("Error erasing label 1\n");
    }

    dbg_free(block);
    dev_close(dev);

    return ((status1 != 0) && (status2 != 0));
}



/* Return 1 for Yes, 0 for No */
static int lvm2_is_labelled(struct labeller *l, struct device *dev)
{
    struct label *label;
    int status;

    status = lvm2_label_read(l, dev, &label);
    if (status) label_free(label);

    return status;
}

/* Check the device is labelled and has the right format_type */
static int _accept_format(struct dev_filter *f, struct device *dev)
{
    struct label *l;
    int status;
    struct filter_private *fp = (struct filter_private *) f->private;

    status = lvm2_label_read(NULL, dev, &l);

    if (status)
    {
	if (strcmp(l->volume_type, fp->disk_type) == 0)
	{
	    switch (fp->version_match)
	    {
	    case VERSION_MATCH_EQUAL:
		if (l->version[0] == fp->version[0] &&
		    l->version[1] == fp->version[1] &&
		    l->version[2] == fp->version[2])
		    return 1;
		break;

	    case VERSION_MATCH_LESSTHAN:
		if (l->version[0] == fp->version[0] &&
		    l->version[1] <  fp->version[1])
		    return 1;
		break;

	    case VERSION_MATCH_LESSEQUAL:
		if (l->version[0] == fp->version[0] &&
		    l->version[1] <= fp->version[1])
		    return 1;
		break;

	    case VERSION_MATCH_ANY:
		return 1;
	    }
	}
	label_free(l);
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
}

/* A filter to find devices with a particular label type on them */
struct dev_filter *lvm2_label_format_filter_create(char *disk_type, uint32_t version[3], int match_type)
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
struct dev_filter *lvm2_label_filter_create()
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
static int lvm2_labels_match(struct labeller *l, struct device *dev)
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

/* Allocate some space for the blocks we are going to read in */
    block1 = dbg_malloc(sectsize);
    if (!block1)
    {
	stack;
	return 0;
    }

    block2 = dbg_malloc(sectsize);
    if (!block2)
    {
	stack;
	dbg_free(block1);
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
    dbg_free(block2);
    dbg_free(block1);

    return status;
}

static int lvm2_label_remove(struct labeller *l, struct device *dev)
{
    uint64_t size;
    uint32_t sectsize;
    char     block[BLOCK_SIZE];
    int      status1, status2;
    long     offset[2];

    if (!dev_get_size(dev, &size))
	return 0;

    if (!dev_get_sectsize(dev, &sectsize))
	return 0;

    if (!dev_open(dev, O_RDWR))
    {
	dbg_free(block);
	return 0;
    }

    get_label_locations(size, sectsize, &offset[0], &offset[1]);
    memset(block, 0, BLOCK_SIZE);

    /* Blank out the first label */
    status1 = dev_write(dev, offset[0], BLOCK_SIZE, block);
    if (!status1)
	log_error("Error erasing label 1\n");

    /* ...and the other at the end of the device */
    status2 = dev_write(dev, offset[1], BLOCK_SIZE, block);
    if (!status2)
	log_error("Error erasing label 2\n");

    dev_close(dev);

    return ((status1 != 0) && (status2 != 0));
}

static void lvm2_label_destroy(struct labeller *l)
{
}

static struct label_ops handler_ops =
{
    can_handle: lvm2_is_labelled,
    write:      lvm2_label_write,
    remove:     lvm2_label_remove,
    read:       lvm2_label_read,
    verify:     lvm2_labels_match,
    destroy:    lvm2_label_destroy,
};

static struct labeller this_labeller =
{
    private : NULL,
    ops     : &handler_ops,
};

/* Don't know how this gets called... */
void lvm2_init()
{
    label_register_handler("LVM2", &this_labeller);
}
