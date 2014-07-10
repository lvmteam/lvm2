/*
 * Copyright (C) 2014 Red Hat, Inc. All rights reserved.
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

/*
 * This file defines reserved names for field values.
 *
 * This is used for registering reserved names with reporting code that
 * uses the exact value defined whenever the reserved name is hit, for
 * example during selection criteria processing.
 *
 * TYPE_RESERVED_VALUE defines reserved value that is not bound to any field,
 * but rather it's bound to a certain type. This can be used as a reserved
 * value for all fields of that type then.
 *
 * FIELD_RESERVED_VALUE defines reserved value bound to a single field.
 *
 * FIELD_BINARY_RESERVED_VALUE is similar to FIELD_RESERVED_VALUE but it
 * is specifically designed for defintion of reserved names for fields
 * with binary values where the reserved names given denote value 1.
 * The first reserved_name given is also used for reporting,
 * others are synonyms which are recognized in addition.
 *
 */

/*
 * TYPE_RESERVED_VALUE(type, reserved_value_id, description, value, reserved_name, ...)
 * FIELD_RESERVED_VALUE(field_id, reserved_value_id, description, value, reserved_name, ...)
 * FIELD_BINARY_RESERVED_VALUE(field_id, reserved_value_id, description, reserved_name for 1, ...)
 */

/* *INDENT-OFF* */

/* Per-type reserved values usable for all fields of certain type. */
TYPE_RESERVED_VALUE(NUM, number_undef_64, "Reserved value for undefined numeric value.", UINT64_C(-1), "-1", "unknown", "undefined", "undef")

/* Reserved values for PV fields */
FIELD_RESERVED_BINARY_VALUE(pv_allocatable, pv_allocatable, "", "allocatable")
FIELD_RESERVED_BINARY_VALUE(pv_exported, pv_exported, "", "exported")
FIELD_RESERVED_BINARY_VALUE(pv_missing, pv_missing, "", "missing")

/* Reserved values for VG fields */
FIELD_RESERVED_BINARY_VALUE(vg_extendable, vg_extendable, "", "extendable")
FIELD_RESERVED_BINARY_VALUE(vg_exported, vg_exported, "", "exported")
FIELD_RESERVED_BINARY_VALUE(vg_partial, vg_partial, "", "partial")
FIELD_RESERVED_BINARY_VALUE(vg_clustered, vg_clustered, "", "clustered")
FIELD_RESERVED_VALUE(vg_permissions, vg_permissions_rw, "", FIRST_NAME(vg_permissions_rw), "writeable", "rw", "read-write")
FIELD_RESERVED_VALUE(vg_permissions, vg_permissions_r, "", FIRST_NAME(vg_permissions_r), "read-only", "r", "ro")
FIELD_RESERVED_VALUE(vg_mda_copies, vg_mda_copies, "", RESERVED(number_undef_64), "unmanaged")

/* Reserved values for LV fields */
FIELD_RESERVED_BINARY_VALUE(lv_initial_image_sync, lv_initial_image_sync, "", "initial image sync", "sync")
FIELD_RESERVED_BINARY_VALUE(lv_image_synced, lv_image_synced, "", "image synced", "synced")
FIELD_RESERVED_BINARY_VALUE(lv_merging, lv_merging, "", "merging")
FIELD_RESERVED_BINARY_VALUE(lv_converting, lv_converting, "", "converting")
FIELD_RESERVED_BINARY_VALUE(lv_allocation_locked, lv_allocation_locked, "", "allocation locked", "locked")
FIELD_RESERVED_BINARY_VALUE(lv_fixed_minor, lv_fixed_minor, "", "fixed minor", "fixed")
FIELD_RESERVED_BINARY_VALUE(lv_active_locally, lv_active_locally, "", "active locally", "active", "locally")
FIELD_RESERVED_BINARY_VALUE(lv_active_remotely, lv_active_remotely, "", "active remotely", "active", "remotely")
FIELD_RESERVED_BINARY_VALUE(lv_active_exclusively, lv_active_exclusively, "", "active exclusively", "active", "exclusively")
FIELD_RESERVED_BINARY_VALUE(lv_merge_failed, lv_merge_failed, "", "merge failed", "failed")
FIELD_RESERVED_BINARY_VALUE(lv_snapshot_invalid, lv_snapshot_invalid, "", "snapshot invalid", "invalid")
FIELD_RESERVED_BINARY_VALUE(lv_suspended, lv_suspended, "", "suspended")
FIELD_RESERVED_BINARY_VALUE(lv_live_table, lv_live_table, "", "live table present", "live table", "live")
FIELD_RESERVED_BINARY_VALUE(lv_inactive_table, lv_inactive_table, "", "inactive table present", "inactive table", "inactive")
FIELD_RESERVED_BINARY_VALUE(lv_device_open, lv_device_open, "", "open")
FIELD_RESERVED_BINARY_VALUE(lv_skip_activation, lv_skip_activation, "", "skip activation", "skip")
FIELD_RESERVED_BINARY_VALUE(zero, zero, "", "zero")
FIELD_RESERVED_VALUE(lv_permissions, lv_permissions_rw, "", FIRST_NAME(lv_permissions_rw), "writeable", "rw", "read-write")
FIELD_RESERVED_VALUE(lv_permissions, lv_permissions_r, "", FIRST_NAME(lv_permissions_r), "", "read-only", "r", "ro")
FIELD_RESERVED_VALUE(lv_permissions, lv_permissions_r_override, "", FIRST_NAME(lv_permissions_r_override), "", "read-only-override", "ro-override", "r-override", "R")
FIELD_RESERVED_VALUE(lv_read_ahead, lv_read_ahead, "", RESERVED(number_undef_64), "auto")

/* *INDENT-ON* */
