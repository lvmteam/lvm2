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

/* Event type definitions. */
enum dm_event_type {
	DM_EVENT_SETTINGS_MASK  = 0x0000FF,
	DM_EVENT_SINGLE		= 0x000001, /* Report multiple errors just once. */
	DM_EVENT_MULTI		= 0x000002, /* Report all of them. */

	DM_EVENT_ERROR_MASK     = 0x00FF00,
	DM_EVENT_SECTOR_ERROR	= 0x000100, /* Failure on a particular sector. */
	DM_EVENT_DEVICE_ERROR	= 0x000200, /* Device failure. */
	DM_EVENT_PATH_ERROR	= 0x000400, /* Failure on an io path. */
	DM_EVENT_ADAPTOR_ERROR	= 0x000800, /* Failure off a host adaptor. */

	DM_EVENT_STATUS_MASK    = 0xFF0000,
	DM_EVENT_SYNC_STATUS	= 0x010000, /* Mirror synchronization completed/failed. */
	DM_EVENT_TIMEOUT	= 0x020000, /* Timeout has occured */

	DM_EVENT_REGISTRATION_PENDING = 0x1000000, /* Monitor thread is setting-up/shutting-down */
};

#define DM_EVENT_ALL_ERRORS DM_EVENT_ERROR_MASK

/* Prototypes for event lib interface. */

struct dm_event_handler;

/* Create and destroy dm_event_handler struct, which is passed to
   register/unregister functions below */
struct dm_event_handler *dm_event_handler_create(void);
void dm_event_handler_destroy(struct dm_event_handler *h);

/* Set parameters of a handler:
   - dso - shared library path to handle the events
   (only one of the following three needs to be set)
   - name - device name or path
   - uuid - device uuid
   - major and minor - device major/minor numbers
   - events - a bitfield defining which events to handle (see
              enum dm_event_type above)
*/
void dm_event_handler_set_dso(struct dm_event_handler *h, const char *path);
void dm_event_handler_set_name(struct dm_event_handler *h, const char *name);
void dm_event_handler_set_uuid(struct dm_event_handler *h, const char *uuid);
void dm_event_handler_set_major(struct dm_event_handler *h, int major);
void dm_event_handler_set_minor(struct dm_event_handler *h, int minor);
void dm_event_handler_set_events(struct dm_event_handler *h,
				 enum dm_event_type event);

/* Get parameters of a handler, same as above */
const char *dm_event_handler_get_dso(const struct dm_event_handler *h);
const char *dm_event_handler_get_name(const struct dm_event_handler *h);
const char *dm_event_handler_get_uuid(const struct dm_event_handler *h);
int dm_event_handler_get_major(const struct dm_event_handler *h);
int dm_event_handler_get_minor(const struct dm_event_handler *h);
enum dm_event_type dm_event_handler_get_events(const struct dm_event_handler *h);

/* Call out to dmeventd to register or unregister a handler. If
   dmeventd is not running, it is spawned first. */
int dm_event_register(const struct dm_event_handler *h);
int dm_event_unregister(const struct dm_event_handler *h);

/* Prototypes for DSO interface, see dmeventd.c, struct dso_data for
   detailed descriptions. */
void process_event(struct dm_task *dmt, enum dm_event_type event);
int register_device(const char *device, const char *uuid, int major, int minor);
int unregister_device(const char *device, const char *uuid, int major,
		      int minor);

#endif
