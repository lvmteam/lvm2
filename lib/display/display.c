/*
 * Copyright (C) 2001  Sistina Software
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
 *
 */

#include <sys/types.h>
#include <string.h>
#include "display/display.h"
#include "metadata/metadata.h"
#include "mm/dbg_malloc.h"
#include "log/log.h"

#define SIZE_BUF 128

char *display_size(unsigned long long size, size_len_t sl)
{
	int s;
	ulong byte = 1024 * 1024 * 1024;
	char *size_buf = NULL;
	char *size_str[][2] = {
		{"Terabyte", "TB"},
		{"Gigabyte", "GB"},
		{"Megabyte", "MB"},
		{"Kilobyte", "KB"},
		{"", ""}
	};

	if (!(size_buf = dbg_malloc(SIZE_BUF))) {
		log_error("no memory for size display buffer");
		return NULL;
	}

	if (size == 0LL)
		sprintf(size_buf, "0");
	else {
		s = 0;
		while (size_str[s] && size < byte)
			s++, byte /= 1024;
		snprintf(size_buf, SIZE_BUF - 1,
			 "%.2f %s", (float) size / byte, size_str[s][sl]);
	}

	/* Caller to deallocate */
	return size_buf;
}

char *display_uuid(char *uuidstr)
{
	int i, j;
	char *uuid;

	if ((!uuidstr) || !(uuid = dbg_malloc(NAME_LEN))) {
		log_error("no memory for uuid display buffer");
		return NULL;
	}

	memset(uuid, 0, NAME_LEN);

	i = 6;
	memcpy(uuid, uuidstr, i);
	uuidstr += i;

	for (j = 0; j < 6; j++) {
		uuid[i++] = '-';
		memcpy(&uuid[i], uuidstr, 4);
		uuidstr += 4;
		i += 4;
	}

	memcpy(&uuid[i], uuidstr, 2);

	/* Caller must free */
	return uuid;
}

