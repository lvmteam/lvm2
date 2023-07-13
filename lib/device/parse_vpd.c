/*
 * Copyright (C) 2022 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/device/device.h"
#include "lib/device/device_id.h"

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>

/*
 * Remove leading spaces.
 * Remove trailing spaces.
 * Replace each space with underscore.
 * Skip quotes, non-ascii, non-printable.
 */
int format_general_id(const char *in, size_t in_bytes, unsigned char *out, size_t out_bytes)
{
	const char *end;
	size_t end_bytes = strlen(in);
	int retlen = 0;
	unsigned j = 0;
	unsigned i;

	if (!end_bytes)
		return 0;

	end = in + end_bytes - 1;
	while ((end > in) && (*end == ' ')) {
		end--;
		end_bytes--;
	}

	for (i = 0; i < end_bytes; i++) {
		if (!in[i])
			break;
		if (j >= (out_bytes - 2))
			break;
		/* skip leading spaces */
		if (!retlen && (in[i] == ' '))
			continue;
		/* skip non-ascii non-printable characters */
		if (!isascii(in[i]) || !isprint(in[i]))
			continue;
		/* skip quote */
		if (in[i] == '"')
			continue;
		/* replace each space with _ */
		if (in[i] == ' ')
			out[j++] = '_';
		else
			out[j++] = in[i];
		retlen++;
	}
	return retlen;
}

/*
 * Remove leading spaces.
 * Remove trailing spaces.
 * Replace series of spaces with a single _.
 * Skip quotes, non-ascii, non-printable.
 */
int format_t10_id(const unsigned char *in, size_t in_bytes, unsigned char *out, size_t out_bytes)
{
	int in_space = 0;
	int retlen = 0;
	unsigned j = 0;
	unsigned i;

	for (i = 0; i < in_bytes; i++) {
		if (!in[i])
			break;
		if (j >= (out_bytes - 2))
			break;
		/* skip leading spaces */
		if (!retlen && (in[i] == ' '))
			continue;
		/* skip non-ascii non-printable characters */
		if (!isascii(in[i]) || !isprint(in[i]))
			continue;
		/* skip quote */
		if (in[i] == '"')
			continue;
		/* replace one or more spaces with _ */
		if (in[i] == ' ') {
			in_space = 1;
			continue;
		}
		/* spaces are finished so insert _ */
		if (in_space) {
			out[j++] = '_';
			in_space = 0;
			retlen++;
		}
		out[j++] = in[i];
		retlen++;
	}
	return retlen;
}

static int _to_hex(const unsigned char *in, int in_bytes, unsigned char *out, int out_bytes)
{
	int off = 0;
	int num;
	int i;

	for (i = 0; i < in_bytes; i++) {
		num = sprintf((char *)out + off, "%02x", in[i]);
		if (num < 0)
			break;
		off += num;
		if (off + 2 >= out_bytes)
			break;
	}
	return off;
}

#define ID_BUFSIZE 1024

/*
 * based on linux kernel function
 */
