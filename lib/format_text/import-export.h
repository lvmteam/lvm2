/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_TEXT_IMPORT_EXPORT_H
#define _LVM_TEXT_IMPORT_EXPORT_H

struct volume_group *text_vg_import(struct pool *mem, struct config_file *cf);
struct config_file *text_vg_export(struct pool *mem, struct volume_group *vg);

#endif
