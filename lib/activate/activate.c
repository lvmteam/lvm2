/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "activate.h"

struct kernel_interface {
	int already_mounted;
	char *mount_point;
};

/*
 * Checks that device mapper has been compiled into the kernel.
 */
int _check_kernel(void)
{
	return 0;
}

int _find_mountpoint(char *buffer, int len)
{
	
}



int _

int vg_activate(struct volume_group *vg)
{
	
}

int lv_activate(struct volume_group *vg, struct logical_volume *lv)
{
	
}

