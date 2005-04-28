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

#ifndef LIB_DMEVENT_H
#define LIB_DMEVENT_H

#include "list.h"

#include <stdint.h>

#define	DAEMON		"/sbin/dmeventd"
#define LOCKFILE	"/var/lock/dmeventd"
#define	FIFO_CLIENT	"/var/run/dmeventd-client"
#define	FIFO_SERVER	"/var/run/dmeventd-server"
#define PIDFILE		"/var/run/dmeventd.pid"

/* Commands for the daemon passed in the message below. */
enum dmeventd_command {
	CMD_ACTIVE = 1,
	CMD_REGISTER_FOR_EVENT,
	CMD_UNREGISTER_FOR_EVENT,
	CMD_GET_NEXT_REGISTERED_DEVICE,
};

/* Message passed between client and daemon. */
struct daemon_message {
	union {
		unsigned int cmd;
		int	 status;
	} opcode;
	char msg[252];
} __attribute__((packed));

/* Fifos for client/daemon communication. */
struct fifos
{
	int client;
	int server;
	char *client_path;
	char *server_path;
};

/* Event type definitions. */
enum event_type {
	SINGLE		= 0x01, /* Report multiple errors just once. */
	MULTI		= 0x02, /* Report all of them. */
	SECTOR_ERROR	= 0x04, /* Failure on a particular sector. */
	DEVICE_ERROR	= 0x08, /* Device failure. */
	PATH_ERROR	= 0x10, /* Failure on an io path. */
	ADAPTOR_ERROR	= 0x20, /* Failure off a host adaptor. */
	SYNC_STATUS	= 0x40, /* Mirror synchronization completed/failed. */
};
#define	ALL_ERRORS (SECTOR_ERROR | DEVICE_ERROR | PATH_ERROR | ADAPTOR_ERROR)

int dm_register_for_event(char *dso_name, char *device, enum event_type events);
int dm_unregister_for_event(char *dso_name, char *device,
			    enum event_type events);
int dm_get_next_registered_device(char **dso_name, char **device,
			    enum event_type *events);

#endif

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
