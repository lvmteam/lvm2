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

#include "lvm-percent.h"

float percent_to_float(percent_t v)
{
    return (float)v / PERCENT_1;
}

percent_t make_percent(uint64_t numerator, uint64_t denominator)
{
    percent_t percent;
    if (!denominator)
        return PERCENT_100; /* FIXME? */
    if (!numerator)
        return PERCENT_0;
    if (numerator == denominator)
        return PERCENT_100;
    switch (percent = PERCENT_100 * ((double) numerator / (double) denominator)) {
    case PERCENT_100:
        return PERCENT_100 - 1;
    case PERCENT_0:
        return PERCENT_0 + 1;
    default:
        return percent;
    }
}

