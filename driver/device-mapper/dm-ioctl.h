/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _DM_IOCTL_H
#define _DM_IOCTL_H

#include "device-mapper.h"

/*
 * Implements a traditional ioctl interface to the
 * device mapper.  Yuck.
 */

struct dm_target_spec {
	int32_t status;		/* used when reading from kernel only */
	unsigned long long sector_start;
	unsigned long long length;

	char target_type[DM_MAX_TYPE_NAME];

	unsigned long next;	/* offset in bytes to next target_spec */

	/*
	 * Parameter string starts immediately
	 * after this object.  Be careful to add
	 * padding after string to ensure correct
	 * alignment of subsequent dm_target_spec.
	 */
};

struct dm_ioctl {
	unsigned long data_size;	/* the size of this structure */
	char name[DM_NAME_LEN];

	int exists;		/* out */
	int suspend;		/* in/out */
	int open_count;		/* out */
	int major;              /* out */
	int minor;		/* in/out */

	int target_count;	/* in/out */
};

/* FIXME: find own numbers, 109 is pinched from LVM */
#define DM_IOCTL 0xfd
#define DM_CHAR_MAJOR 124

#define	DM_CREATE _IOWR(DM_IOCTL, 0x00, struct dm_ioctl)
#define	DM_REMOVE _IOW(DM_IOCTL, 0x01, struct dm_ioctl)
#define	DM_SUSPEND _IOW(DM_IOCTL, 0x02, struct dm_ioctl)
#define	DM_RELOAD _IOWR(DM_IOCTL, 0x03, struct dm_ioctl)
#define DM_INFO _IOWR(DM_IOCTL, 0x04, struct dm_ioctl)

#endif
