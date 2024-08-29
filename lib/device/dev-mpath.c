/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/activate/activate.h"
#include "lib/commands/toolcontext.h"
#include "lib/device/device_id.h"
#include "lib/datastruct/str_list.h"
#include "device_mapper/misc/dm-ioctl.h"
#ifdef UDEV_SYNC_SUPPORT
#include <libudev.h>
#include "lib/device/dev-ext-udev-constants.h"
#endif

#include <dirent.h>
#include <ctype.h>

#define MPATH_PREFIX "mpath-"

/*
 * This hash table keeps track of whether a given dm device
 * is a mpath device or not.
 *
 * If dm-3 is an mpath device, then the constant "2" is stored in
 * the hash table with the key of the dm minor number ("3" for dm-3).
 * If dm-3 is not an mpath device, then the constant "1" is stored in
 * the hash table with the key of the dm minor number.
 */
static struct dm_pool *_wwid_mem;
static struct dm_hash_table *_minor_hash_tab;
static struct dm_hash_table *_wwid_hash_tab;
static struct dm_list _ignored;
static struct dm_list _ignored_exceptions;

#define MAX_WWID_LINE 512

static void _read_blacklist_file(const char *path)
{
	FILE *fp;
	char line[MAX_WWID_LINE];
	char wwid[MAX_WWID_LINE];
	char *word;
	int section_black = 0;
	int section_exceptions = 0;
	int found_quote;
	int found_type;
	int i, j;

	if (!(fp = fopen(path, "r")))
		return;

	while (fgets(line, sizeof(line), fp)) {
		word = NULL;

		/* skip initial white space on the line */
		for (i = 0; i < MAX_WWID_LINE; i++) {
			if ((line[i] == '\n') || (line[i] == '\0'))
				break;
			if (isspace(line[i]))
				continue;
			word = &line[i];
			break;
		}

		if (!word || word[0] == '#')
			continue;

		/* identify the start of the section we want to read */
		if (strchr(word, '{')) {
			if (!strncmp(word, "blacklist_exceptions", 20))
				section_exceptions = 1;
			else if (!strncmp(word, "blacklist", 9))
				section_black = 1;
			continue;
		}
		/* identify the end of the section we've been reading */
		if (strchr(word, '}')) {
			section_exceptions = 0;
			section_black = 0;
			continue;
		}
		/* skip lines that are not in a section we want */
		if (!section_black && !section_exceptions)
			continue;

		/*
		 * read a wwid from the blacklist{_exceptions} section.
		 * does not recognize other non-wwid entries in the
		 * section, and skips those (should the entire mp
		 * config filtering be disabled if non-wwids are seen?
		 */
		if (!strstr(word, "wwid"))
			continue;

		i += 4; /* skip "wwid" */

		/*
		 * copy wwid value from the line.
		 * the wwids copied here need to match the
		 * wwids read from /etc/multipath/wwids,
		 * which are matched to wwids from sysfs.
		 */

		memset(wwid, 0, sizeof(wwid));
		found_quote = 0;
		found_type = 0;
		j = 0;

		for (; i < MAX_WWID_LINE; i++) {
			if ((line[i] == '\n') || (line[i] == '\0'))
				break;
			if (!j && isspace(line[i]))
				continue;
			if (isspace(line[i]))
				break;
			/* quotes around wwid are optional */
			if ((line[i] == '"') && !found_quote) {
				found_quote = 1;
				continue;
			}
			/* second quote is end of wwid */
			if ((line[i] == '"') && found_quote)
				break;
			/* exclude initial 3/2/1 for naa/eui/t10 */
			if (!j && !found_type &&
			    ((line[i] == '3') || (line[i] == '2') || (line[i] == '1'))) {
				found_type = 1;
				continue;
			}

			wwid[j] = line[i];
			j++;
		}

		if (j < 8)
			continue;

		log_debug("multipath wwid %s in %s %s",
			  wwid, section_exceptions ? "blacklist_exceptions" : "blacklist", path);

		if (section_exceptions) {
			if (!str_list_add(_wwid_mem, &_ignored_exceptions, dm_pool_strdup(_wwid_mem, wwid)))
				stack;
		} else {
			if (!str_list_add(_wwid_mem, &_ignored, dm_pool_strdup(_wwid_mem, wwid)))
				stack;
		}
	}

	if (fclose(fp))
		stack;
}

