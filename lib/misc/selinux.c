/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "selinux.h"

#include <selinux/selinux.h>

int set_selinux_context(const char *path, mode_t mode)
{
	security_context_t scontext;

	if (is_selinux_enabled() <= 0)
		return 1;

	if (matchpathcon(path, mode, &scontext) < 0) {
		log_error("%s: matchpathcon %07o failed: %s", path, mode,
			  strerror(errno));
		return 0;
	}

	log_very_verbose("Setting SELinux context for %s to %s.",
			 path, scontext);

	if ((lsetfilecon(path, scontext) < 0) && (errno != ENOTSUP)) {
		log_sys_error("lsetfilecon", path);
		freecon(scontext);
		return 0;
	}

	freecon(scontext);
	return 1;
}
