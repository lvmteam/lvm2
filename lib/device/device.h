/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_DEVICE_H
#define _LVM_DEVICE_H

#include "uuid.h"

#include <fcntl.h>

#define DEV_ACCESSED_W		0x00000001	/* Device written to? */
#define DEV_REGULAR		0x00000002	/* Regular file? */
#define DEV_ALLOCED		0x00000004	/* dm_malloc used */
#define DEV_OPENED_RW		0x00000008	/* Opened RW */
#define DEV_OPENED_EXCL		0x00000010	/* Opened EXCL */
#define DEV_O_DIRECT		0x00000020	/* Use O_DIRECT */
#define DEV_O_DIRECT_TESTED	0x00000040	/* DEV_O_DIRECT is reliable */

/*
 * Support for external device info.
 * Any new external device info source needs to be
 * registered using EXT_REGISTER macro in dev-ext.c.
 */
typedef enum {
	DEV_EXT_NONE,
	DEV_EXT_UDEV,
	DEV_EXT_NUM
} dev_ext_t;

struct dev_ext {
	int enabled;
	dev_ext_t src;
	void *handle;
};

/*
 * All devices in LVM will be represented by one of these.
 * pointer comparisons are valid.
 */
struct device {
	struct dm_list aliases;	/* struct dm_str_list */
	dev_t dev;

	/* private */
	int fd;
	int open_count;
	int error_count;
	int max_error_count;
	int phys_block_size;
	int block_size;
	int read_ahead;
	uint32_t flags;
	uint64_t end;
	struct dm_list open_list;
	struct dev_ext ext;

	char pvid[ID_LEN + 1];
	char _padding[7];
};

struct device_list {
	struct dm_list list;
	struct device *dev;
};

struct device_area {
	struct device *dev;
	uint64_t start;		/* Bytes */
	uint64_t size;		/* Bytes */
};

/*
 * Support for external device info.
 */
const char *dev_ext_name(struct device *dev);
int dev_ext_enable(struct device *dev, dev_ext_t src);
int dev_ext_disable(struct device *dev);
struct dev_ext *dev_ext_get(struct device *dev);
int dev_ext_release(struct device *dev);

/*
 * All io should use these routines.
 */
int dev_get_block_size(struct device *dev, unsigned int *phys_block_size, unsigned int *block_size);
int dev_get_size(struct device *dev, uint64_t *size);
int dev_get_read_ahead(struct device *dev, uint32_t *read_ahead);
int dev_discard_blocks(struct device *dev, uint64_t offset_bytes, uint64_t size_bytes);

/* Use quiet version if device number could change e.g. when opening LV */
int dev_open(struct device *dev);
int dev_open_quiet(struct device *dev);
int dev_open_flags(struct device *dev, int flags, int direct, int quiet);
int dev_open_readonly(struct device *dev);
int dev_open_readonly_buffered(struct device *dev);
int dev_open_readonly_quiet(struct device *dev);
int dev_close(struct device *dev);
int dev_close_immediate(struct device *dev);
void dev_close_all(void);
int dev_test_excl(struct device *dev);

int dev_fd(struct device *dev);
const char *dev_name(const struct device *dev);

int dev_read(struct device *dev, uint64_t offset, size_t len, void *buffer);
int dev_read_circular(struct device *dev, uint64_t offset, size_t len,
		      uint64_t offset2, size_t len2, char *buf);
int dev_write(struct device *dev, uint64_t offset, size_t len, void *buffer);
int dev_append(struct device *dev, size_t len, char *buffer);
int dev_set(struct device *dev, uint64_t offset, size_t len, int value);
void dev_flush(struct device *dev);

struct device *dev_create_file(const char *filename, struct device *dev,
			       struct dm_str_list *alias, int use_malloc);

/* Return a valid device name from the alias list; NULL otherwise */
const char *dev_name_confirmed(struct device *dev, int quiet);

#endif