static void _read_wwid_exclusions(void)
{
	static const char _mpath_conf[] = "/etc/multipath/conf.d";
	char path[PATH_MAX] = { 0 };
	DIR *dir;
	struct dirent *de;
	struct dm_str_list *sl, *sl2;
	int rem_count = 0;

	_read_blacklist_file("/etc/multipath.conf");

	if ((dir = opendir(_mpath_conf))) {
		while ((de = readdir(dir))) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%s", _mpath_conf, de->d_name);
			_read_blacklist_file(path);
		}
		if (closedir(dir))
                        log_sys_debug("closedir", _mpath_conf);
	}

	/* for each wwid in ignored_exceptions, remove it from ignored */

	dm_list_iterate_items_safe(sl, sl2, &_ignored) {
		if (str_list_match_item(&_ignored_exceptions, sl->str))
			str_list_del(&_ignored, sl->str);
	}

	/* for each wwid in ignored, remove it from wwid_hash */

	dm_list_iterate_items(sl, &_ignored) {
		dm_hash_remove_binary(_wwid_hash_tab, sl->str, strlen(sl->str));
		rem_count++;
	}

	if (rem_count)
		log_debug("multipath config ignored %d wwids", rem_count);
}

static void _read_wwid_file(const char *config_wwids_file, int *entries)
{
	FILE *fp;
	char line[MAX_WWID_LINE];
	char *wwid, *p;
	char typestr[2] = { 0 };
	int count = 0;

	if (config_wwids_file[0] != '/') {
		log_print_unless_silent("Ignoring unknown multipath_wwids_file.");
		return;
	}

	if (!(fp = fopen(config_wwids_file, "r"))) {
		log_debug("multipath wwids file not found");
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#')
			continue;

		wwid = line;

		if (line[0] == '/')
			wwid++;


		/*
		 * the initial character is the id type,
		 * 1 is t10, 2 is eui, 3 is naa, 8 is scsi name.
		 * wwids are stored in the hash table without the type character.
		 * It seems that sometimes multipath does not include
		 * the type character (seen with t10 scsi_debug devs).
		 */
		typestr[0] = *wwid;
		if (typestr[0] == '1' || typestr[0] == '2' || typestr[0] == '3')
			wwid++;

		if ((p = strchr(wwid, '/')))
			*p = '\0';

		(void) dm_hash_insert_binary(_wwid_hash_tab, wwid, strlen(wwid), (void*)1);
		count++;
	}

	if (fclose(fp))
		stack;

	log_debug("multipath wwids read %d from %s", count, config_wwids_file);
	*entries = count;
}

int dev_mpath_init(const char *config_wwids_file)
{
	struct dm_pool *mem;
	struct dm_hash_table *minor_tab;
	struct dm_hash_table *wwid_tab;
	int entries = 0;

	dm_list_init(&_ignored);
	dm_list_init(&_ignored_exceptions);

	if (!(mem = dm_pool_create("mpath", 256))) {
		log_error("mpath pool creation failed.");
		return 0;
	}

	if (!(minor_tab = dm_hash_create(110))) {
		log_error("mpath hash table creation failed.");
		dm_pool_destroy(mem);
		return 0;
	}

	_wwid_mem = mem;
	_minor_hash_tab = minor_tab;

	/* multipath_wwids_file="" disables the use of the file */
	if (config_wwids_file && !strlen(config_wwids_file)) {
		log_debug("multipath wwids file disabled.");
		return 1;
	}

	if (!(wwid_tab = dm_hash_create(110))) {
		log_error("mpath hash table creation failed.");
		dm_hash_destroy(_minor_hash_tab);
		dm_pool_destroy(_wwid_mem);
		_minor_hash_tab = NULL;
		_wwid_mem = NULL;
		return 0;
	}

	_wwid_hash_tab = wwid_tab;

	if (config_wwids_file) {
		_read_wwid_file(config_wwids_file, &entries);
		_read_wwid_exclusions();
	}

	if (!entries) {
		/* reading dev wwids is skipped with null wwid_hash_tab */
		dm_hash_destroy(_wwid_hash_tab);
		_wwid_hash_tab = NULL;
	}

	return 1;
}

void dev_mpath_exit(void)
{
	if (_minor_hash_tab)
		dm_hash_destroy(_minor_hash_tab);
	if (_wwid_hash_tab)
		dm_hash_destroy(_wwid_hash_tab);
	if (_wwid_mem)
		dm_pool_destroy(_wwid_mem);

	_minor_hash_tab = NULL;
	_wwid_hash_tab = NULL;
	_wwid_mem = NULL;
}


/*
 * given "/dev/foo" return "foo"
 */
static const char *_get_sysfs_name(struct device *dev)
{
	const char *name;

	if (!(name = strrchr(dev_name(dev), '/'))) {
		log_error("Cannot find '/' in device name.");
		return NULL;
	}
	name++;

	if (!*name) {
		log_error("Device name is not valid.");
		return NULL;
	}

	return name;
}

