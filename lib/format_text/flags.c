/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "metadata.h"
#include "import-export.h"
#include "lvm-string.h"

/*
 * Bitsets held in the 'status' flags get
 * converted into arrays of strings.
 */
struct flag {
	const int mask;
	const char *description;
};

static struct flag _vg_flags[] = {
	{EXPORTED_VG, "EXPORTED"},
	{RESIZEABLE_VG, "RESIZEABLE"},
	{PARTIAL_VG, "PARTIAL"},
	{LVM_READ, "READ"},
	{LVM_WRITE, "WRITE"},
	{CLUSTERED, "CLUSTERED"},
	{SHARED, "SHARED"},
	{0, NULL}
};

static struct flag _pv_flags[] = {
	{ALLOCATABLE_PV, "ALLOCATABLE"},
	{EXPORTED_VG, "EXPORTED"},
	{0, NULL}
};

static struct flag _lv_flags[] = {
	{LVM_READ, "READ"},
	{LVM_WRITE, "WRITE"},
	{FIXED_MINOR, "FIXED_MINOR"},
	{VISIBLE_LV, "VISIBLE"},
	{0, NULL}
};

static struct flag *_get_flags(int type)
{
	switch (type) {
	case VG_FLAGS:
		return _vg_flags;

	case PV_FLAGS:
		return _pv_flags;

	case LV_FLAGS:
		return _lv_flags;
	}

	log_err("Unknown flag set requested.");
	return NULL;
}

static int _emit(char **buffer, size_t *size, const char *fmt, ...)
{
	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(*buffer, *size, fmt, ap);
	va_end(ap);

	if (n < 0 || (n == *size))
		return 0;

	*buffer += n;
	*size -= n;
	return 1;
}

/*
 * Converts a bitset to an array of string values,
 * using one of the tables defined at the top of
 * the file.
 */
int print_flags(uint32_t status, int type, char *buffer, size_t size)
{
	int f, first = 1;
	struct flag *flags;

	if (!(flags = _get_flags(type))) {
		stack;
		return 0;
	}

	if (!_emit(&buffer, &size, "["))
		return 0;

	for (f = 0; flags[f].mask; f++) {
		if (status & flags[f].mask) {
			if (!first) {
				if (!_emit(&buffer, &size, ", "))
					return 0;

			} else
				first = 0;

			if (!_emit(&buffer, &size, "\"%s\"",
				   flags[f].description))
				return 0;

			status &= ~flags[f].mask;
		}
	}

	if (!_emit(&buffer, &size, "]"))
		return 0;

	if (status)
		log_error("Metadata inconsistency: Not all flags successfully "
			  "exported.");

	return 1;
}

int read_flags(uint32_t *status, int type, struct config_value *cv)
{
	int f;
	uint32_t s = 0;
	struct flag *flags;

	if (!(flags = _get_flags(type))) {
		stack;
		return 0;
	}

	if (cv->type == CFG_EMPTY_ARRAY)
		goto out;

	while (cv) {
		if (cv->type != CFG_STRING) {
			log_err("Status value is not a string.");
			return 0;
		}

		for (f = 0; flags[f].description; f++)
			if (!strcmp(flags[f].description, cv->v.str)) {
				s |= flags[f].mask;
				break;
			}

		if (!flags[f].description) {
			log_err("Unknown status flag '%s'.", cv->v.str);
			return 0;
		}

		cv = cv->next;
	}

      out:
	*status = s;
	return 1;
}
