/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_FORMAT_TEXT_H
#define _LVM_FORMAT_TEXT_H

#include "lvm-types.h"
#include "metadata.h"
#include "pool.h"

#define FMT_TEXT_NAME "lvm2"
#define FMT_TEXT_ALIAS "text"

/*
 * Archives a vg config.  'retain_days' is the minimum number of
 * days that an archive file must be held for.  'min_archives' is
 * the minimum number of archives required to be kept for each
 * volume group.
 */
int archive_vg(struct volume_group *vg,
	       const char *dir,
	       const char *desc, uint32_t retain_days, uint32_t min_archive);

/*
 * Displays a list of vg backups in a particular archive directory.
 */
int archive_list(struct cmd_context *cmd, const char *dir, const char *vg);

/*
 * The text format can read and write a volume_group to a file.
 */
struct format_type *create_text_format(struct cmd_context *cmd);
void *create_text_context(struct cmd_context *cmd, const char *path,
			  const char *desc);

struct labeller *text_labeller_create(const struct format_type *fmt);

int pvhdr_read(struct device *dev, char *buf);

int add_da(const struct format_type *fmt, struct pool *mem, struct list *das,
	   uint64_t start, uint64_t size);
void del_das(struct list *das);

int add_mda(const struct format_type *fmt, struct pool *mem, struct list *mdas,
	    struct device *dev, uint64_t start, uint64_t size);
void del_mdas(struct list *mdas);

int vgname_from_mda(const struct format_type *fmt, struct device_area *dev_area,
		    char *buf, uint32_t size);

#endif
