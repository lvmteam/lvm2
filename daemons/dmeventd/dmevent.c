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
#include "libmultilog.h"

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

static enum event_type events = ALL_ERRORS; /* All until we can distinguish. */
static char default_dso_name[] = "noop";  /* default DSO is noop */
static int default_reg = 1;		 /* default action is register */

/* Display help. */
static void print_usage(char *name)
{
	char *cmd = strrchr(name, '/');

	cmd = cmd ? cmd + 1 : name;
	printf("Usage::\n"
	       "%s [options] <device>\n"
	       "\n"
	       "Options:\n"
	       "  -d <dso>           Specify the DSO to use.\n"
	       "  -h                 Print this usage.\n"
	       "  -l                 List registered devices.\n"
	       "  -r                 Register for event (default).\n"
	       "  -u                 Unregister for event.\n"
	       "\n", cmd);
}

/* Parse command line arguments. */
static int parse_argv(int argc, char **argv,
		      char **dso_name, char **device, int *reg, int *list)
{
	int c;
	const char *options = "d:hlru";

	while ((c = getopt(argc, argv, options)) != -1) {
		switch (c) {
		case 'd':
			if (!(*dso_name = strdup(optarg)))
				exit(EXIT_FAILURE);

			break;
		case 'h':
			print_usage(argv[0]);
			exit(EXIT_SUCCESS);
		case 'l':
			*list = 1;
			break;
		case 'r':
			*reg = 1;
			break;
		case 'u':
			*reg = 0;
			break;
		default:
			fprintf(stderr, "Unknown option '%c'.\n"
				"Try '-h' for help.\n", c);

			return 0;
		}
	}

	if (!*dso_name && !(*dso_name = strdup(default_dso_name)))
		exit(EXIT_FAILURE);

	if (optind >= argc) {
		if (!*list) {
			fprintf(stderr, "You need to specify a device.\n");
			return 0;
		}
	} else if (!(*device = strdup(argv[optind])))
			exit(EXIT_FAILURE);

	return 1;
}

int main(int argc, char **argv)
{
	int list = 0, next = 0, ret, reg = default_reg;
	char *device = NULL, *device_arg, *dso_name = NULL, *dso_name_arg;
	struct log_data *ldata;

	if (!parse_argv(argc, argv, &dso_name, &device, &reg, &list))
		exit(EXIT_FAILURE);

	device_arg = device;
	dso_name_arg = dso_name;


	if(!(ldata = malloc(sizeof(*ldata))))
		exit(ENOMEM);
	if(!memset(ldata, 0, sizeof(*ldata)))
		exit(ENOMEM);

	/* FIXME: use -v/-q options to set this */
	ldata->verbose_level = _LOG_DEBUG;
	multilog_add_type(standard, ldata);

	if (list) {
		do {
			if (!(ret= dm_get_registered_device(&dso_name,
							    &device,
							    &events, next))) {
				printf("%s %s 0x%x\n",
				       dso_name, device, events);

				if (device_arg)
					break;

				next = 1;
			}
		} while (!ret);

		if (dso_name)
			free(dso_name);

		if (device)
			free(device);

		ret = (ret && device_arg) ? EXIT_FAILURE : EXIT_SUCCESS;
		goto out;
	}

	if ((ret = reg ? dm_register_for_event(dso_name, device, events) :
			 dm_unregister_for_event(dso_name, device, events))) {
		fprintf(stderr, "Failed to %sregister %s: %s\n",
			reg ? "": "un", device, strerror(-ret));
		ret = EXIT_FAILURE;
	} else {
		printf("%s %sregistered successfully.\n",
		       device, reg ? "" : "un");
		ret = EXIT_SUCCESS;
	}

   out:
	multilog_del_type(standard, ldata);

	if (device_arg)
		free(device_arg);

	if (dso_name_arg)
		free(dso_name_arg);

	exit(ret);
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
