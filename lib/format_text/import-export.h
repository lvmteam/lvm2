/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_TEXT_IMPORT_EXPORT_H
#define _LVM_TEXT_IMPORT_EXPORT_H

#include "config.h"
#include "lvm-types.h"
#include "metadata.h"
#include "uuid-map.h"

#include <stdio.h>

/*
 * Constants to help indentify which files this code can parse.
 */
#define CONTENTS_FIELD "contents"
#define CONTENTS_VALUE "Text Format Volume Group"

#define FORMAT_VERSION_FIELD "version"
#define FORMAT_VERSION_VALUE 1

/*
 * VGs, PVs and LVs all have status bitsets, we gather together
 * common code for reading and writing them.
 */
enum {
	VG_FLAGS,
	PV_FLAGS,
	LV_FLAGS
};

int print_flags(uint32_t status, int type, char *buffer, size_t size);
int read_flags(uint32_t *status, int type, struct config_value *cv);


int text_vg_export(FILE *fp, struct volume_group *vg, const char *desc);
struct volume_group *text_vg_import(struct format_instance *fid,
				    const char *file,
				    struct uuid_map *um,
				    time_t *when, char **desc);

#endif
