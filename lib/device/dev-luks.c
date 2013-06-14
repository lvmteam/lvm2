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

#include "lib.h"
#include "dev-type.h"

#define LUKS_SIGNATURE "LUKS\xba\xbe"
#define LUKS_SIGNATURE_SIZE 6

int dev_is_luks(struct device *dev, uint64_t *signature)
{
	char buf[LUKS_SIGNATURE_SIZE];
	int ret = -1;

	if (!dev_open_readonly(dev)) {
		stack;
		return -1;
	}

	*signature = 0;

	if (!dev_read(dev, 0, LUKS_SIGNATURE_SIZE, buf))
		goto_out;

	ret = memcmp(buf, LUKS_SIGNATURE, LUKS_SIGNATURE_SIZE) ? 0 : 1;

out:
	if (!dev_close(dev))
		stack;

	return ret;
}
