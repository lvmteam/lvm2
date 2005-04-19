/*
 * Copyright (C) 2005 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_PV_ALLOC_H
#include "pool.h"

int alloc_pv_segment_whole_pv(struct pool *mem, struct physical_volume *pv);
int peg_dup(struct pool *mem, struct list *peg_new, struct list *peg_free_new,
	    struct list *peg_old);

#endif
