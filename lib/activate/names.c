/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "names.h"
#include "lvm-string.h"
#include "log.h"
#include "limits.h"

#include <libdevmapper.h>

/*
 * The volume group name and the logical volume name are
 * seperated by a single ':', any colons in the vg name are
 * doubled up to form a pair.
 */
int build_dm_name(char *buffer, size_t len, const char *prefix,
		  const char *vg_name, const char *lv_name)
{
	char *out;
	const char *in;

	for (out = buffer, in = vg_name; len && *in; len--) {
		if (*in == ':') {
			*out++ = ':';
			if (!--len)
				break;
		}

		*out++ = *in++;
		len--;
	}

	if (!len)
		return 0;

	if (lvm_snprintf(out, len, ":%s%s", prefix, lv_name) == -1) {
		log_err("Couldn't build logical volume name.");
		return 0;
	}

	return 1;
}

int build_dm_path(char *buffer, size_t len, const char *prefix,
		  const char *vg_name, const char *lv_name)
{
	char dev_name[PATH_MAX];

	if (!build_dm_name(dev_name, sizeof(dev_name),
			   prefix, vg_name, lv_name)) {
		stack;
		return 0;
	}

	if (lvm_snprintf(buffer, len, "%s/%s", dm_dir(), dev_name) == -1) {
		stack;
		return 0;
	}

	return 1;
}

int build_vg_path(char *buffer, size_t len,
		  const char *dev_dir, const char *vg_name)
{
	if (lvm_snprintf(buffer, len, "%s%s", dev_dir, vg_name) == -1) {
		stack;
		return 0;
	}

	return 1;
}

int build_lv_link_path(char *buffer, size_t len, const char *dev_dir,
		       const char *vg_name, const char *lv_name)
{
	if (lvm_snprintf(buffer, len, "%s%s/%s",
			 dev_dir, vg_name, lv_name) == -1) {
		stack;
		return 0;
	}

	return 1;
}


