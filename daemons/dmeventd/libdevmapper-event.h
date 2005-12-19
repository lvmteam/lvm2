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

/*
 * Note that this file is released only as part of a technology preview
 * and its contents may change in future updates in ways that do not
 * preserve compatibility.
 */

#ifndef LIB_DMEVENT_H
#define LIB_DMEVENT_H

#include <stdint.h>

/* FIXME This stuff must be configurable. */

#define	DM_EVENT_DAEMON		"/sbin/dmeventd"
#define DM_EVENT_LOCKFILE	"/var/lock/dmeventd"
#define	DM_EVENT_FIFO_CLIENT	"/var/run/dmeventd-client"
#define	DM_EVENT_FIFO_SERVER	"/var/run/dmeventd-server"
#define DM_EVENT_PIDFILE	"/var/run/dmeventd.pid"

#define DM_EVENT_DEFAULT_TIMEOUT 10

/* Commands for the daemon passed in the message below. */
enum dm_event_command {
	DM_EVENT_CMD_ACTIVE = 1,
	DM_EVENT_CMD_REGISTER_FOR_EVENT,
	DM_EVENT_CMD_UNREGISTER_FOR_EVENT,
	DM_EVENT_CMD_GET_REGISTERED_DEVICE,
	DM_EVENT_CMD_GET_NEXT_REGISTERED_DEVICE,
	DM_EVENT_CMD_SET_TIMEOUT,
	DM_EVENT_CMD_GET_TIMEOUT,
};

/* Message passed between client and daemon. */
struct dm_event_daemon_message {
	union {
		unsigned int cmd;	/* FIXME Use fixed size. */
		int	 status;	/* FIXME Use fixed size. */
	} opcode;
	char msg[252];		/* FIXME Why is this 252 ? */
} __attribute__((packed));	/* FIXME Do this properly! */

/* FIXME Is this meant to be exported?  I can't see where the interface uses it. */
/* Fifos for client/daemon communication. */
struct dm_event_fifos {
	int client;
	int server;
	const char *client_path;
	const char *server_path;
};

/* Event type definitions. */
/* FIXME Use masks to separate the types and provide for extension. */
enum dm_event_type {
	DM_EVENT_SINGLE		= 0x01, /* Report multiple errors just once. */
	DM_EVENT_MULTI		= 0x02, /* Report all of them. */

	DM_EVENT_SECTOR_ERROR	= 0x04, /* Failure on a particular sector. */
	DM_EVENT_DEVICE_ERROR	= 0x08, /* Device failure. */
	DM_EVENT_PATH_ERROR	= 0x10, /* Failure on an io path. */
	DM_EVENT_ADAPTOR_ERROR	= 0x20, /* Failure off a host adaptor. */

	DM_EVENT_SYNC_STATUS	= 0x40, /* Mirror synchronization completed/failed. */
	DM_EVENT_TIMEOUT	= 0x80, /* Timeout has occured */
};

/* FIXME Use a mask. */
#define	DM_EVENT_ALL_ERRORS (DM_EVENT_SECTOR_ERROR | DM_EVENT_DEVICE_ERROR | \
			     DM_EVENT_PATH_ERROR | DM_EVENT_ADAPTOR_ERROR)

/* Prototypes for event lib interface. */

/* FIXME Replace device with standard name/uuid/devno choice */
/* Interface changes: 
   First register a handler, passing in a unique ref for the device. */
//  int dm_event_register_handler(const char *dso_name, const char *device);
//  int dm_event_register(const char *dso_name, const char *name, const char *uuid, uint32_t major, uint32_t minor, enum dm_event_type events);
/* Or (better?) add to task structure and use existing functions - run a task to register/unregister events - we may need to run task withe that with the new event mechanism anyway, then the dso calls just hook in.
*/
 
/* FIXME Missing consts? */
int dm_event_register(char *dso_name, char *device, enum dm_event_type events);
int dm_event_unregister(char *dso_name, char *device,
			enum dm_event_type events);
int dm_event_get_registered_device(char **dso_name, char **device,
				   enum dm_event_type *events, int next);
int dm_event_set_timeout(char *device, uint32_t timeout);
int dm_event_get_timeout(char *device, uint32_t *timeout);

/* Prototypes for DSO interface. */
void process_event(const char *device, enum dm_event_type event);
int register_device(const char *device);
int unregister_device(const char *device);

#endif
