/*
 * Copyright (C) 2006 Red Hat, Inc. All rights reserved.
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

#include <unistd.h>
#include <fcntl.h>

#ifdef UDEV_SYNC_SUPPORT
static const char _no_context_msg[] = "Udev library context not set.";
struct udev *_udev;

int udev_init_library_context(void)
{
	if (_udev)
		udev_unref(_udev);

	if (!(_udev = udev_new())) {
		log_error("Failed to create udev library context.");
		return 0;
	}

	return 1;
}

void udev_fin_library_context(void)
{
	udev_unref(_udev);
	_udev = NULL;
}

int udev_is_running(void)
{
	struct udev_queue *udev_queue;
	int r;

	if (!_udev) {
		log_debug_activation(_no_context_msg);
		goto bad;
	}

	if (!(udev_queue = udev_queue_new(_udev))) {
		log_debug_activation("Could not get udev state.");
		goto bad;
	}

	r = udev_queue_get_udev_is_active(udev_queue);
	udev_queue_unref(udev_queue);

	return r;

bad:
	log_debug_activation("Assuming udev is not running.");
	return 0;
}

struct udev* udev_get_library_context(void)
{
	return _udev;
}

#else	/* UDEV_SYNC_SUPPORT */

int udev_init_library_context(void)
{
	return 1;
}

void udev_fin_library_context(void)
{
}

int udev_is_running(void)
{
	return 0;
}

#endif

int lvm_getpagesize(void)
{
	return getpagesize();
}

int read_urandom(void *buf, size_t len)
{
	int fd;

	/* FIXME: we should stat here, and handle other cases */
	/* FIXME: use common _io() routine's open/read/close */
	if ((fd = open("/dev/urandom", O_RDONLY)) < 0) {
		log_sys_error("open", "read_urandom: /dev/urandom");
		return 0;
	}

	if (read(fd, buf, len) != (ssize_t) len) {
		log_sys_error("read", "read_urandom: /dev/urandom");
		if (close(fd))
			stack;
		return 0;
	}

	if (close(fd))
		stack;

	return 1;
}

