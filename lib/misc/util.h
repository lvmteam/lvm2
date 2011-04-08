/*
 * Copyright (C) 2007 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_UTIL_H
#define _LVM_UTIL_H

#define min(a, b) ({ typeof(a) _a = (a); \
		     typeof(b) _b = (b); \
		     (void) (&_a == &_b); \
		     _a < _b ? _a : _b; })

#define max(a, b) ({ typeof(a) _a = (a); \
		     typeof(b) _b = (b); \
		     (void) (&_a == &_b); \
		     _a > _b ? _a : _b; })

#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 4 && __GNUC_MINOR__ >= 6)
#define uninitialized_var(x) x
#else
#define uninitialized_var(x) x = x
#endif

#define KERNEL_VERSION(major, minor, release) (((major) << 16) + ((minor) << 8) + (release))

#endif
