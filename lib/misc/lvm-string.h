/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_STRING_H
#define _LVM_STRING_H

#include <stdio.h>
#include <stdarg.h>

/*
 * On error, up to glibc 2.0.6, snprintf returned -1 if buffer was too small;
 * From glibc 2.1 it returns number of chars (excl. trailing null) that would 
 * have been written had there been room.
 *
 * lvm_snprintf reverts to the old behaviour.
 */
int lvm_snprintf(char *buf, size_t bufsize, const char *format, ...);

int emit_to_buffer(char **buffer, size_t *size, const char *fmt, ...);

#endif
