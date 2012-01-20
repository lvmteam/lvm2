/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_PERCENT_H
#define _LVM_PERCENT_H
#include <stdint.h>

/*
 * A fixed-point representation of percent values. One percent equals to
 * PERCENT_1 as defined below. Values that are not multiples of PERCENT_1
 * represent fractions, with precision of 1/1000000 of a percent. See
 * percent_to_float for a conversion to a floating-point representation.
 *
 * You should always use make_percent when building percent_t values. The
 * implementation of make_percent is biased towards the middle: it ensures that
 * the result is PERCENT_0 or PERCENT_100 if and only if this is the actual
 * value -- it never rounds any intermediate value (> 0 or < 100) to either 0
 * or 100.
 */
typedef int32_t percent_t;

typedef enum {
	PERCENT_0 = 0,
	PERCENT_1 = 1000000,
	PERCENT_100 = 100 * PERCENT_1,
	PERCENT_INVALID = -1,
	PERCENT_MERGE_FAILED = -2
} percent_range_t;

float percent_to_float(percent_t v);
percent_t make_percent(uint64_t numerator, uint64_t denominator);

#endif