/*
 * given major:minor
 * readlink translates /sys/dev/block/major:minor to /sys/.../foo
 * from /sys/.../foo return "foo"
 */
static const char *_get_sysfs_name_by_devt(const char *sysfs_dir, dev_t devno,
					  char *buf, size_t buf_size)
{
	const char *name;
	char path[PATH_MAX];
	int size;

	if (dm_snprintf(path, sizeof(path), "%sdev/block/%u:%u", sysfs_dir,
			MAJOR(devno), MINOR(devno)) < 0) {
		log_error("Sysfs path string is too long.");
		return NULL;
	}

	if ((size = readlink(path, buf, buf_size - 1)) < 0) {
		log_sys_error("readlink", path);
		return NULL;
	}
	buf[size] = '\0';

	if (!(name = strrchr(buf, '/'))) {
		log_error("Cannot find device name in sysfs path.");
		return NULL;
	}
	name++;

	return name;
}

#ifdef UDEV_SYNC_SUPPORT
static int _dev_is_mpath_component_udev(struct device *dev)
{
	const char *value;
	struct dev_ext *ext;

	/*
	 * external_device_info_source="udev" enables these udev checks.
	 * external_device_info_source="none" disables them.
	 */

	if (!(ext = dev_ext_get(dev)))
		return_0;

	value = udev_device_get_property_value((struct udev_device *)ext->handle, DEV_EXT_UDEV_BLKID_TYPE);
	if (value && !strcmp(value, DEV_EXT_UDEV_BLKID_TYPE_MPATH))
		return 1;

	value = udev_device_get_property_value((struct udev_device *)ext->handle, DEV_EXT_UDEV_MPATH_DEVICE_PATH);
	if (value && !strcmp(value, "1"))
		return 1;

	return 0;
}
#else
static int _dev_is_mpath_component_udev(struct device *dev)
{
	return 0;
}
#endif

/* mpath_devno is major:minor of the dm multipath device currently using the component dev. */

