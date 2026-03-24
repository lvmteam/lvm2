/*
 * Copyright (C) 2018-2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef LIB_VDO_PARSE_H
#define LIB_VDO_PARSE_H

#include "libdm/libdevmapper.h"

const char *vdo_parse_eat_space(const char *b, const char *e);
int vdo_parse_tok_eq(const char *b, const char *e, const char *str);
int vdo_parse_uint64(const char *b, const char *e, void *context);
int vdo_parse_operating_mode(const char *b, const char *e, void *context);

#endif /* LIB_VDO_PARSE_H */