int parse_vpd_ids(const unsigned char *vpd_data, int vpd_datalen, struct dm_list *ids)
{
	char id[ID_BUFSIZE];
	unsigned char tmp_str[ID_BUFSIZE];
	const unsigned char *d, *cur_id_str;
	size_t id_len = ID_BUFSIZE;
	int id_size = -1;
	int type;
	uint8_t cur_id_size = 0;

	memset(id, 0, ID_BUFSIZE);
	for (d = vpd_data + 4;
	     d < vpd_data + vpd_datalen;
	     d += d[3] + 4) {
		memset(tmp_str, 0, sizeof(tmp_str));

		switch (d[1] & 0xf) {
		case 0x1:
			/* T10 Vendor ID */
			cur_id_size = d[3];
			if ((size_t)(cur_id_size + 4) > id_len)
				cur_id_size = id_len - 4;
			cur_id_str = d + 4;
			format_t10_id(cur_id_str, cur_id_size, tmp_str, sizeof(tmp_str));
			id_size = snprintf(id, ID_BUFSIZE, "t10.%s", tmp_str);
			if (id_size < 0)
				break;
			if (id_size >= ID_BUFSIZE)
				id_size = ID_BUFSIZE - 1;
			dev_add_wwid(id, 1, ids);
			break;
		case 0x2:
			/* EUI-64 */
			cur_id_size = d[3];
			cur_id_str = d + 4;
			switch (cur_id_size) {
			case 8:
				_to_hex(cur_id_str, 8, tmp_str, sizeof(tmp_str));
				id_size = snprintf(id, ID_BUFSIZE, "eui.%s", tmp_str);
				break;
			case 12:
				_to_hex(cur_id_str, 12, tmp_str, sizeof(tmp_str));
				id_size = snprintf(id, ID_BUFSIZE, "eui.%s", tmp_str);
				break;
			case 16:
				_to_hex(cur_id_str, 16, tmp_str, sizeof(tmp_str));
				id_size = snprintf(id, ID_BUFSIZE, "eui.%s", tmp_str);
				break;
			default:
				break;
			}
			if (id_size < 0)
				break;
			if (id_size >= ID_BUFSIZE)
				id_size = ID_BUFSIZE - 1;
			dev_add_wwid(id, 2, ids);
			break;
		case 0x3:
			/* NAA */
			cur_id_size = d[3];
			cur_id_str = d + 4;
			switch (cur_id_size) {
			case 8:
				_to_hex(cur_id_str, 8, tmp_str, sizeof(tmp_str));
				id_size = snprintf(id, ID_BUFSIZE, "naa.%s", tmp_str);
				break;
			case 16:
				_to_hex(cur_id_str, 16, tmp_str, sizeof(tmp_str));
				id_size = snprintf(id, ID_BUFSIZE, "naa.%s", tmp_str);
				break;
			default:
				break;
			}
			if (id_size < 0)
				break;
			if (id_size >= ID_BUFSIZE)
				id_size = ID_BUFSIZE - 1;
			dev_add_wwid(id, 3, ids);
			break;
		case 0x8:
			/* SCSI name string */
			cur_id_size = d[3];
			cur_id_str = d + 4;
			if (cur_id_size >= id_len)
				cur_id_size = id_len - 1;
			memcpy(id, cur_id_str, cur_id_size);
			id_size = cur_id_size;

			/*
			 * if naa or eui ids are provided as scsi names,
			 * consider them to be naa/eui types.
			 */
			if (!memcmp(id, "eui.", 4))
				type = 2;
			else if (!memcmp(id, "naa.", 4))
				type = 3;
			else
				type = 8;

			/*
			 * Not in the kernel version, copying multipath code,
			 * which checks if this string begins with naa or eui
			 * and if so does tolower() on the chars.
			 */
			if ((type == 2) || (type == 3)) {
				unsigned i;
				for (i = 0; i < strlen(id); i++)
					id[i] = tolower(id[i]);
			}
			dev_add_wwid(id, type, ids);
			break;
		default:
			break;
		}
	}

	return id_size;
}

int parse_vpd_serial(const unsigned char *in, char *out, size_t outsize)
{
	uint8_t len_buf[2] __attribute__((aligned(8))) = { 0 };
	size_t len;

	/* parsing code from multipath tools */
	/* ignore in[0] and in[1] */
	/* len is in[2] and in[3] */
	/* serial begins at in[4] */

	len_buf[0] = in[2];
	len_buf[1] = in[3];
	len = len_buf[0] << 8 | len_buf[1];

	if (outsize == 0)
		return 0;

	if (len > DEV_WWID_SIZE)
		len = DEV_WWID_SIZE;
	/*
	 * Strip leading and trailing whitespace
	 */
	while (len > 0 && in[len + 3] == ' ')
		--len;
	while (len > 0 && in[4] == ' ') {
		++in;
		--len;
	}

	if (len >= outsize)
                len = outsize - 1;

	if (len > 0) {
		memcpy(out, in + 4, len);
		out[len] = '\0';
	}
	return len;
}

