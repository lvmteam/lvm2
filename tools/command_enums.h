/*
 * Copyright (C) 2024 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _LVM_COMMAND_ENUMS_H
#define _LVM_COMMAND_ENUMS_H

/*
 * cmd_enum.h uses the generated cmds.h to create the enum with an ID
 * for each command definition in command-lines.in.
 */
#include "lib/commands/cmd_enum.h"

/* define the enums for the command line --options, foo_ARG */
enum {
#define arg(a, b, c, d, e, f, g) a ,
#include "args.h"
#undef arg
};

/* define the enums for the values accepted by command line --options, foo_VAL */
enum {
#define val(a, b, c, d) a ,
#include "vals.h"
#undef val
};

/* define enums for LV properties, foo_LVP */
enum {
	LVP_NONE,
#define lvp(a) a ## _LVP ,
#include "lv_props.h"
#undef lvp
	LVP_COUNT
};

/* define enums for LV types, foo_LVT */
enum {
	LVT_NONE,
#define lvt(a) a ## _LVT ,
#include "lv_types.h"
#undef lvt
	LVT_COUNT
};

enum {
#define xx(a, b...) a ## _COMMAND,
#include "commands.h"
#undef xx
        LVM_COMMAND_COUNT
};

#define PERMITTED_READ_ONLY 	0x00000002
/* Process all VGs if none specified on the command line. */
#define ALL_VGS_IS_DEFAULT	0x00000004
/* Process all devices with --all if none are specified on the command line. */
#define ENABLE_ALL_DEVS		0x00000008	
/* Command may try to interpret a vgname arg as a uuid. */
#define ALLOW_UUID_AS_NAME	0x00000010
/* Command needs a shared lock on a VG; it only reads the VG. */
#define LOCKD_VG_SH		0x00000020
/* Command does not process any metadata. */
#define NO_METADATA_PROCESSING	0x00000040
/* Command must use all specified arg names and fail if all cannot be used. */
#define MUST_USE_ALL_ARGS        0x00000100
/* Command should process unused duplicate devices. */
#define ENABLE_DUPLICATE_DEVS    0x00000400
/* Command does not accept tags as args. */
#define DISALLOW_TAG_ARGS        0x00000800
/* Command may need to find VG name in an option value. */
#define GET_VGNAME_FROM_OPTIONS  0x00001000
/* The data read from disk by label scan can be used for vg_read. */
#define CAN_USE_ONE_SCAN	 0x00002000
/* Command can use hints file */
#define ALLOW_HINTS		 0x00004000
/* Command can access exported vg. */
#define ALLOW_EXPORTED           0x00008000
/* Command checks and reports warning if devs used by LV are incorrect. */
#define CHECK_DEVS_USED		 0x00010000
/* Command prints devices file entries that were not found. */
#define DEVICE_ID_NOT_FOUND      0x00020000
/* Command prints devices file entries that were not found. */
#define ALTERNATIVE_EXTENTS	 0x00040000

#include "command.h"       /* defines struct command */
#include "command-count.h" /* defines COMMAND_COUNT */

#endif /* _LVM_COMMAND_ENUMS_H */
