/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_DISPLAY_H
#define _LVM_DISPLAY_H

#include "metadata.h"

#include <stdint.h>

uint64_t units_to_bytes(const char *units, char *unit_type);

/* Specify size in KB */
const char *display_size(struct cmd_context *cmd, uint64_t size);
const char *display_size_long(struct cmd_context *cmd, uint64_t size);
const char *display_size_units(struct cmd_context *cmd, uint64_t size);

char *display_uuid(char *uuidstr);
void display_stripe(const struct lv_segment *seg, uint32_t s, const char *pre);

void pvdisplay_colons(struct physical_volume *pv);
void pvdisplay_segments(struct physical_volume *pv);
void pvdisplay_full(struct cmd_context *cmd, struct physical_volume *pv,
		    void *handle);
int pvdisplay_short(struct cmd_context *cmd, struct volume_group *vg,
		    struct physical_volume *pv, void *handle);

void lvdisplay_colons(struct logical_volume *lv);
int lvdisplay_segments(struct logical_volume *lv);
int lvdisplay_full(struct cmd_context *cmd, struct logical_volume *lv,
		   void *handle);

void vgdisplay_extents(struct volume_group *vg);
void vgdisplay_full(struct volume_group *vg);
void vgdisplay_colons(struct volume_group *vg);
void vgdisplay_short(struct volume_group *vg);

void display_formats(struct cmd_context *cmd);
void display_segtypes(struct cmd_context *cmd);

/*
 * Allocation policy display conversion routines.
 */
const char *get_alloc_string(alloc_policy_t alloc);
alloc_policy_t get_alloc_from_string(const char *str);

#endif
