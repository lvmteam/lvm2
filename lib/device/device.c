/*
 * Copyright (C) 2001 Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/genhd.h>

#include "device.h"
#include "log.h"

int do_ioctl(const char *file, int mode, unsigned long cmd, void *req)
{
        int ret, fd;

        if ((fd = open(file, mode)) < 0) {
                log_sys_err("open");
                return errno;
        }

        if ((ret = ioctl(fd, cmd, req)) < 0) {
                log_sys_err("ioctl");
                ret = errno;
        }

        close(fd);
        return ret;
}

int device_get_size(const char *dev_name)
{
	int ret, r;

	log_verbose("Getting device size");
	if (!(ret = do_ioctl(dev_name, O_RDONLY, BLKGETSIZE, &r)))
		return r;

	return ret;
}
