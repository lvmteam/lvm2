/*
 * Copyright (C) 2005 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "libdevmapper.h"
#include "libdm-event.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

static char *dso_name = "kickme";
static char *device = "/dev/mapper/test";
static char *device1 = "/dev/mapper/test1";
enum event_type events = ALL_ERRORS;

static void log_return(int ret, char *what, char *device)
{
	printf("%s returned \"%s\"\n", what, strerror(-ret));

	if (ret < 0)
		printf("%s %s FAILED :-(((\n", what, device);
	else
		printf("%s %s succeded :-)\n", what, device);
}

int main(int argc, char **argv)
{
	int reg = 0;

	if (argc > 1 && *argv[1] == 'r')
		reg = 1;

	if (reg) {
		log_return(dm_register_for_event(dso_name, device, events),
			   "register", device);
		log_return(dm_register_for_event(dso_name, device1, events),
			   "register", device1);
	} else {
		log_return(dm_unregister_for_event(dso_name, device, events),
			   "unregister", device);
		log_return(dm_unregister_for_event(dso_name, device1, events),
			   "unregister", device1);
	}

	return 0;
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