static int _dev_is_mpath_component_sysfs(struct cmd_context *cmd, struct device *dev,
					 int primary_result, dev_t primary_dev, dev_t *mpath_devno)
{
	struct dev_types *dt = cmd->dev_types;
	const char *name;               /* e.g. "sda" for "/dev/sda" */
	char link_path[PATH_MAX];       /* some obscure, unpredictable sysfs path */
	char holders_path[PATH_MAX];    /* e.g. "/sys/block/sda/holders/" */
	char dm_dev_path[PATH_MAX];     /* e.g. "/dev/dm-1" */
	char uuid[DM_UUID_LEN];
	char *holder_name;		/* e.g. "dm-1" */
	const char *sysfs_dir = dm_sysfs_dir();
	DIR *dr;
	struct dirent *de;
	unsigned dev_major = MAJOR(dev->dev);
	unsigned dev_minor = MINOR(dev->dev);
	unsigned dm_dev_major;
	unsigned dm_dev_minor;
	struct stat info;
	int is_mpath_component = 0;

	switch (primary_result) {

	case 2: /* The dev is partition. */

		/* gets "foo" for "/dev/foo" where "/dev/foo" comes from major:minor */
		if (!(name = _get_sysfs_name_by_devt(sysfs_dir, primary_dev, link_path, sizeof(link_path))))
			return_0;
		break;

	case 1: /* The dev is already a primary dev. Just continue with the dev. */

		/* gets "foo" for "/dev/foo" */
		if (!(name = _get_sysfs_name(dev)))
			return_0;
		break;

	default: /* 0, error. */
		log_warn("Failed to get primary device for %u:%u.", dev_major, dev_minor);
		return 0;
	}

	if (dm_snprintf(holders_path, sizeof(holders_path), "%sblock/%s/holders", sysfs_dir, name) < 0) {
		log_warn("Sysfs path to check mpath is too long.");
		return 0;
	}

	/* also will filter out partitions */
	if (stat(holders_path, &info))
		return 0;

	if (!S_ISDIR(info.st_mode)) {
		log_warn("Path %s is not a directory.", holders_path);
		return 0;
	}

	/*
	 * If any holder is a dm mpath device, then return 1;
	 */

	if (!(dr = opendir(holders_path))) {
		log_debug("Device %s has no holders dir", dev_name(dev));
		return 0;
	}

	while ((de = readdir(dr))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		/*
		 * holder_name is e.g. "dm-1"
		 * dm_dev_path is then e.g. "/dev/dm-1"
		 */
		holder_name = de->d_name;

		if (dm_snprintf(dm_dev_path, sizeof(dm_dev_path), "%s/%s", cmd->dev_dir, holder_name) < 0) {
			log_warn("dm device path to check mpath is too long.");
			continue;
		}

		/*
		 * stat "/dev/dm-1" which is the holder of the dev we're checking
		 * dm_dev_major:dm_dev_minor come from stat("/dev/dm-1")
		 */
		if (stat(dm_dev_path, &info)) {
			log_debug_devs("dev_is_mpath_component %s holder %s stat result %d",
					dev_name(dev), dm_dev_path, errno);
			continue;
		}
		dm_dev_major = MAJOR(info.st_rdev);
		dm_dev_minor = MINOR(info.st_rdev);
	
		if (dm_dev_major != dt->device_mapper_major) {
			log_debug_devs("dev_is_mpath_component %s holder %s %d:%d does not have dm major",
					dev_name(dev), dm_dev_path, dm_dev_major, dm_dev_minor);
			continue;
		}

		/*
		 * A previous call may have checked if dm_dev_minor is mpath and saved
		 * the result in the hash table.  If there's a saved result just use that.
		 *
		 * The minor number of "/dev/dm-1" is added to the hash table with
		 * const value 2 meaning that dm minor 1 (for /dev/dm-1) is a multipath dev
		 * and const value 1 meaning that dm minor 1 is not a multipath dev.
		 */

		if (_minor_hash_tab) {
			void *d = dm_hash_lookup_binary(_minor_hash_tab, &dm_dev_minor, sizeof(dm_dev_minor));
			long look = (long) d;
			if (look > 0) {
				log_debug_devs("dev_is_mpath_component %s holder %s %u:%u already checked as %sbeing mpath.",
						dev_name(dev), holder_name, dm_dev_major, dm_dev_minor, (look > 1) ? "" : "not ");

				is_mpath_component = (look == 2);
				goto out;
			}

			/* no saved result for dm_dev_minor, so check the uuid for it */
		}

		/* Check whether holder's UUID uses MPATH prefix */
		/* TODO: reuse/merge with dev_has_mpath_uuid() as this function also recognizes kpartx partition */
		if (devno_dm_uuid(cmd, dm_dev_major, dm_dev_minor, uuid, sizeof(uuid)) &&
		    !strncmp(uuid, MPATH_PREFIX, sizeof(MPATH_PREFIX) - 1)) {
			log_debug_devs("dev_is_mpath_component %s holder %s %u:%u ignore mpath component",
					dev_name(dev), holder_name, dm_dev_major, dm_dev_minor);

			/* For future checks, save that the dm minor refers to mpath ("2" == is mpath) */
			if (_minor_hash_tab)
				(void) dm_hash_insert_binary(_minor_hash_tab, &dm_dev_minor, sizeof(dm_dev_minor), (void*)2);

			is_mpath_component = 1;
			goto out;
		}

		/* For future checks, save that the dm minor does not refer to mpath ("1" == is not mpath) */
		if (_minor_hash_tab)
			(void) dm_hash_insert_binary(_minor_hash_tab, &dm_dev_minor, sizeof(dm_dev_minor), (void*)1);
	}

 out:
	if (closedir(dr))
		log_sys_debug("closedir", holders_path);

	if (is_mpath_component)
		*mpath_devno = MKDEV(dm_dev_major, dm_dev_minor);
	return is_mpath_component;
}

