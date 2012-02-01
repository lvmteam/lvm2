/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_GLOBALS_H
#define _LVM_GLOBALS_H

#define VERBOSE_BASE_LEVEL _LOG_WARN
#define SECURITY_LEVEL 0
#define PV_MIN_SIZE_KB 512

void init_verbose(int level);
void init_test(int level);
void init_md_filtering(int level);
void init_pvmove(int level);
void init_full_scan_done(int level);
void init_obtain_device_list_from_udev(int device_list_from_udev);
void init_trust_cache(int trustcache);
void init_debug(int level);
void init_cmd_name(int status);
void init_ignorelockingfailure(int level);
void init_lockingfailed(int level);
void init_security_level(int level);
void init_mirror_in_sync(int in_sync);
void init_dmeventd_monitor(int reg);
void init_background_polling(int polling);
void init_ignore_suspended_devices(int ignore);
void init_error_message_produced(int produced);
void init_is_static(unsigned value);
void init_udev_checking(int checking);
void init_dev_disable_after_error_count(int value);
void init_pv_min_size(uint64_t sectors);
void init_activation_checks(int checks);
void init_detect_internal_vg_cache_corruption(int detect);
void init_retry_deactivation(int retry);

void set_cmd_name(const char *cmd_name);
void set_sysfs_dir_path(const char *path);

int test_mode(void);
int md_filtering(void);
int pvmove_mode(void);
int full_scan_done(void);
int obtain_device_list_from_udev(void);
int trust_cache(void);
int verbose_level(void);
int debug_level(void);
int ignorelockingfailure(void);
int lockingfailed(void);
int security_level(void);
int mirror_in_sync(void);
int background_polling(void);
int ignore_suspended_devices(void);
const char *log_command_name(void);
unsigned is_static(void);
int udev_checking(void);
const char *sysfs_dir_path(void);
uint64_t pv_min_size(void);
int activation_checks(void);
int detect_internal_vg_cache_corruption(void);
int retry_deactivation(void);

#define DMEVENTD_MONITOR_IGNORE -1
int dmeventd_monitor_mode(void);

#define NO_DEV_ERROR_COUNT_LIMIT 0
int dev_disable_after_error_count(void);

#endif
