/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_UUID_H
#define _LVM_UUID_H

#include "lvm-types.h"

#define ID_LEN 32
#define ID_LEN_S "32"

struct id {
	uint8_t uuid[ID_LEN];
};

/*
 * Unique logical volume identifier
 * With format1 this is VG uuid + LV uuid + '\0'
 */
union lvid {
	struct id id[2];
	char s[2 * sizeof(struct id) + 1];
};

int lvid_from_lvnum(union lvid *lvid, struct id *vgid, int lv_num);
int lvnum_from_lvid(union lvid *lvid);

int lvid_create(union lvid *lvid, struct id *vgid);
int id_create(struct id *id);
int id_valid(struct id *id);
int id_equal(struct id *lhs, struct id *rhs);

/*
 * Fills 'buffer' with a more human readable form
 * of the uuid.
 */
int id_write_format(struct id *id, char *buffer, size_t size);

/*
 * Reads a formatted uuid.
 */
int id_read_format(struct id *id, char *buffer);

#endif
