/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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
#include "device.h"
#include "memlock.h"
#include "lvm-string.h"
#include "defaults.h"
#include "metadata-exported.h"

#include <stdarg.h>

static int _verbose_level = VERBOSE_BASE_LEVEL;
static int _silent = 0;
static int _test = 0;
static int _md_filtering = 0;
static int _pvmove = 0;
static int _full_scan_done = 0;	/* Restrict to one full scan during each cmd */
static int _obtain_device_list_from_udev = DEFAULT_OBTAIN_DEVICE_LIST_FROM_UDEV;
static int _trust_cache = 0; /* Don't scan when incomplete VGs encountered */
static int _debug_level = 0;
static int _debug_classes_logged = DEFAULT_LOGGED_DEBUG_CLASSES;
static int _log_cmd_name = 0;
static int _ignorelockingfailure = 0;
static int _security_level = SECURITY_LEVEL;
static char _cmd_name[30] = "";
static int _mirror_in_sync = 0;
static int _dmeventd_monitor = DEFAULT_DMEVENTD_MONITOR;
static int _background_polling = DEFAULT_BACKGROUND_POLLING;
static int _ignore_suspended_devices = 0;
static int _error_message_produced = 0;
static unsigned _is_static = 0;
static int _udev_checking = 1;
static int _retry_deactivation = DEFAULT_RETRY_DEACTIVATION;
static int _activation_checks = 0;
static char _sysfs_dir_path[PATH_MAX] = "";
static int _dev_disable_after_error_count = DEFAULT_DISABLE_AFTER_ERROR_COUNT;
static uint64_t _pv_min_size = (DEFAULT_PV_MIN_SIZE_KB * 1024L >> SECTOR_SHIFT);
static int _detect_internal_vg_cache_corruption =
	DEFAULT_DETECT_INTERNAL_VG_CACHE_CORRUPTION;

void init_verbose(int level)
{
	_verbose_level = level;
}

void init_silent(int silent)
{
	_silent = silent;
}

void init_test(int level)
{
	if (!_test && level)
		log_warn("TEST MODE: Metadata will NOT be updated and volumes will not be (de)activated.");
	_test = level;
}

void init_md_filtering(int level)
{
	_md_filtering = level;
}

void init_pvmove(int level)
{
	_pvmove = level;
}

void init_full_scan_done(int level)
{
	_full_scan_done = level;
}

void init_obtain_device_list_from_udev(int device_list_from_udev)
{
	_obtain_device_list_from_udev = device_list_from_udev;
}

void init_trust_cache(int trustcache)
{
	_trust_cache = trustcache;
}

void init_ignorelockingfailure(int level)
{
	_ignorelockingfailure = level;
}

void init_security_level(int level)
{
	_security_level = level;
}

void init_mirror_in_sync(int in_sync)
{
	_mirror_in_sync = in_sync;
}

void init_dmeventd_monitor(int reg)
{
	_dmeventd_monitor = reg;
}

void init_background_polling(int polling)
{
	_background_polling = polling;
}

void init_ignore_suspended_devices(int ignore)
{
	_ignore_suspended_devices = ignore;
}

void init_cmd_name(int status)
{
	_log_cmd_name = status;
}

void init_is_static(unsigned value)
{
	_is_static = value;
}

void init_udev_checking(int checking)
{
	if ((_udev_checking = checking))
		log_debug_activation("LVM udev checking enabled");
	else
		log_debug_activation("LVM udev checking disabled");
}

void init_retry_deactivation(int retry)
{
	_retry_deactivation = retry;
}

void init_activation_checks(int checks)
{
	if ((_activation_checks = checks))
		log_debug_activation("LVM activation checks enabled");
	else
		log_debug_activation("LVM activation checks disabled");
}

void init_dev_disable_after_error_count(int value)
{
	_dev_disable_after_error_count = value;
}

void init_pv_min_size(uint64_t sectors)
{
	_pv_min_size = sectors;
}

void init_detect_internal_vg_cache_corruption(int detect)
{
	_detect_internal_vg_cache_corruption = detect;
}

void set_cmd_name(const char *cmd)
{
	strncpy(_cmd_name, cmd, sizeof(_cmd_name) - 1);
	_cmd_name[sizeof(_cmd_name) - 1] = '\0';
}

void set_sysfs_dir_path(const char *path)
{
	strncpy(_sysfs_dir_path, path, sizeof(_sysfs_dir_path) - 1);
	_sysfs_dir_path[sizeof(_sysfs_dir_path) - 1] = '\0';
}

const char *log_command_name(void)
{
	if (!_log_cmd_name)
		return "";

	return _cmd_name;
}

void init_error_message_produced(int value)
{
	_error_message_produced = value;
}

int error_message_produced(void)
{
	return _error_message_produced;
}

int test_mode(void)
{
	return _test;
}

int md_filtering(void)
{
	return _md_filtering;
}

int pvmove_mode(void)
{
	return _pvmove;
}

int full_scan_done(void)
{
	return _full_scan_done;
}

int obtain_device_list_from_udev(void)
{
	return _obtain_device_list_from_udev;
}

int trust_cache(void)
{
	return _trust_cache;
}

int background_polling(void)
{
	return _background_polling;
}

int ignorelockingfailure(void)
{
	return _ignorelockingfailure;
}

int security_level(void)
{
	return _security_level;
}

int mirror_in_sync(void)
{
	return _mirror_in_sync;
}

int dmeventd_monitor_mode(void)
{
	return _dmeventd_monitor;
}

int ignore_suspended_devices(void)
{
	return _ignore_suspended_devices;
}

void init_debug(int level)
{
	_debug_level = level;
}

void init_debug_classes_logged(int classes)
{
	_debug_classes_logged = classes;
}

int debug_class_is_logged(int class)
{
	/* If no class given, log it */
	if (!class)
		return 1;

	return (_debug_classes_logged & class) ? 1 : 0;
}

int verbose_level(void)
{
	return _verbose_level;
}

int debug_level(void)
{
	return _debug_level;
}

int silent_mode(void)
{
	return _silent;
}

unsigned is_static(void)
{
	return _is_static;
}

int udev_checking(void)
{
	return _udev_checking;
}

int retry_deactivation(void)
{
	return _retry_deactivation;
}

int activation_checks(void)
{
	return _activation_checks;
}

const char *sysfs_dir_path(void)
{
	return _sysfs_dir_path;
}

int dev_disable_after_error_count(void)
{
	return _dev_disable_after_error_count;
}

uint64_t pv_min_size(void)
{
	return _pv_min_size;
}

int detect_internal_vg_cache_corruption(void)
{
	return _detect_internal_vg_cache_corruption;
}
