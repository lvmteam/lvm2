/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_LABEL_H
#define _LVM_LABEL_H

#include "cache.h"
#include "uuid.h"
#include "device.h"

#define LABEL_ID "LABELONE"
#define LABEL_SIZE SECTOR_SIZE	/* Think very carefully before changing this */
#define LABEL_SCAN_SECTORS 4L
#define LABEL_SCAN_SIZE (LABEL_SCAN_SECTORS << SECTOR_SHIFT)

/* On disk - 32 bytes */
struct label_header {
	uint8_t id[8];		/* LABELONE */
	uint64_t sector_xl;	/* Sector number of this label */
	uint32_t crc_xl;	/* From next field to end of sector */
	uint32_t offset_xl;	/* Offset from start of struct to contents */
	uint8_t type[8];	/* LVM2 001 */
} __attribute__ ((packed));

/* In core */
struct label {
	char type[8];
	uint64_t sector;
	struct labeller *labeller;
	void *info;
};

struct labeller;

struct label_ops {
	/*
	 * Is the device labelled with this format ?
	 */
	int (*can_handle) (struct labeller * l, char *buf, uint64_t sector);

	/*
	 * Write a label to a volume.
	 */
	int (*write) (struct label * label, char *buf);

	/*
	 * Read a label from a volume.
	 */
	int (*read) (struct labeller * l, struct device * dev,
		     char *buf, struct label ** label);

	/*
	 * Additional consistency checks for the paranoid.
	 */
	int (*verify) (struct labeller * l, char *buf, uint64_t sector);

	/*
	 * Populate label_type etc.
	 */
	int (*initialise_label) (struct labeller * l, struct label * label);

	/*
	 * Destroy a previously read label.
	 */
	void (*destroy_label) (struct labeller * l, struct label * label);

	/*
	 * Destructor.
	 */
	void (*destroy) (struct labeller * l);
};

struct labeller {
	struct label_ops *ops;
	const void *private;
};

int label_init(void);
void label_exit(void);

int label_register_handler(const char *name, struct labeller *handler);

struct labeller *label_get_handler(const char *name);

int label_remove(struct device *dev);
int label_read(struct device *dev, struct label **result);
int label_write(struct device *dev, struct label *label);
int label_verify(struct device *dev);
struct label *label_create(struct labeller *labeller);
void label_destroy(struct label *label);

#endif
