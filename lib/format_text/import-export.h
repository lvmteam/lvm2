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
#include "pool.h"

#include <stdio.h>

/*
 * Constants to identify files this code can parse.
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

struct text_vg_version_ops {
	int (*check_version) (struct config_tree * cf);
	struct volume_group *(*read_vg) (struct format_instance * fid,
					 struct config_tree * cf);
	void (*read_desc) (struct pool * mem, struct config_tree * cf,
			   time_t *when, char **desc);
};

struct text_vg_version_ops *text_vg_vsn1_init(void);

int print_flags(uint32_t status, int type, char *buffer, size_t size);
int read_flags(uint32_t *status, int type, struct config_value *cv);

int text_vg_export_file(struct volume_group *vg, const char *desc, FILE *fp);
int text_vg_export_raw(struct volume_group *vg, const char *desc, char *buf,
		       uint32_t size);
struct volume_group *text_vg_import_file(struct format_instance *fid,
					 const char *file,
					 time_t *when, char **desc);
struct volume_group *text_vg_import_fd(struct format_instance *fid,
				       const char *file,
				       struct device *dev,
				       off_t offset, uint32_t size,
				       off_t offset2, uint32_t size2,
				       checksum_fn_t checksum_fn,
				       uint32_t checksum,
				       time_t *when, char **desc);

#endif
