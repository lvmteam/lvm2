/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_STRING_H
#define _LVM_STRING_H

#include <stdarg.h>

/*
 * Different versions of glibc have different
 * return values for over full buffers.
 */
static inline int lvm_snprintf(char *str, size_t size, const char *format, ...)
{
	int n;
	va_list ap;

	va_start(ap, format);
	n = vsnprintf(str, size, format, ap);
	va_end(ap);

	if (n < 0 || (n == size))
		return -1;

	return n;
}

#endif