static int _dev_in_wwid_file(struct cmd_context *cmd, struct device *dev,
			     int primary_result, dev_t primary_dev)
{
	char idbuf[DEV_WWID_SIZE] = { 0 };
	struct dev_wwid *dw;
	char *wwid, *full_wwid;

	if (!_wwid_hash_tab)
		return 0;

	/*
	 * Check the primary device, not the partition.
	 */
	if (primary_result == 2) {
		if (!(dev = dev_cache_get_by_devt(cmd, primary_dev))) {
			log_debug("dev_is_mpath_component %s no primary dev", dev_name(dev));
			return 0;
		}
	}

	/*
	 * sysfs wwid uses format: naa.<value>, eui.<value>, t10.<value>.
	 * multipath wwids file uses format: 3<value>, 2<value>, 1<value>.
	 *
	 * We omit the type prefix before looking up.  The multipath/wwids
	 * values in the wwid_hash_tab have the initial character removed.
	 *
	 * There's no type prefix for "scsi name string" type 8 ids.
	 *
	 * First try looking up any wwids that have already been read.
	 */
lookup:
	dm_list_iterate_items(dw, &dev->wwids) {
		if (dw->type == 1 || dw->type == 2 || dw->type == 3)
			wwid = &dw->id[4];
		else
			wwid = dw->id;

		if (dm_hash_lookup_binary(_wwid_hash_tab, wwid, strlen(wwid))) {
			full_wwid = dw->id;
			goto found;
		}
	}

	/*
	 * The id from sysfs wwid may not be the id used by multipath,
	 * or a device may not have a vpd_pg83 file (e.g. nvme).
	 */

	if (!(dev->flags & DEV_ADDED_VPD_WWIDS) && dev_read_vpd_wwids(cmd, dev))
		goto lookup;

	if (!(dev->flags & DEV_ADDED_SYS_WWID) && dev_read_sys_wwid(cmd, dev, idbuf, sizeof(idbuf), &dw)) {
		if (dw->type == 1 || dw->type == 2 || dw->type == 3)
			wwid = &dw->id[4];
		else
			wwid = dw->id;

		if (dm_hash_lookup_binary(_wwid_hash_tab, wwid, strlen(wwid))) {
			full_wwid = dw->id;
			goto found;
		}
	}

	return 0;

 found:
	log_debug_devs("dev_is_mpath_component %s %s in wwids file", dev_name(dev), full_wwid);
	return 1;
}

int dev_is_mpath_component(struct cmd_context *cmd, struct device *dev, dev_t *holder_devno)
{
	struct dev_types *dt = cmd->dev_types;
	int primary_result;
	dev_t primary_dev;

	/*
	 * multipath only uses SCSI or NVME devices
	 */
	if (!major_is_scsi_device(dt, MAJOR(dev->dev)) && !dev_is_nvme(dt, dev))
		return 0;

	/*
	 * primary_result 2: dev is a partition, primary_dev is the whole device
	 * primary_result 1: dev is a whole device
	 */
	if (!(primary_result = dev_get_primary_dev(dt, dev, &primary_dev)))
		return_0;

	if (_dev_is_mpath_component_sysfs(cmd, dev, primary_result, primary_dev, holder_devno) == 1)
		goto found;

	if (_dev_in_wwid_file(cmd, dev, primary_result, primary_dev))
		goto found;

	if (external_device_info_source() == DEV_EXT_UDEV) {
		if (_dev_is_mpath_component_udev(dev) == 1)
			goto found;
	}

	/*
	 * TODO: save the result of this function in dev->flags and use those
	 * flags on repeated calls to avoid repeating the work multiple times
	 * for the same device when there are partitions on the device.
	 */

	return 0;
found:
	return 1;
}

const char *dev_mpath_component_wwid(struct cmd_context *cmd, struct device *dev)
{
	char slaves_path[PATH_MAX];
	char wwid_path[PATH_MAX];
	char sysbuf[PATH_MAX] = { 0 };
	char *slave_name;
	const char *wwid = NULL;
	DIR *dr;
	struct dirent *de;

	/* /sys/dev/block/253:7/slaves/sda/device/wwid */

	if (dm_snprintf(slaves_path, sizeof(slaves_path), "%sdev/block/%u:%u/slaves",
			dm_sysfs_dir(), MAJOR(dev->dev), MINOR(dev->dev)) < 0) {
		log_warn("Sysfs path to check mpath components is too long.");
		return NULL;
	}

	/* Get wwid from first component */

	if (!(dr = opendir(slaves_path))) {
		if (errno == ENOTDIR)
			log_warn("WARNING: Path %s is not a directory.", slaves_path);
		else if (errno != ENOENT)
			log_sys_debug("opendir", slaves_path);
		return NULL;
	}

	while ((de = readdir(dr))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		/* slave_name "sda" */
		slave_name = de->d_name;

		/* read /sys/block/sda/device/wwid */

		if (dm_snprintf(wwid_path, sizeof(wwid_path), "%sblock/%s/device/wwid",
				dm_sysfs_dir(), slave_name) < 0) {
			log_warn("Failed to create sysfs wwid path for %s", slave_name);
			continue;
		}

		if (!get_sysfs_value(wwid_path, sysbuf, sizeof(sysbuf), 0))
			stack;

		if (!sysbuf[0])
			continue;

		if (strstr(sysbuf, "scsi_debug")) {
			unsigned i;
			for (i = 0; i < strlen(sysbuf); i++) {
				if (sysbuf[i] == ' ')
					sysbuf[i] = '_';
			}
		}

		if ((wwid = dm_pool_strdup(cmd->mem, sysbuf)))
			break;
	}
	if (closedir(dr))
		log_sys_debug("closedir", slaves_path);

	return wwid;
}
