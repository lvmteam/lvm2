/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_LABEL_H
#define _LVM_LABEL_H

#include "uuid.h"
#include "device.h"

struct label {
	struct id id;

	char volume_type[32];
	uint32_t version[3];

	size_t extra_len;
	char *extra_info;
};

struct labeller;

struct label_ops {
	/*
	 * Is the device labelled with this format ?
	 */
	int (*can_handle)(struct labeller *l, struct device *dev);

	/*
	 * Write a label to a volume.
	 */
	int (*write)(struct labeller *l,
		     struct device *dev, struct label *label);

	/*
	 * Remove a label from a device.
	 */
	int (*remove)(struct labeller *l, struct device *dev);

	/*
	 * Read a label from a volume.
	 */
	int (*read)(struct labeller *l,
		    struct device *dev, struct label **label);

	/*
	 * Additional consistency checks for the paranoid.
	 */
	int (*verify)(struct labeller *l, struct device *dev);

	/*
	 * Destructor.
	 */
	void (*destroy)(struct labeller *l);
};

struct labeller {
	struct label_ops *ops;
	void *private;
};


int label_init(void);
void label_exit(void);

int label_register_handler(const char *name, struct labeller *handler);

struct labeller *label_get_handler(const char *name);

int label_remove(const char *path);
int label_read(const char *path, struct label **result);
int label_verify(const char *path);
void label_free(struct label *l);

/*
 * We'll support two label types: the 'pretend the
 * LVM1 pv structure at the begining of the disk
 * is a label' hack, and pjc's 1 sector labels at
 * the front and back of the device.
 */

#endif
