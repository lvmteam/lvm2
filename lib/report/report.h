/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_REPORT_H
#define _LVM_REPORT_H

#include "metadata-exported.h"
#include "label.h"
#include "activate.h"

typedef enum {
	LVS		= 1,
	LVSINFO		= 2,
	LVSSTATUS	= 4,
	LVSINFOSTATUS   = 8,
	PVS		= 16,
	VGS		= 32,
	SEGS		= 64,
	SEGSSTATUS	= 128,
	PVSEGS		= 256,
	LABEL		= 512,
	DEVTYPES	= 1024
} report_type_t;

/*
 * The "struct selection_handle" is used only for selection
 * of items that should be processed further (not for display!).
 *
 * It consists of selection reporting handle "selection_rh"
 * used for the selection itself (not for display on output!).
 * The items are reported directly in memory to a buffer and
 * then compared against selection criteria. Once we know the
 * result of the selection, the buffer is dropped!
 *
 * The "orig_report_type" is the original requested report type.
 * The "report_type" is the reporting type actually used which
 * also counts with report types of the fields used in selection
 * criteria.
 *
 * The "selected" variable is used for propagating the result
 * of the selection.
 */
struct selection_handle {
	struct dm_report *selection_rh;
	report_type_t orig_report_type;
	report_type_t report_type;
	int selected;
};

struct field;
struct report_handle;

typedef int (*field_report_fn) (struct report_handle * dh, struct field * field,
				const void *data);

void *report_init(struct cmd_context *cmd, const char *format, const char *keys,
		  report_type_t *report_type, const char *separator,
		  int aligned, int buffered, int headings, int field_prefixes,
		  int quoted, int columns_as_rows, const char *selection);
void *report_init_for_selection(struct cmd_context *cmd, report_type_t *report_type,
				const char *selection);
int report_for_selection(struct cmd_context *cmd,
			 struct selection_handle *sh,
			 struct physical_volume *pv,
			 struct volume_group *vg,
			 struct logical_volume *lv);
void report_free(void *handle);
int report_object(void *handle, int selection_only, const struct volume_group *vg,
		  const struct logical_volume *lv, const struct physical_volume *pv,
		  const struct lv_segment *seg, const struct pv_segment *pvseg,
		  const struct lv_with_info_and_seg_status *lvdm,
		  const struct label *label);
int report_devtypes(void *handle);
int report_output(void *handle);

#endif
