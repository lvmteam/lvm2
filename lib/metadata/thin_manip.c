/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "metadata.h"

int attach_pool_metadata(struct lv_segment *seg, struct logical_volume *thin_pool_metadata)
{
	// FIXME Housekeeping needed here (cf attach_mirror_log)
	seg->metadata_lv = thin_pool_metadata;

	return 1;
}

int attach_pool_lv(struct lv_segment *seg, struct logical_volume *thin_pool_lv)
{
	// FIXME Housekeeping needed here (cf attach_mirror_log)
	seg->thin_pool_lv = thin_pool_lv;

	return 1;
}

