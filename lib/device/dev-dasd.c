/*
 * Copyright (C) 2015 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "metadata.h"
#include "dev-type.h"
#include <sys/ioctl.h>

typedef struct dasd_information_t {
	unsigned int devno;		/* S/390 devno */
	unsigned int real_devno;	/* for aliases */
	unsigned int schid;		/* S/390 subchannel identifier */
	unsigned int cu_type  : 16;	/* from SenseID */
	unsigned int cu_model :  8;	/* from SenseID */
	unsigned int dev_type : 16;	/* from SenseID */
	unsigned int dev_model : 8;	/* from SenseID */
	unsigned int open_count;
	unsigned int req_queue_len;
	unsigned int chanq_len;		/* length of chanq */
	char type[4];			/* from discipline.name, 'none' for unknown */
	unsigned int status;		/* current device level */
	unsigned int label_block;	/* where to find the VOLSER */
	unsigned int FBA_layout;	/* fixed block size (like AIXVOL) */
	unsigned int characteristics_size;
	unsigned int confdata_size;
	unsigned char characteristics[64];/*from read_device_characteristics */
	unsigned char configuration_data[256];/*from read_configuration_data */
	unsigned int format;		/* format info like formatted/cdl/ldl/... */
	unsigned int features;		/* dasd features like 'ro',...            */
	unsigned int reserved0;		/* reserved for further use ,...          */
	unsigned int reserved1;		/* reserved for further use ,...          */
	unsigned int reserved2;		/* reserved for further use ,...          */
	unsigned int reserved3;		/* reserved for further use ,...          */
	unsigned int reserved4;		/* reserved for further use ,...          */
	unsigned int reserved5;		/* reserved for further use ,...          */
	unsigned int reserved6;		/* reserved for further use ,...          */
	unsigned int reserved7;		/* reserved for further use ,...          */
} dasd_information_t;

#define DASD_FORMAT_CDL 2
#define BIODASDINFO2  _IOR('D', 3, dasd_information_t)

int dasd_is_cdl_formatted(struct device *dev)
{
	int ret = 0;
	dasd_information_t dasd_info;

	if (!dev_open_readonly(dev)) {
		stack;
		return ret;
	}

	if (ioctl(dev->fd, BIODASDINFO2, &dasd_info) != 0)
		goto_out;

	if (dasd_info.format == DASD_FORMAT_CDL)
		ret = 1;
out:
	if (!dev_close(dev))
		stack;

	return ret;
}
