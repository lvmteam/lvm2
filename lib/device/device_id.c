/*
 * Copyright (C) 2020 Red Hat, Inc. All rights reserved.
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
#include "lib/commands/toolcontext.h"
#include "lib/device/device.h"
#include "lib/device/device_id.h"
#include "lib/device/dev-type.h"
#include "lib/label/label.h"
#include "lib/metadata/metadata.h"
#include "lib/format_text/layout.h"
#include "lib/cache/lvmcache.h"
#include "lib/datastruct/str_list.h"
#include "lib/metadata/metadata-exported.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/sysmacros.h>

#define DEVICES_FILE_MAJOR 1
#define DEVICES_FILE_MINOR 1
#define VERSION_LINE_MAX 256

static int _devices_fd = -1;
static int _using_devices_file;
static int _devices_file_locked;
static char _devices_lockfile[PATH_MAX];
static char _devices_file_systemid[PATH_MAX];
static char _devices_file_version[VERSION_LINE_MAX];
static const char *_searched_file = DEFAULT_RUN_DIR "/searched_devnames";

char *devices_file_version(void)
{
	return _devices_file_version;
}

/*
 * cmd->devicesfile is set when using a non-system devices file,
 * and at least for now, the searched_devnames optimization
 * only applies to the system devices file.
 */

static void _touch_searched_devnames(struct cmd_context *cmd)
{
	FILE *fp;

	if (cmd->devicesfile)
		return;

	if (!(fp = fopen(_searched_file, "w")))
		return;
	if (fclose(fp))
		stack;
}

void unlink_searched_devnames(struct cmd_context *cmd)
{
	if (cmd->devicesfile)
		return;

	if (unlink(_searched_file))
		log_debug("unlink %s errno %d", _searched_file, errno);
	else
		log_debug("unlink %s", _searched_file);
}

static int _searched_devnames_exists(struct cmd_context *cmd)
{
	struct stat buf;

	if (cmd->devicesfile)
		return 0;

	if (!stat(_searched_file, &buf))
		return 1;

	if (errno != ENOENT)
		log_debug("stat %s errno %d", _searched_file, errno);

	return 0;
}

/*
 * How the devices file and device IDs are used by an ordinary command:
 *
 * 1. device_ids_read() reads the devices file, and adds a 'struct dev_use'
 *    to cmd->use_devices for each entry.  These are the devices lvm
 *    can use, but we do not yet know which devnames they correspond to.
 * 2. dev_cache_scan() gets a list of all devices (devnames) on the system,
 *    and adds a 'struct device' to dev-cache for each.
 * 3. device_ids_match() matches du entries from the devices file
 *    with devices from dev-cache.  With this complete, we know the
 *    devnames to use for each of the entries in the devices file.
 * 4. label_scan (or equivalent) iterates through all devices in
 *    dev-cache, checks each one with filters, which excludes many,
 *    and reads lvm headers and metadata from the devs that pass the
 *    filters.  lvmcache is populated with summary info about each PV
 *    during this phase.
 * 5. device_ids_validate() checks if the PVIDs saved in the devices
 *    file are correct based on the PVIDs read from disk in the 
 *    previous step.  If not it updates the devices file.
 *
 * cmd->use_devices reflect the entries in the devices file.
 * When reading the devices file, a 'du' struct is added to use_devices
 * for each entry.
 * When adding devices to the devices file, a new du struct is added
 * to use_devices, and then a new file entry is written for each du.
 *
 * After reading the devices file, we want to match each du from
 * the file to an actual device on the system.  We look at struct device's
 * in dev-cache to find one that matches each du, based on the device_id.
 * When a match is made, du->dev is set, and DEV_MATCHED_USE_ID is set
 * in the dev.
 *
 * After the use_devices entries are matched to system devices,
 * label_scan can be called to filter and scan devices.  After
 * label_scan, device_ids_validate() is called to check if the
 * PVID read from each device matches the PVID recorded in the
 * devices file for the device.
 *
 * A device can have multiple device IDs, e.g. a dev could have
 * both a wwid and a serial number, but only one of these IDs is
 * used as the device ID in the devices file, e.g. the wwid is
 * preferred so that would be used in the devices file.
 * Each of the different types of device IDs can be saved in
 * dev->ids list (struct dev_id).  So, one dev may have multiple
 * entries in dev->ids, e.g. one for wwid and one for serial.
 * The dev_id struct that is actually being used for the device
 * is set in dev->id.
 * The reason for saving multiple IDs in dev->ids is because
 * the process of matching devs to devices file entries can
 * involve repeatedly checking other dev_id types for a given
 * device, so we save each type as it is read to avoid rereading
 * the same id type many times.
 */

void free_du(struct dev_use *du)
{
	free(du->idname);
	free(du->devname);
	free(du->pvid);
	free(du);
}

void free_dus(struct dm_list *dus)
{
	struct dev_use *du, *safe;

	dm_list_iterate_items_safe(du, safe, dus) {
		dm_list_del(&du->list);
		free_du(du);
	}
}

void free_did(struct dev_id *id)
{
	if (strlen(id->idname))
		free(id->idname); /* idname = "" when id type doesn't exist */
	free(id);
}

void free_dids(struct dm_list *ids)
{
	struct dev_id *id, *safe;

	dm_list_iterate_items_safe(id, safe, ids) {
		dm_list_del(&id->list);
		free_did(id);
	}
}

/* More than one _ in a row is replaced with one _ */
static void _reduce_repeating_underscores(char *buf, size_t bufsize)
{
	char *tmpbuf;
	unsigned us = 0, i, j = 0;

	if (!(tmpbuf = strndup(buf, bufsize-1)))
		return;

	memset(buf, 0, bufsize);

	for (i = 0; i < strlen(tmpbuf); i++) {
		if (tmpbuf[i] == '_')
			us++;
		else
			us = 0;

		if (us == 1)
			buf[j++] = '_';
		else if (us > 1)
			continue;
		else
			buf[j++] = tmpbuf[i];

		if (j == bufsize)
			break;
	}
	buf[bufsize-1] = '\0';
	free(tmpbuf);
}

static void _remove_leading_underscores(char *buf, size_t bufsize)
{
	char *tmpbuf;
	unsigned i, j = 0;

	if (buf[0] != '_')
		return;

	if (!(tmpbuf = strndup(buf, bufsize-1)))
		return;

	memset(buf, 0, bufsize);

	for (i = 0; i < strlen(tmpbuf); i++) {
		if (!j && tmpbuf[i] == '_')
			continue;
		buf[j++] = tmpbuf[i];

		if (j == bufsize)
			break;
	}
	free(tmpbuf);
}

static void _remove_trailing_underscores(char *buf, int bufsize)
{
	char *end;

	end = buf + strlen(buf) - 1;
	while ((end > buf) && (*end == '_'))
		end--;
	end[1] = '\0';
}

static int _read_sys_block(struct cmd_context *cmd, struct device *dev,
			   const char *suffix, char *sysbuf, int sysbufsize,
			   int binary, int *retlen)
{
	char path[PATH_MAX];
	const char *sysfs_dir;
	dev_t devt = dev->dev;
	dev_t prim = 0;
	int ret;

	sysfs_dir = cmd->device_id_sysfs_dir ?: dm_sysfs_dir();
 retry:
	if (dm_snprintf(path, sizeof(path), "%sdev/block/%d:%d/%s",
			sysfs_dir, (int)MAJOR(devt), (int)MINOR(devt), suffix) < 0) {
		log_error("Failed to create sysfs path for %s", dev_name(dev));
		return 0;
	}

	if (binary) {
		ret = get_sysfs_binary(path, sysbuf, sysbufsize, retlen);
		if (ret && !*retlen)
			ret = 0;
	} else {
		ret = get_sysfs_value(path, sysbuf, sysbufsize, 0);
		if (ret && !sysbuf[0])
			ret = 0;
	}

	if (ret) {
		sysbuf[sysbufsize - 1] = '\0';
		return 1;
	}

	if (prim)
		goto fail;

	/* in case it failed because dev is a partition... */

	ret = dev_get_primary_dev(cmd->dev_types, dev, &prim);
	if (ret == 2) {
		devt = prim;
		goto retry;
	}

 fail:
	return 0;
}

int read_sys_block(struct cmd_context *cmd, struct device *dev,
		   const char *suffix, char *sysbuf, int sysbufsize)
{
	return _read_sys_block(cmd, dev, suffix, sysbuf, sysbufsize, 0, NULL);
}

int read_sys_block_binary(struct cmd_context *cmd, struct device *dev,
			  const char *suffix, char *sysbuf, int sysbufsize,
			  int *retlen)
{
	return _read_sys_block(cmd, dev, suffix, sysbuf, sysbufsize, 1, retlen);
}

static int _dm_uuid_has_prefix(char *sysbuf, const char *prefix)
{
	if (!strncmp(sysbuf, prefix, strlen(prefix)))
		return 1;

	/*
	 * If it's a kpartx partitioned dm device the dm uuid will
	 * be part%d-<prefix>...  e.g. part1-mpath-abc...
	 * Check for the prefix after the part%-
	 */
	if (!strncmp(sysbuf, "part", 4)) {
		const char *dash = strchr(sysbuf, '-');

		if (!dash)
			return 0;

		if (!strncmp(dash + 1, prefix, strlen(prefix)))
			return 1;
	}
	return 0;
}

/* the dm uuid uses the wwid of the underlying dev */
int dev_has_mpath_uuid(struct cmd_context *cmd, struct device *dev, const char **idname_out)
{
	char sysbuf[PATH_MAX] = { 0 };
	const char *idname;

	if (!read_sys_block(cmd, dev, "dm/uuid", sysbuf, sizeof(sysbuf)))
		return 0;

	if (!_dm_uuid_has_prefix(sysbuf, "mpath-"))
		return 0;

	if (!idname_out)
		return 1;
	if (!(idname = strdup(sysbuf)))
		return_0;
	*idname_out = idname;
	return 1;
}

static int _dev_has_crypt_uuid(struct cmd_context *cmd, struct device *dev, const char **idname_out)
{
	char sysbuf[PATH_MAX] = { 0 };
	const char *idname;

	if (!read_sys_block(cmd, dev, "dm/uuid", sysbuf, sizeof(sysbuf)))
		return 0;

	if (!_dm_uuid_has_prefix(sysbuf, "CRYPT-"))
		return 0;

	if (!idname_out)
		return 1;
	if (!(idname = strdup(sysbuf)))
		return_0;
	*idname_out = idname;
	return 1;
}

static int _dev_has_lvmlv_uuid(struct cmd_context *cmd, struct device *dev, const char **idname_out)
{
	char sysbuf[PATH_MAX] = { 0 };
	const char *idname;

	if (!read_sys_block(cmd, dev, "dm/uuid", sysbuf, sizeof(sysbuf)))
		return 0;

	if (!_dm_uuid_has_prefix(sysbuf, "LVM-"))
		return 0;

	if (!idname_out)
		return 1;
	if (!(idname = strdup(sysbuf)))
		return_0;
	*idname_out = idname;
	return 1;
}

/*
 * The numbers 1,2,3 for NAA,EUI,T10 are part of the standard
 * and are used in the vpd data.
 */
static int _wwid_type_num(char *id)
{
	if (!strncmp(id, "naa.", 4))
		return 3;
	else if (!strncmp(id, "eui.", 4))
		return 2;
	else if (!strncmp(id, "t10.", 4))
		return 1;
	else
		return -1;
}

int wwid_type_to_idtype(int wwid_type)
{
	switch (wwid_type) {
	case 3: return DEV_ID_TYPE_WWID_NAA;
	case 2: return DEV_ID_TYPE_WWID_EUI;
	case 1: return DEV_ID_TYPE_WWID_T10;
	default: return -1;
	}
}

int idtype_to_wwid_type(int idtype)
{
	switch (idtype) {
	case DEV_ID_TYPE_WWID_NAA: return 3;
	case DEV_ID_TYPE_WWID_EUI: return 2;
	case DEV_ID_TYPE_WWID_T10: return 1;
	default: return -1;
	}
}

void free_wwids(struct dm_list *ids)
{
	struct dev_wwid *dw, *safe;

	dm_list_iterate_items_safe(dw, safe, ids) {
		dm_list_del(&dw->list);
		free(dw);
	}
}

/*
 * wwid type 8 "scsi name string" (which includes "iqn" names) is
 * included in vpd_pg83, but we currently do not use these for
 * device ids (maybe in the future.)
 * They can still be checked by dev-mpath when looking for a device
 * in /etc/multipath/wwids.
 */

struct dev_wwid *dev_add_wwid(char *id, int id_type, struct dm_list *ids)
{
	struct dev_wwid *dw;
	int len;

	if (!id_type) {
		id_type = _wwid_type_num(id);
		if (id_type == -1)
			log_debug("unknown wwid type %s", id);
	}

	if (!(dw = zalloc(sizeof(struct dev_wwid))))
		return NULL;
	len = strlen(id);
	if (len >= DEV_WWID_SIZE)
		len = DEV_WWID_SIZE - 1;
	memcpy(dw->id, id, len);
	dw->type = id_type;
	dm_list_add(ids, &dw->list);
	return dw;
}

#define VPD_SIZE 4096

int dev_read_vpd_wwids(struct cmd_context *cmd, struct device *dev)
{
	char vpd_data[VPD_SIZE] = { 0 };
	int vpd_datalen = 0;

	dev->flags |= DEV_ADDED_VPD_WWIDS;

	if (!read_sys_block_binary(cmd, dev, "device/vpd_pg83", (char *)vpd_data, VPD_SIZE, &vpd_datalen))
		return 0;
	if (!vpd_datalen)
		return 0;

	/* adds dev_wwid entry to dev->wwids for each id in vpd data */
	parse_vpd_ids((const unsigned char *)vpd_data, vpd_datalen, &dev->wwids);
	return 1;
}

int dev_read_sys_wwid(struct cmd_context *cmd, struct device *dev,
		      char *outbuf, int outbufsize, struct dev_wwid **dw_out)
{
	char buf[DEV_WWID_SIZE] = { 0 };
	struct dev_wwid *dw;
	int is_t10 = 0;
	int ret;
	unsigned i;

	dev->flags |= DEV_ADDED_SYS_WWID;

	ret = read_sys_block(cmd, dev, "device/wwid", buf, sizeof(buf));
	if (!ret || !buf[0]) {
		/* the wwid file is not under device for nvme devs */
		ret = read_sys_block(cmd, dev, "wwid", buf, sizeof(buf));
	}
	if (!ret || !buf[0])
		return 0;

	for (i = 0; i < sizeof(buf) - 4; i++) {
		if (buf[i] == ' ')
			continue;
		if (!strncmp(&buf[i], "t10", 3))
			is_t10 = 1;
		break;
	}

	/*
	 * Remove leading and trailing spaces.
	 * Replace internal spaces with underscores.
	 * t10 wwids have multiple sequential spaces
	 * replaced by a single underscore.
	 */
	if (is_t10)
		format_t10_id((const unsigned char *)buf, sizeof(buf), (unsigned char *)outbuf, outbufsize);
	else
		format_general_id((const char *)buf, sizeof(buf), (unsigned char *)outbuf, outbufsize);

	/* Note, if wwids are also read from vpd, this same wwid will be added again. */

	if (!(dw = dev_add_wwid(buf, 0, &dev->wwids)))
		return_0;
	if (dw_out)
		*dw_out = dw;
	return 1;
}

static int _dev_read_sys_serial(struct cmd_context *cmd, struct device *dev,
				char *outbuf, int outbufsize)
{
	char buf[VPD_SIZE] = { 0 };
	const char *devname;
	int vpd_datalen = 0;

	/*
	 * Look in
	 * /sys/dev/block/major:minor/device/serial
	 * /sys/dev/block/major:minor/device/vpd_pg80
	 * /sys/class/block/vda/serial
	 * (Only virtio disks /dev/vdx are known to use /sys/class/block/vdx/serial.)
	 */

	read_sys_block(cmd, dev, "device/serial", buf, sizeof(buf));
	if (buf[0]) {
		format_general_id((const char *)buf, sizeof(buf), (unsigned char *)outbuf, outbufsize);
		if (outbuf[0])
			return 1;
	}

	if (read_sys_block_binary(cmd, dev, "device/vpd_pg80", buf, VPD_SIZE, &vpd_datalen) && vpd_datalen) {
		parse_vpd_serial((const unsigned char *)buf, outbuf, outbufsize);
		if (outbuf[0])
			return 1;
	}
	
	devname = dev_name(dev);
	if (!strncmp(devname, "/dev/vd", 7)) {
		char path[PATH_MAX];
		char vdx[8] = { 0 };
		const char *sysfs_dir;
		const char *base;
		unsigned i, j = 0;
		int ret;

		/* /dev/vda to vda */
		base = basename(devname);

		/* vda1 to vda */
		for (i = 0; i < strlen(base); i++) {
			if (isdigit(base[i]))
				break;
			vdx[j] = base[i];
			j++;
		}

		sysfs_dir = cmd->device_id_sysfs_dir ?: dm_sysfs_dir();

		if (dm_snprintf(path, sizeof(path), "%s/class/block/%s/serial", sysfs_dir, vdx) < 0)
			return 0;

		ret = get_sysfs_value(path, buf, sizeof(buf), 0);
		if (ret && !buf[0])
			ret = 0;
		if (ret) {
			format_general_id((const char *)buf, sizeof(buf), (unsigned char *)outbuf, outbufsize);
			if (buf[0])
				return 1;
		}
	}

	return 0;
}

const char *device_id_system_read(struct cmd_context *cmd, struct device *dev, uint16_t idtype)
{
	char sysbuf[PATH_MAX] = { 0 };
	char sysbuf2[PATH_MAX] = { 0 };
	const char *idname = NULL;
	struct dev_wwid *dw;
	unsigned i;

	if (idtype == DEV_ID_TYPE_SYS_WWID) {
		dev_read_sys_wwid(cmd, dev, sysbuf, sizeof(sysbuf), NULL);

		/* FIXME: enable these QEMU t10 wwids */

		/* qemu wwid begins "t10.ATA     QEMU HARDDISK ..." */
		if (strstr(sysbuf, "QEMU HARDDISK"))
			sysbuf[0] = '\0';
	}

	else if (idtype == DEV_ID_TYPE_SYS_SERIAL) {
		_dev_read_sys_serial(cmd, dev, sysbuf, sizeof(sysbuf));
	}

	else if (idtype == DEV_ID_TYPE_MPATH_UUID) {
		read_sys_block(cmd, dev, "dm/uuid", sysbuf, sizeof(sysbuf));
		/* if (strncmp(sysbuf, "mpath", 5)) sysbuf[0] = '\0'; */
	}

	else if (idtype == DEV_ID_TYPE_CRYPT_UUID) {
		read_sys_block(cmd, dev, "dm/uuid", sysbuf, sizeof(sysbuf));
		/* if (strncmp(sysbuf, "CRYPT", 5)) sysbuf[0] = '\0'; */
	}

	else if (idtype == DEV_ID_TYPE_LVMLV_UUID) {
		read_sys_block(cmd, dev, "dm/uuid", sysbuf, sizeof(sysbuf));
		/* if (strncmp(sysbuf, "LVM", 3)) sysbuf[0] = '\0'; */
	}

	else if (idtype == DEV_ID_TYPE_MD_UUID) {
		read_sys_block(cmd, dev, "md/uuid", sysbuf, sizeof(sysbuf));
	}

	else if (idtype == DEV_ID_TYPE_LOOP_FILE) {
		read_sys_block(cmd, dev, "loop/backing_file", sysbuf, sizeof(sysbuf));
		/* if backing file is deleted, fall back to devname */
		if (strstr(sysbuf, "(deleted)"))
			sysbuf[0] = '\0';
	}

	else if (idtype == DEV_ID_TYPE_DEVNAME) {
		if (dm_list_empty(&dev->aliases))
			goto_bad;
		if (!(idname = strdup(dev_name(dev))))
			goto_bad;
		return idname;
	}

	else if (idtype == DEV_ID_TYPE_WWID_NAA ||
		 idtype == DEV_ID_TYPE_WWID_EUI ||
		 idtype == DEV_ID_TYPE_WWID_T10) {
		if (!(dev->flags & DEV_ADDED_VPD_WWIDS))
			dev_read_vpd_wwids(cmd, dev);
		dm_list_iterate_items(dw, &dev->wwids) {
			if (idtype_to_wwid_type(idtype) == dw->type)
				return strdup(dw->id);
		}
		return NULL;
	}

	/*
	 * Replace all spaces, quotes, control chars with underscores.
	 * sys_wwid, sys_serial, and wwid_* have already been handled,
	 * and with slightly different replacement (see format_t10_id,
	 * format_general_id.)
	 */
	if ((idtype != DEV_ID_TYPE_SYS_WWID) &&
	    (idtype != DEV_ID_TYPE_SYS_SERIAL) &&
	    (idtype != DEV_ID_TYPE_WWID_NAA) &&
	    (idtype != DEV_ID_TYPE_WWID_EUI) &&
	    (idtype != DEV_ID_TYPE_WWID_T10)) {
		for (i = 0; i < strlen(sysbuf); i++) {
			if ((sysbuf[i] == '"') ||
			    isblank(sysbuf[i]) ||
			    isspace(sysbuf[i]) ||
			    iscntrl(sysbuf[i]))
				sysbuf[i] = '_';
		}
	}

	/*
	 * Reduce actual leading and trailing underscores for sys_wwid
	 * and sys_serial, since underscores were previously used as
	 * replacements for leading/trailing spaces which are now ignored.
	 * Also reduce any actual repeated underscores in t10 wwid since
	 * multiple repeated spaces were also once replaced by underscores.
	 */
	if ((idtype == DEV_ID_TYPE_SYS_WWID) ||
	    (idtype == DEV_ID_TYPE_SYS_SERIAL)) {
		memcpy(sysbuf2, sysbuf, sizeof(sysbuf2));
		_remove_leading_underscores(sysbuf2, sizeof(sysbuf2));
		_remove_trailing_underscores(sysbuf2, sizeof(sysbuf2));
		if (idtype == DEV_ID_TYPE_SYS_WWID && !strncmp(sysbuf2, "t10", 3) && strstr(sysbuf2, "__"))
			_reduce_repeating_underscores(sysbuf2, sizeof(sysbuf2));
		if (memcmp(sysbuf, sysbuf2, sizeof(sysbuf)))
			log_debug("device_id_system_read reduced underscores %s to %s", sysbuf, sysbuf2);
		memcpy(sysbuf, sysbuf2, sizeof(sysbuf));
	}

	if (!sysbuf[0])
		goto bad;

	if (!(idname = strdup(sysbuf)))
		goto_bad;

	return idname;
 bad:
	return NULL;
}

/*
 * Check if this dev would use a stable idtype or if it
 * would use DEV_ID_TYPE_DEVNAME.
 */
static int _dev_has_stable_id(struct cmd_context *cmd, struct device *dev)
{
	char sysbuf[PATH_MAX] = { 0 };
	struct dev_id *id;
	const char *idname;

	/*
	 * An idtype other than DEVNAME is stable, i.e. it doesn't change after
	 * reboot or device reattach.
	 * An id on dev->ids with idtype set and !idname means that idtype does
	 * not exist for the dev.  (Optimization to avoid repeated negative
	 * system_read.)
	 */
	dm_list_iterate_items(id, &dev->ids) {
		if ((id->idtype != DEV_ID_TYPE_DEVNAME) && id->idname)
			return 1;
	}

	/*
	 * Use device_id_system_read() instead of read_sys_block() when
	 * system_read ignores some values from sysfs.
	 */

	if ((idname = device_id_system_read(cmd, dev, DEV_ID_TYPE_SYS_WWID))) {
		free((void*)idname);
		return 1;
	}

	if ((idname = device_id_system_read(cmd, dev, DEV_ID_TYPE_SYS_SERIAL))) {
		free((void*)idname);
		return 1;
	}

	if ((MAJOR(dev->dev) == cmd->dev_types->loop_major) &&
	    (idname = device_id_system_read(cmd, dev, DEV_ID_TYPE_LOOP_FILE))) {
		free((void*)idname);
		return 1;
	}

	if ((MAJOR(dev->dev) == cmd->dev_types->device_mapper_major)) {
		if (!read_sys_block(cmd, dev, "dm/uuid", sysbuf, sizeof(sysbuf)))
			goto_out;

		if (_dm_uuid_has_prefix(sysbuf, "mpath-"))
			return 1;
		if (_dm_uuid_has_prefix(sysbuf, "CRYPT-"))
			return 1;
		if (_dm_uuid_has_prefix(sysbuf, "LVM-"))
			return 1;
	}

	if ((MAJOR(dev->dev) == cmd->dev_types->md_major) &&
	    read_sys_block(cmd, dev, "md/uuid", sysbuf, sizeof(sysbuf)))
		return 1;

	if (!(dev->flags & DEV_ADDED_VPD_WWIDS))
		dev_read_vpd_wwids(cmd, dev);
	if (!dm_list_empty(&dev->wwids))
		return 1;

 out:
	/* DEV_ID_TYPE_DEVNAME would be used for this dev. */
	return 0;
}

const char *idtype_to_str(uint16_t idtype)
{
	if (idtype == DEV_ID_TYPE_SYS_WWID)
		return "sys_wwid";
	if (idtype == DEV_ID_TYPE_SYS_SERIAL)
		return "sys_serial";
	if (idtype == DEV_ID_TYPE_DEVNAME)
		return "devname";
	if (idtype == DEV_ID_TYPE_MPATH_UUID)
		return "mpath_uuid";
	if (idtype == DEV_ID_TYPE_CRYPT_UUID)
		return "crypt_uuid";
	if (idtype == DEV_ID_TYPE_LVMLV_UUID)
		return "lvmlv_uuid";
	if (idtype == DEV_ID_TYPE_MD_UUID)
		return "md_uuid";
	if (idtype == DEV_ID_TYPE_LOOP_FILE)
		return "loop_file";
	if (idtype == DEV_ID_TYPE_WWID_NAA)
		return "wwid_naa";
	if (idtype == DEV_ID_TYPE_WWID_EUI)
		return "wwid_eui";
	if (idtype == DEV_ID_TYPE_WWID_T10)
		return "wwid_t10";
	return "unknown";
}

uint16_t idtype_from_str(const char *str)
{
	if (!strcmp(str, "sys_wwid"))
		return DEV_ID_TYPE_SYS_WWID;
	if (!strcmp(str, "sys_serial"))
		return DEV_ID_TYPE_SYS_SERIAL;
	if (!strcmp(str, "devname"))
		return DEV_ID_TYPE_DEVNAME;
	if (!strcmp(str, "mpath_uuid"))
		return DEV_ID_TYPE_MPATH_UUID;
	if (!strcmp(str, "crypt_uuid"))
		return DEV_ID_TYPE_CRYPT_UUID;
	if (!strcmp(str, "lvmlv_uuid"))
		return DEV_ID_TYPE_LVMLV_UUID;
	if (!strcmp(str, "md_uuid"))
		return DEV_ID_TYPE_MD_UUID;
	if (!strcmp(str, "loop_file"))
		return DEV_ID_TYPE_LOOP_FILE;
	if (!strcmp(str, "wwid_naa"))
		return DEV_ID_TYPE_WWID_NAA;
	if (!strcmp(str, "wwid_eui"))
		return DEV_ID_TYPE_WWID_EUI;
	if (!strcmp(str, "wwid_t10"))
		return DEV_ID_TYPE_WWID_T10;
	return 0;
}

const char *dev_idtype_for_metadata(struct cmd_context *cmd, struct device *dev)
{
	const char *str;

	if (!cmd->enable_devices_file)
		return NULL;

	if (!dev || !dev->id || !dev->id->idtype || (dev->id->idtype == DEV_ID_TYPE_DEVNAME))
		return NULL;

	str = idtype_to_str(dev->id->idtype);
	if (!strcmp(str, "unknown"))
		return NULL;

	return str;
}

const char *dev_idname_for_metadata(struct cmd_context *cmd, struct device *dev)
{
	if (!cmd->enable_devices_file)
		return NULL;

	if (!dev || !dev->id || !dev->id->idtype || (dev->id->idtype == DEV_ID_TYPE_DEVNAME))
		return NULL;

	return dev->id->idname;
}

static const char *_dev_idname(struct device *dev, uint16_t idtype)
{
	struct dev_id *id;

	dm_list_iterate_items(id, &dev->ids) {
		if (id->idtype != idtype)
			continue;
		if (!id->idname)
			continue;
		return id->idname;
	}
	return NULL;
}

static int _dev_has_id(struct device *dev, uint16_t idtype, const char *idname)
{
	struct dev_id *id;

	dm_list_iterate_items(id, &dev->ids) {
		if (id->idtype != idtype)
			continue;
		if (!id->idname)
			continue;
		if (!strcmp(idname, id->idname))
			return 1;
	}
	return 0;
}

static void _copy_idline_str(char *src, char *dst, int len)
{
	char *s, *d = dst;

	memset(dst, 0, len);

	if (!(s = strchr(src, '=')))
		return;
	s++;
	while ((*s == ' ') && (s < src + len))
		s++;
	while ((*s != ' ') && (*s != '\0') && (*s != '\n') && (s < src + len)) {
		*d = *s;
		s++;
		d++;
	}

	dst[len-1] = '\0';
}

int device_ids_read(struct cmd_context *cmd)
{
	char line[PATH_MAX];
	char buf[PATH_MAX];
	char *idtype, *idname, *devname, *pvid, *part;
	struct dev_use *du;
	FILE *fp;
	int line_error;
	int ret = 1;

	if (!cmd->enable_devices_file)
		return 1;

	/*
	 * The use_devices list should rarely if ever be non-empty at this
	 * point, it means device_ids_read has been called twice.
	 * If we wanted to redo reading the file, we'd need to
	 * free_dus(&cmd->use_devices) and clear the MATCHED_USE_ID flag in all
	 * dev->flags.
	 */
	if (!dm_list_empty(&cmd->use_devices)) {
		log_debug("device_ids_read already done");
		return 1;
	}

	log_debug("device_ids_read %s", cmd->devices_file_path);

	if (!(fp = fopen(cmd->devices_file_path, "r"))) {
		log_warn("Cannot open devices file to read.");
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#')
			continue;

		if (!strncmp(line, "SYSTEMID", 8)) {
			_copy_idline_str(line, _devices_file_systemid, sizeof(_devices_file_systemid));
			log_debug("read devices file systemid %s", _devices_file_systemid);
			if ((!cmd->system_id && _devices_file_systemid[0]) ||
			    (cmd->system_id && strcmp(cmd->system_id, _devices_file_systemid))) {
				log_warn("WARNING: devices file has unmatching system id %s vs local %s.",
					  _devices_file_systemid[0] ? _devices_file_systemid : "none", cmd->system_id ?: "none");
			}
			continue;
		}
		if (!strncmp(line, "VERSION", 7)) {
			_copy_idline_str(line, _devices_file_version, sizeof(_devices_file_version));
			log_debug("read devices file version %s", _devices_file_version);
			continue;
		}

		idtype = strstr(line, "IDTYPE");
		idname = strstr(line, "IDNAME");
		devname = strstr(line, "DEVNAME");
		pvid = strstr(line, "PVID");
		part = strstr(line, "PART");
		line_error = 0;

		/* These two are the minimum required. */
		if (!idtype || !idname)
			continue;

		if (!(du = zalloc(sizeof(struct dev_use)))) {
			log_warn("WARNING: failed to process devices file entry.");
			continue;
		}

		_copy_idline_str(idtype, buf, PATH_MAX);
		if (buf[0])
			du->idtype = idtype_from_str(buf);

		_copy_idline_str(idname, buf, PATH_MAX);
		if (buf[0] && (buf[0] != '.')) {
			if (!(du->idname = strdup(buf)))
				line_error = 1;
		}

		if (devname) {
			_copy_idline_str(devname, buf, PATH_MAX);
			if (buf[0] && (buf[0] != '.')) {
				if (!(du->devname = strdup(buf)))
					line_error = 1;
			}
		}

		if (pvid) {
			_copy_idline_str(pvid, buf, PATH_MAX);
			if (buf[0] && (buf[0] != '.')) {
				if (!(du->pvid = strdup(buf)))
					line_error = 1;
			}
		}

		if (part) {
			_copy_idline_str(part, buf, PATH_MAX);
			if (buf[0] && (buf[0] != '.'))
				du->part = atoi(buf);
		}

		if (line_error) {
			log_warn("WARNING: failed to process devices file entry.");
			free_du(du);
			continue;
		}

		dm_list_add(&cmd->use_devices, &du->list);
	}
	if (fclose(fp))
		stack;

	return ret;
}

int device_ids_write(struct cmd_context *cmd)
{
	char dirpath[PATH_MAX];
	char tmppath[PATH_MAX];
	char version_buf[VERSION_LINE_MAX] = {0};
	FILE *fp;
	int dir_fd;
	time_t t;
	struct dev_use *du;
	const char *devname;
	const char *pvid;
	uint32_t df_major = 0, df_minor = 0, df_counter = 0;
	int file_exists;
	int ret = 1;

	if (!cmd->enable_devices_file && !cmd->pending_devices_file)
		return 1;

	/*
	 * pending_devices_file: setup_devices found no system devices file
	 * exists and has not enabled the devices file, but may want to
	 * create a new devices file here and enable it.
	 *
	 * If this is pvcreate/vgcreate with the system devices file,
	 * and the devices file doesn't exist, then we may not want to
	 * create one for the new PVs created.  This is because doing so
	 * would cause existing PVs on the system to be left out and not
	 * be visible.  So, if the pvcreate/vgcreate have seen existing PVs
	 * during the label scan, then skip creating/writing a new system
	 * devices file.  But, if they have not seen any other PVs, then
	 * create a new system devices file here with the newly created PVs.
	 * The idea is that pvcreate/vgcreate of the first PVs is probably
	 * system installation, and we'd like to have a devices file created
	 * automatically during installation.  (The installer could also touch
	 * the devices file to create it, and that would cause
	 * pvcreate/vgcreate to always populate it.)
	 */
	file_exists = devices_file_exists(cmd);

	log_debug("device_ids_write create %d edit %d pending %d exists %d version %s devicesfile %s",
		  cmd->create_edit_devices_file, cmd->edit_devices_file, cmd->pending_devices_file, file_exists,
		  _devices_file_version[0] ? _devices_file_version : ".", cmd->devicesfile ?: ".");

	if (cmd->pending_devices_file && cmd->create_edit_devices_file && !cmd->devicesfile && !file_exists &&
	    (!strncmp(cmd->name, "pvcreate", 8) || !strncmp(cmd->name, "vgcreate", 8))) {
		/* If any PVs were seen during scan then don't create a new devices file. */
		if (lvmcache_vg_info_count()) {
			log_warn("Not creating system devices file due to existing VGs.");
			free_dus(&cmd->use_devices);
			return 1;
		}
		log_warn("Creating devices file %s", cmd->devices_file_path);
		cmd->enable_devices_file = 1;
	}

	if (test_mode())
		return 1;

	if (_devices_file_version[0]) {
		if (sscanf(_devices_file_version, "%u.%u.%u", &df_major, &df_minor, &df_counter) != 3) {
			/* don't update a file we can't parse */
			log_warn("WARNING: not updating devices file with unparsed version.");
			return 0;
		}
		if (df_major > DEVICES_FILE_MAJOR) {
			/* don't update a file with a newer major version */
			log_warn("WARNING: not updating devices file with larger major version.");
			return 0;
		}
	}

	if (dm_snprintf(dirpath, sizeof(dirpath), "%s/devices", cmd->system_dir) < 0) {
		ret = 0;
		goto out;
	}

	if (dm_snprintf(tmppath, sizeof(tmppath), "%s_new", cmd->devices_file_path) < 0) {
		ret = 0;
		goto out;
	}

	(void) unlink(tmppath); /* in case a previous file was left */

	if (!(fp = fopen(tmppath, "w+"))) {
		log_warn("Cannot open tmp devices_file to write.");
		ret = 0;
		goto out;
	}

	if ((dir_fd = open(dirpath, O_RDONLY)) < 0) {
		if (fclose(fp))
                        log_sys_debug("fclose", tmppath);
		ret = 0;
		goto out;
	}

	t = time(NULL);

	fprintf(fp, "# LVM uses devices listed in this file.\n");
	fprintf(fp, "# Created by LVM command %s pid %d at %s", cmd->name, getpid(), ctime(&t));

	/*
	 * It's useful to ensure that this devices file is associated to a
	 * single system because this file can be used to control access to
	 * shared devices.  If this file is copied/cloned to another system,
	 * that new system should not automatically gain access to the devices
	 * that the original system is using.
	 */
	if (cmd->system_id)
		fprintf(fp, "SYSTEMID=%s\n", cmd->system_id);

	if (dm_snprintf(version_buf, VERSION_LINE_MAX, "VERSION=%u.%u.%u", DEVICES_FILE_MAJOR, DEVICES_FILE_MINOR, df_counter+1) < 0)
		stack;
	else
		fprintf(fp, "%s\n", version_buf);

	/* as if we had read this version in case we want to write again */
	memset(_devices_file_version, 0, sizeof(_devices_file_version));
	_copy_idline_str(version_buf, _devices_file_version, sizeof(_devices_file_version));

	dm_list_iterate_items(du, &cmd->use_devices) {
		devname = du->dev ? dev_name(du->dev) : du->devname;
		if (!devname || devname[0] != '/')
			devname = ".";

		if (!du->pvid || !du->pvid[0] || (du->pvid[0] == '.'))
			pvid = ".";
		else
			pvid = du->pvid;

		if (du->part) {
			fprintf(fp, "IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s PART=%d\n",
				idtype_to_str(du->idtype) ?: ".",
				du->idname ?: ".", devname, pvid, du->part);
		} else {
			fprintf(fp, "IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s\n",
				idtype_to_str(du->idtype) ?: ".",
				du->idname ?: ".", devname, pvid);
		}
	}

	if (fflush(fp))
		stack;
	if (fclose(fp))
		stack;

	if (rename(tmppath, cmd->devices_file_path) < 0) {
		log_error("Failed to replace devices file errno %d", errno);
		ret = 0;
	}

	if (fsync(dir_fd) < 0)
		stack;
	if (close(dir_fd) < 0)
		stack;

	log_debug("Wrote devices file %s", version_buf);
out:
	return ret;
}

static void _device_ids_update_try(struct cmd_context *cmd)
{
	int held = 0;

	if (cmd->expect_missing_vg_device) {
		log_print_unless_silent("Devices file update skipped.");
		return;
	}

	/*
	 * Use a non-blocking lock since it's not essential to
	 * make this update, the next cmd will make these changes
	 * if we skip it this update.
	 * If this command already holds an ex lock on the
	 * devices file, lock_devices_file ex succeeds and
	 * held is set.
	 * If we get the lock, only update the devices file if
	 * it's not been changed since we read it.
	 */
	if (!lock_devices_file_try(cmd, LOCK_EX, &held)) {
		log_debug("Skip devices file update (busy).");
	} else {
		if (device_ids_version_unchanged(cmd)) {
			if (!device_ids_write(cmd))
				stack;
		} else
			log_debug("Skip devices file update (changed).");
	}
	if (!held)
		unlock_devices_file(cmd);
}

int device_ids_version_unchanged(struct cmd_context *cmd)
{
	char line[PATH_MAX];
	char version_buf[VERSION_LINE_MAX];
	FILE *fp;

	if (!(fp = fopen(cmd->devices_file_path, "r"))) {
		log_warn("WARNING: cannot open devices file to read.");
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#')
			continue;

		if (!strncmp(line, "VERSION", 7)) {
			if (fclose(fp))
				stack;

			_copy_idline_str(line, version_buf, sizeof(version_buf));

			log_debug("check devices file version %s prev %s", version_buf, _devices_file_version);

			if (!strcmp(version_buf, _devices_file_version))
				return 1;
			return 0;
		}
	}

	if (fclose(fp))
		stack;
	return 0;
}

int device_ids_use_devname(struct cmd_context *cmd)
{
	struct dev_use *du;

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (du->idtype == DEV_ID_TYPE_DEVNAME)
			return 1;
	}
	return 0;
}

static int _device_ids_use_lvmlv(struct cmd_context *cmd)
{
	struct dev_use *du;

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (du->idtype == DEV_ID_TYPE_LVMLV_UUID)
			return 1;
	}
	return 0;
}

struct dev_use *get_du_for_devno(struct cmd_context *cmd, dev_t devno)
{
	struct dev_use *du;

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (du->dev && du->dev->dev == devno)
			return du;
	}
	return NULL;
}

struct dev_use *get_du_for_dev(struct cmd_context *cmd, struct device *dev)
{
	struct dev_use *du;

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (du->dev == dev)
			return du;
	}
	return NULL;
}

struct dev_use *get_du_for_pvid(struct cmd_context *cmd, const char *pvid)
{
	struct dev_use *du;

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (!du->pvid)
			continue;
		if (!memcmp(du->pvid, pvid, ID_LEN))
			return du;
	}
	return NULL;
}

struct dev_use *get_du_for_devname(struct cmd_context *cmd, const char *devname)
{
	struct dev_use *du;

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (!du->devname)
			continue;
		if (!strcmp(du->devname, devname))
			return du;
	}
	return NULL;
}

struct dev_use *get_du_for_device_id(struct cmd_context *cmd, uint16_t idtype, const char *idname)
{
	struct dev_use *du;

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (du->idname && (du->idtype == idtype) && !strcmp(du->idname, idname))
			return du;
	}
	return NULL;
}

/*
 * Add or update entry for this dev.
 * . add an entry to dev->ids and point dev->id to it
 * . add or update entry in cmd->use_devices
 */
int device_id_add(struct cmd_context *cmd, struct device *dev, const char *pvid_arg,
		  const char *idtype_arg, const char *id_arg, int use_idtype_only)
{
	char pvid[ID_LEN+1] = { 0 };
	uint16_t idtype = 0;
	const char *idname = NULL;
	const char *check_idname = NULL;
	const char *update_matching_kind = NULL;
	const char *update_matching_name = NULL;
	struct dev_use *du, *update_du = NULL, *du_dev, *du_pvid, *du_devname, *du_devid;
	struct dev_id *id;
	int found_id = 0;
	int part = 0;

	if (!dev_get_partition_number(dev, &part))
		return_0;

	/* Ensure valid dev_name(dev) below. */
	if (dm_list_empty(&dev->aliases))
		return_0;

	/*
	 * When enable_devices_file=0 and pending_devices_file=1 we let
	 * pvcreate/vgcreate add new du's to cmd->use_devices.  These du's may
	 * be written to a new system devices file in device_ids_write, or they
	 * may not, or devices_file_write may decide not to write a new system
	 * devices file and devices file may remain disabled.
	 */
	if (!cmd->enable_devices_file && !cmd->pending_devices_file)
		 return 1;

	/*
	 * The pvid_arg may be passed from a 'struct id' (pv->id) which
	 * may not have a terminating \0.
	 * Make a terminated copy to use as a string.
	 */
	memcpy(&pvid, pvid_arg, ID_LEN);

	/*
	 * Choose the device_id type for the device being added.
	 * possible breakage:
	 * . if the kernel changes what it prints from sys/wwid (e.g. from
	 *   the t10 value to the naa value for the dev), this would break
	 *   matching du to dev unless lvm tries to match all of the dev's
	 *   different wwids from vpd_pg83 against sys_wwid entries.
	 * . adding a new device_id type into the devices file breaks prior
	 *   lvm versions that attempt to use the devices file from the new
	 *   lvm version.
	 * . using a value for sys_wwid that comes from vpd_pg83 and not
	 *   sys/wwid (e.g. taking a naa wwid from vpd_pg83 when sys/wwid
	 *   is printing the t10 wwid) would break prior lvm versions that
	 *   only match a du against the sys/wwid values.
	 */

	if (idtype_arg) {
		if (!(idtype = idtype_from_str(idtype_arg))) {
			if (use_idtype_only) {
				log_error("The specified --deviceidtype %s is unknown.", idtype_arg);
				return 0;
			}
			log_warn("WARNING: ignoring unknown device_id type %s.", idtype_arg);
		} else {
			if (id_arg) {
				if ((idname = strdup(id_arg)))
					goto id_done;
				log_warn("WARNING: ignoring device_id name %s.", id_arg);
			}

			if ((idname = device_id_system_read(cmd, dev, idtype)))
				goto id_done;

			if (use_idtype_only) {
				log_error("The specified --deviceidtype %s is not available for %s.", idtype_arg, dev_name(dev));
				return 0;
			}

			log_warn("WARNING: ignoring deviceidtype %s which is not available for device.", idtype_arg);
			idtype = 0;
		}
	}

	if (MAJOR(dev->dev) == cmd->dev_types->device_mapper_major) {
		if (dev_has_mpath_uuid(cmd, dev, &idname)) {
			idtype = DEV_ID_TYPE_MPATH_UUID;
			goto id_done;
		}

		if (_dev_has_crypt_uuid(cmd, dev, &idname)) {
			idtype = DEV_ID_TYPE_CRYPT_UUID;
			goto id_done;
		}

		if (_dev_has_lvmlv_uuid(cmd, dev, &idname)) {
			idtype = DEV_ID_TYPE_LVMLV_UUID;
			goto id_done;
		}
	}

	/* TODO: kpartx partitions on loop devs. */
	if (MAJOR(dev->dev) == cmd->dev_types->loop_major) {
		idtype = DEV_ID_TYPE_LOOP_FILE;
		if ((idname = device_id_system_read(cmd, dev, idtype)))
			goto id_done;
		goto id_last;
	}

	if (MAJOR(dev->dev) == cmd->dev_types->md_major) {
		idtype = DEV_ID_TYPE_MD_UUID;
		if ((idname = device_id_system_read(cmd, dev, idtype)))
			goto id_done;
		goto id_last;
	}

	if (MAJOR(dev->dev) == cmd->dev_types->drbd_major) {
		/* TODO */
		log_warn("Missing support for DRBD idtype");
		goto id_last;
	}

	/*
	 * No device-specific, existing, or user-specified idtypes,
	 * so use first available of sys_wwid, wwid_naa, wwid_eui,
	 * wwid_t10, sys_serial, devname.
	 */

	idtype = DEV_ID_TYPE_SYS_WWID;
	if ((idname = device_id_system_read(cmd, dev, idtype)))
		goto id_done;

	idtype = DEV_ID_TYPE_WWID_NAA;
	if ((idname = device_id_system_read(cmd, dev, idtype)))
		goto id_done;

	idtype = DEV_ID_TYPE_WWID_EUI;
	if ((idname = device_id_system_read(cmd, dev, idtype)))
		goto id_done;

	idtype = DEV_ID_TYPE_WWID_T10;
	if ((idname = device_id_system_read(cmd, dev, idtype)))
		goto id_done;

	idtype = DEV_ID_TYPE_SYS_SERIAL;
	if ((idname = device_id_system_read(cmd, dev, idtype)))
		goto id_done;
id_last:
	idtype = DEV_ID_TYPE_DEVNAME;
	if ((idname = device_id_system_read(cmd, dev, idtype)))
		goto id_done;

id_done:
	if (!idname)
		return_0;

	/*
	 * Create a dev_id struct for the new idtype on dev->ids.
	 */
	dm_list_iterate_items(id, &dev->ids) {
		if (id->idtype == idtype) {
			found_id = 1;
			break;
		}
	}

	if (found_id && idname && (!id->idname || strcmp(id->idname, idname))) {
		log_debug("Replacing device id %s old %s new %s",
			  idtype_to_str(id->idtype), id->idname ?: ".", idname);
		dm_list_del(&id->list);
		free_did(id);
		found_id = 0;
	}
	if (!found_id) {
		if (!(id = zalloc(sizeof(struct dev_id)))) {
			free((char *)idname);
			return_0;
		}
		id->idtype = idtype;
		id->idname = (char *)idname;
		id->dev = dev;
		dm_list_add(&dev->ids, &id->list);
	} else
		free((char*)idname);

	dev->id = id;
	dev->flags |= DEV_MATCHED_USE_ID;

	idname = NULL;
	idtype = 0;

	/*
	 * "dev" is the device we are adding.
	 * "id" is the device_id it's using, set in dev->id.
	 *
	 * Update the cmd->use_devices list for the new device.  The
	 * use_devices list will be used to update the devices file.
	 *
	 * The dev being added can potentially overlap existing entries
	 * in various ways.  If one of the existing entries is truely for
	 * this device being added, then we want to update that entry.
	 * If some other existing entries are not for the same device, but
	 * have some overlapping values, then we want to try to update
	 * those other entries to fix any incorrect info.
	 */

	/* Is there already an entry matched to this device? */
	du_dev = get_du_for_dev(cmd, dev);

	/* Is there already an entry matched to this device's pvid? */
	du_pvid = get_du_for_pvid(cmd, pvid);

	/* Is there already an entry using this device's name? */
	du_devname = get_du_for_devname(cmd, dev_name(dev));

	/* Is there already an entry using the device_id for this device? */
	du_devid = get_du_for_device_id(cmd, id->idtype, id->idname);

	if (du_dev)
		log_debug("device_id_add %s pvid %s matches entry %p dev %s",
			  dev_name(dev), pvid, du_dev, dev_name(du_dev->dev));
	if (du_pvid)
		log_debug("device_id_add %s pvid %s matches entry %p dev %s with same pvid %s",
			  dev_name(dev), pvid, du_pvid, du_pvid->dev ? dev_name(du_pvid->dev) : ".",
			  du_pvid->pvid);
	if (du_devid)
		log_debug("device_id_add %s pvid %s matches entry %p dev %s with same device_id %d %s",
			  dev_name(dev), pvid, du_devid, du_devid->dev ? dev_name(du_devid->dev) : ".",
			  du_devid->idtype, du_devid->idname);
	if (du_devname)
		log_debug("device_id_add %s pvid %s matches entry %p dev %s with same devname %s",
			  dev_name(dev), pvid, du_devname, du_devname->dev ? dev_name(du_devname->dev) : ".",
			  du_devname->devname);

	if (du_pvid && (du_pvid->dev != dev))
		log_warn("WARNING: adding device %s with PVID %s which is already used for %s device_id %s.",
			 dev_name(dev), pvid, du_pvid->dev ? dev_name(du_pvid->dev) : "missing device",
			 du_pvid->idname ?: "none");

	if (du_devid && (du_devid->dev != dev)) {
		if (!du_devid->dev) {
			log_warn("WARNING: adding device %s with idname %s which is already used for missing device.",
				 dev_name(dev), id->idname);
		} else {
			int ret1, ret2;
			dev_t devt1, devt2;
			/* Check if both entries are partitions of the same device. */
			ret1 = dev_get_primary_dev(cmd->dev_types, dev, &devt1);
			ret2 = dev_get_primary_dev(cmd->dev_types, du_devid->dev, &devt2);
			if ((ret1 == 2) && (ret2 == 2) && (devt1 == devt2)) {
				log_debug("Using separate entries for partitions of same device %s part %d %s part %d.",
					  dev_name(dev), part, dev_name(du_devid->dev), du_devid->part);
			} else {
				log_warn("WARNING: adding device %s with idname %s which is already used for %s.",
					 dev_name(dev), id->idname, dev_name(du_devid->dev));
			}
		}
	}

	/*
	 * If one of the existing entries (du_dev, du_pvid, du_devid, du_devname)
	 * is truely for the same device that is being added, then set update_du to
	 * that existing entry to be updated.
	 */

	if (du_dev) {
		update_du = du_dev;
		dm_list_del(&update_du->list);
		update_matching_kind = "device";
		update_matching_name = dev_name(dev);
	} else if (du_pvid) {
		/*
		 * If the device_id of the existing entry for PVID is the same
		 * as the device_id of the device being added, then update the
		 * existing entry.  If the device_ids differ, then the devices
		 * have duplicate PVIDs, and the new device gets a new entry
		 * (if we allow it to be added.)
		 */
		if (du_pvid->idtype == id->idtype)
			check_idname = strdup(id->idname);
		else
			check_idname = device_id_system_read(cmd, dev, du_pvid->idtype);

		if (!du_pvid->idname || (check_idname && !strcmp(check_idname, du_pvid->idname))) {
			update_du = du_pvid;
			dm_list_del(&update_du->list);
			update_matching_kind = "PVID";
			update_matching_name = pvid;
		} else {
			if (!cmd->current_settings.yes &&
			    yes_no_prompt("Add device with duplicate PV to devices file?") == 'n') {
				log_print_unless_silent("Device not added.");
				free((void *)check_idname);
				return 1;
			}
		}
	} else if (du_devid) {
		/*
		 * Do we create a new du or update the existing du?
		 * If it's the same device, update the existing du,
		 * but if it's two devices with the same device_id, then
		 * create a new du.
		 *
		 * We know that 'dev' has device_id 'id'.
		 * Check if du_devid->dev is different from 'dev'
		 * and that du_devid->idname matches id.
		 * If so, then there are two different devices with
		 * the same device_id (create a new du for dev.)
		 * If not, then update the existing du_devid.
		 */
		if (du_devid->dev == dev) {
			/* update the existing entry with matching devid */
			update_du = du_devid;
			dm_list_del(&update_du->list);
			update_matching_kind = "device_id";
			update_matching_name = id->idname;
		}
	}

	free((void *)check_idname);

	if (!update_du) {
		log_debug("Adding new entry to devices file for %s PVID %s %s %s.",
			  dev_name(dev), pvid, idtype_to_str(id->idtype), id->idname);
		if (!(du = zalloc(sizeof(struct dev_use))))
			return_0;
	} else {
		du = update_du;
		log_debug("Updating existing entry in devices file for %s that matches %s %s.",
			  dev_name(dev), update_matching_kind, update_matching_name);
	}

	free(du->idname);
	free(du->devname);
	free(du->pvid);

	du->idtype = id->idtype;
	du->idname = strdup(id->idname);
	du->devname = strdup(dev_name(dev));
	du->dev = dev;
	du->pvid = strdup(pvid);

	dev_get_partition_number(dev, &du->part);

	if (!du->idname || !du->devname || !du->pvid) {
		free_du(du);
		return_0;
	}

	dm_list_add(&cmd->use_devices, &du->list);

	return 1;
}

/*
 * Update entry for this dev.
 * Set PVID=.
 * update entry in cmd->use_devices
 */
void device_id_pvremove(struct cmd_context *cmd, struct device *dev)
{
	struct dev_use *du;

	if (!cmd->enable_devices_file)
		return;

	if (!(du = get_du_for_dev(cmd, dev))) {
		log_warn("WARNING: devices to use does not include %s", dev_name(dev));
		return;
	}

	if (du->pvid) {
		free(du->pvid);
		du->pvid = NULL;
	}
}

void device_id_update_vg_uuid(struct cmd_context *cmd, struct volume_group *vg, struct id *old_vg_id)
{
	struct dev_use *du;
	struct lv_list *lvl;
	char old_vgid[ID_LEN+1] = { 0 };
	char new_vgid[ID_LEN+1] = { 0 };
	char old_idname[PATH_MAX];
	int update = 0;

	if (!cmd->enable_devices_file)
		return;

	/* Without this setting there is no stacking LVs on PVs. */
	if (!cmd->scan_lvs)
		return;

	/* Check if any devices file entries are stacked on LVs. */
	if (!_device_ids_use_lvmlv(cmd))
		return;

	memcpy(old_vgid, old_vg_id, ID_LEN);
	memcpy(new_vgid, &vg->id, ID_LEN);

	/*
	 * for each LV in VG, if there is a du for that LV (meaning a PV exists
	 * on the LV), then update the du idname, replacing the old vgid with
	 * the new vgid.
	 */
	dm_list_iterate_items(lvl, &vg->lvs) {
		memset(old_idname, 0, sizeof(old_idname));
		memcpy(old_idname, "LVM-", 4);
		memcpy(old_idname+4, old_vgid, ID_LEN);
		memcpy(old_idname+4+ID_LEN, &lvl->lv->lvid.id[1], ID_LEN);

		if ((du = get_du_for_device_id(cmd, DEV_ID_TYPE_LVMLV_UUID, old_idname))) {
			log_debug("device_id update %s pvid %s vgid %s to %s",
				  du->devname ?: ".", du->pvid ?: ".", old_vgid, new_vgid);
			memcpy(du->idname+4, new_vgid, ID_LEN);
			update = 1;

			if (du->dev && du->dev->id && (du->dev->id->idtype == DEV_ID_TYPE_LVMLV_UUID))
				memcpy(du->dev->id->idname+4, new_vgid, ID_LEN);
		}
	}

	if (update &&
	    !device_ids_write(cmd))
		stack;
	unlock_devices_file(cmd);
}

static int _idtype_compatible_with_major_number(struct cmd_context *cmd, int idtype, unsigned major)
{
	/* devname can be used with any kind of device */
	if (idtype == DEV_ID_TYPE_DEVNAME)
		return 1;

	if (idtype == DEV_ID_TYPE_MPATH_UUID ||
	    idtype == DEV_ID_TYPE_CRYPT_UUID ||
	    idtype == DEV_ID_TYPE_LVMLV_UUID)
		return (major == cmd->dev_types->device_mapper_major);

	if (idtype == DEV_ID_TYPE_MD_UUID)
		return (major == cmd->dev_types->md_major);

	if (idtype == DEV_ID_TYPE_LOOP_FILE)
		return (major == cmd->dev_types->loop_major);

	if (major == cmd->dev_types->device_mapper_major)
		return (idtype == DEV_ID_TYPE_MPATH_UUID ||
			idtype == DEV_ID_TYPE_CRYPT_UUID ||
			idtype == DEV_ID_TYPE_LVMLV_UUID ||
			idtype == DEV_ID_TYPE_DEVNAME);

	if (major == cmd->dev_types->md_major)
		return (idtype == DEV_ID_TYPE_MD_UUID ||
			idtype == DEV_ID_TYPE_DEVNAME);

	if (major == cmd->dev_types->loop_major)
		return (idtype == DEV_ID_TYPE_LOOP_FILE ||
			idtype == DEV_ID_TYPE_DEVNAME);

	return 1;
}

static int _match_dm_devnames(struct cmd_context *cmd, struct device *dev,
			      struct dev_id *id, struct dev_use *du)
{
	struct stat buf;

	if (MAJOR(dev->dev) != cmd->dev_types->device_mapper_major)
		return 0;

	if (id->idname && du->idname && !strcmp(id->idname, du->idname))
		return 1;

	if (du->idname && !strcmp(du->idname, dev_name(dev))) {
		log_debug("Match device_id %s %s to %s: ignoring idname %s",
			  idtype_to_str(du->idtype), du->idname, dev_name(dev), id->idname ?: ".");
		return 1;
	}

	if (!du->idname)
		return 0;

	/* detect that a du entry is for a dm device */

	if (!strncmp(du->idname, "/dev/dm-", 8) || !strncmp(du->idname, "/dev/mapper/", 12)) {
		if (stat(du->idname, &buf))
			return 0;

		if ((MAJOR(buf.st_rdev) == cmd->dev_types->device_mapper_major) &&
		    (MINOR(buf.st_rdev) == MINOR(dev->dev))) {
			log_debug("Match device_id %s %s to %s: using other dm name, ignoring %s",
				  idtype_to_str(du->idtype), du->idname, dev_name(dev), id->idname ?: ".");
			return 1;
		}
	}

	return 0;
}

/*
 * du is a devices file entry.  dev is any device on the system.
 * check if du is for dev by comparing the device's ids to du->idname.
 *
 * check for a dev->ids entry with du->idtype, if found compare it,
 * if not, system_read idtype for the dev, add entry to dev->ids,
 * compare it to du to check if it matches.
 *
 * When a match is found, set up links among du/id/dev.
 */

static int _match_du_to_dev(struct cmd_context *cmd, struct dev_use *du, struct device *dev)
{
	char du_idname[PATH_MAX];
	struct dev_id *id;
	const char *idname;
	int part;

	/*
	 * The idname will be removed from an entry with devname type when the
	 * devname is read and found to hold a different PVID than the PVID in
	 * the entry.  At that point we only have the PVID and no known
	 * location for it.
	 */
	if (!du->idname || !du->idtype) {
		/*
		log_debug("Mismatch device_id %s %s %s to %s",
			  du->idtype ? idtype_to_str(du->idtype) : "idtype_missing",
			  du->idname ? du->idname : "idname_missing",
			  du->devname ? du->devname : "devname_missing",
			  dev_name(dev));
		*/
		return 0;
	}

	/*
	 * Some idtypes can only match devices with a specific major number,
	 * so we can skip trying to match certain du entries based simply on
	 * the major number of dev.
	 */
	if (!_idtype_compatible_with_major_number(cmd, du->idtype, (int)MAJOR(dev->dev))) {
		/*
		log_debug("Mismatch device_id %s %s to %s: wrong major",
			  idtype_to_str(du->idtype), du->idname ?: ".", dev_name(dev));
		*/
		return 0;
	}

	if (!dev_get_partition_number(dev, &part)) {
		/*
		log_debug("Mismatch device_id %s %s to %s: no partition",
			  idtype_to_str(du->idtype), du->idname ?: ".", dev_name(dev));
		*/
		return 0;
	}
	if (part != du->part) {
		/*
		log_debug("Mismatch device_id %s %s to %s: wrong partition %d vs %d",
			  idtype_to_str(du->idtype), du->idname ?: ".", dev_name(dev), du->part, part);
		*/
		return 0;
	}

	/*
	 * sys_wwid and sys_serial were saved in the past with leading and
	 * trailing spaces replaced with underscores, and t10 wwids also had
	 * repeated internal spaces replaced with one underscore each.  Now we
	 * ignore leading and trailing spaces and replace multiple repeated
	 * spaces with one underscore in t10 wwids.  In order to handle
	 * system.devices entries created by older versions, modify the IDNAME
	 * value that's read (du->idname) to remove leading and trailing
	 * underscores, and reduce repeated underscores to one in t10 wwids.
	 *
	 * Example: wwid is reported as "  t10.123  456  " (without quotes)
	 * Previous versions would save this in system.devices as: __t10.123__456__
	 * Current versions will save this in system.devices as: t10.123_456
	 * device_id_system_read() now returns: t10.123_456
	 * When this code reads __t10.123__456__ from system.devices, that
	 * string is modified to t10.123_456 so that it will match the value
	 * returned from device_id_system_read().
	 */
	strncpy(du_idname, du->idname, PATH_MAX-1);
	if (((du->idtype == DEV_ID_TYPE_SYS_WWID) || (du->idtype == DEV_ID_TYPE_SYS_SERIAL)) &&
	    strchr(du_idname, '_')) {
		_remove_leading_underscores(du_idname, sizeof(du_idname));
		_remove_trailing_underscores(du_idname, sizeof(du_idname));
		if (du->idtype == DEV_ID_TYPE_SYS_WWID && !strncmp(du_idname, "t10", 3) && strstr(du_idname, "__"))
			_reduce_repeating_underscores(du_idname, sizeof(du_idname));
	}

	/*
	 * Try to match du with ids that have already been read for the dev
	 * (and saved on dev->ids to avoid rereading.)
	 */
	dm_list_iterate_items(id, &dev->ids) {
		if (!id->idname)
			continue;

		if (id->idtype == du->idtype) {
			/*
			 * dm names can have different forms, so matching names
			 * is not always a direct comparison.
			 */
			if ((id->idtype == DEV_ID_TYPE_DEVNAME) && _match_dm_devnames(cmd, dev, id, du)) {
				/* dm devs can have differing names that we know still match */
				du->dev = dev;
				dev->id = id;
				dev->flags |= DEV_MATCHED_USE_ID;
				log_debug("Match device_id %s %s to %s: dm names",
					  idtype_to_str(du->idtype), du->idname, dev_name(dev));
				return 1;
			}

			if (!strcmp(id->idname, du_idname)) {
				du->dev = dev;
				dev->id = id;
				dev->flags |= DEV_MATCHED_USE_ID;
				log_debug("Match device_id %s %s to %s",
					  idtype_to_str(du->idtype), du_idname, dev_name(dev));
				return 1;

			} else {
				/*
				log_debug("Mismatch device_id %s %s to %s: idname %s",
			  		  idtype_to_str(du->idtype), du->idname ?: ".", dev_name(dev), id->idname ?: ":");
				*/
				return 0;
			}
		}
	}

	if (!(id = zalloc(sizeof(struct dev_id))))
		return_0;

	idname = device_id_system_read(cmd, dev, du->idtype);

	/*
	 * Save this id for the dev, even if it doesn't exist (NULL)
	 * or doesn't match du.  This avoids system_read of this idtype
	 * repeatedly, and the saved id will be found in the loop
	 * over dev->ids above.
	 */
	id->idtype = du->idtype;
	id->idname = (char *)idname ?: (char *)"";
	id->dev = dev;
	dm_list_add(&dev->ids, &id->list);

	if (idname && !strcmp(idname, du_idname)) {
		du->dev = dev;
		dev->id = id;
		dev->flags |= DEV_MATCHED_USE_ID;
		log_debug("Match device_id %s %s to %s",
			  idtype_to_str(du->idtype), idname, dev_name(dev));
		return 1;
	}

	/*
	log_debug("Mismatch device_id %s %s to %s: idname %s",
		  idtype_to_str(du->idtype), du->idname ?: ".", dev_name(dev), idname ?: ".");
	*/

	/*
	 * Make the du match this device if the dev has a vpd_pg83 wwid
	 * that matches du->idname, even if the sysfs wwid for dev did
	 * not match the du->idname.  This could happen if sysfs changes
	 * which wwid it reports (there are often multiple), or if lvm in
	 * the future selects a sys_wwid value from vpd_pg83 data rather
	 * than from the sysfs wwid.
	 *
	 * TODO: update the df entry IDTYPE somewhere?
	 */
	if (du->idtype == DEV_ID_TYPE_SYS_WWID) {
		struct dev_wwid *dw;

	       	if (!(dev->flags & DEV_ADDED_VPD_WWIDS))
			dev_read_vpd_wwids(cmd, dev);

		dm_list_iterate_items(dw, &dev->wwids) {
			if (!strcmp(dw->id, du_idname)) {
				if (!(id = zalloc(sizeof(struct dev_id))))
					return_0;
				/* wwid types are 1,2,3 and idtypes are DEV_ID_TYPE_ */
				id->idtype = wwid_type_to_idtype(dw->type);
				id->idname = strdup(dw->id);
				id->dev = dev;
				dm_list_add(&dev->ids, &id->list);
				du->dev = dev;
				dev->id = id;
				dev->flags |= DEV_MATCHED_USE_ID;
				log_print_unless_silent("Match device_id %s %s to vpd_pg83 %s %s.",
							idtype_to_str(du->idtype), du_idname,
							idtype_to_str(id->idtype), dev_name(dev));
				du->idtype = id->idtype;
				return 1;
			}
		}
	}

	return 0;
}

int device_ids_match_dev(struct cmd_context *cmd, struct device *dev)
{
	struct dev_use *du;

	/* First check the du entry with matching devname since it's likely correct. */
	if ((du = get_du_for_devname(cmd, dev_name(dev)))) {
		if (_match_du_to_dev(cmd, du, dev))
			return 1;
	}

	/* Check all du entries since the devname could have changed. */
	dm_list_iterate_items(du, &cmd->use_devices) {
		if (!_match_du_to_dev(cmd, du, dev))
			continue;
		return 1;
	}

	return 0;
}

/*
 * For each entry on cmd->use_devices (entries in the devices file),
 * find a struct device from dev-cache.  They are paired based strictly
 * on the device id.
 *
 * This must not open or read devices.  This function cannot use filters.
 * filters are applied after this, and the filters may open devs in the first
 * nodata filtering.  The second filtering, done after label_scan has read
 * a device, is allowed to read a device to evaluate filters that need to see
 * data from the dev.
 *
 * When a device id of a particular type is obtained for a dev, a id for that
 * type is saved in dev->ids in case it needs to be checked again.
 *
 * When a device in dev-cache is matched to an entry in the devices file
 * (a struct dev_use), then:
 * . du->dev = dev;
 * . dev->id = id;
 * . dev->flags |= DEV_MATCHED_USE_ID;
 *
 * Later when filter-deviceid is run to exclude devices that are not
 * included in the devices file, the filter checks if DEV_MATCHED_USE_ID
 * is set which means that the dev matches a devices file entry and
 * passes the filter.
 */

void device_ids_match_device_list(struct cmd_context *cmd)
{
	struct dev_use *du;

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (du->dev)
			continue;
		if (!(du->dev = dev_cache_get_existing(cmd, du->devname, NULL))) {
			log_warn("Device not found for %s.", du->devname);
		} else {
			/* Should we set dev->id?  Which idtype?  Use --deviceidtype? */
			du->dev->flags |= DEV_MATCHED_USE_ID;
		}
	}
}

void device_ids_match(struct cmd_context *cmd)
{
	struct dev_iter *iter;
	struct dev_use *du;
	struct device *dev;

	if (cmd->enable_devices_list) {
		device_ids_match_device_list(cmd);
		return;
	}

	if (!cmd->enable_devices_file)
		return;

	log_debug("compare devices file entries to devices");

	/*
	 * We would set cmd->filter_deviceid_skip but we are disabling
	 * all filters (dev_cache_get NULL arg) so it's not necessary.
	 */

	dm_list_iterate_items(du, &cmd->use_devices) {
		/* already matched */
		if (du->dev) {
			log_debug("devices idname %s previously matched %s",
				  du->idname, dev_name(du->dev));
			continue;
		}

		/*
		 * du->devname from the devices file is the last known
		 * device name.  It may be incorrect, but it's usually
		 * correct, so it's an efficient place to check for a
		 * match first.
		 *
		 * NULL filter is used because we are just setting up the
		 * the du/dev pairs in preparation for using the filters.
		 */
		if (du->devname &&
		    (dev = dev_cache_get_existing(cmd, du->devname, NULL))) {
			/* On successful match, du, dev, and id are linked. */
			if (_match_du_to_dev(cmd, du, dev))
				continue;
			else {
				/*
				 * The device node may exist but the device is disconnected / zero size,
				 * and likely has no sysfs entry to check for wwid.  Continue to look
				 * for the device id on other devs.
				 */
				log_debug("devices entry %s %s devname found but not matched", du->devname, du->pvid ?: ".");
			}
		}

		/*
		 * Iterate through all devs and try to match du.
		 *
		 * If a match is made here it means the du->devname is wrong,
		 * so the device_id file should be updated with a new devname.
		 *
		 * NULL filter is used because we are just setting up the
		 * the du/dev pairs in preparation for using the filters.
		 */
		if (!(iter = dev_iter_create(NULL, 0)))
			continue;
		while ((dev = dev_iter_get(cmd, iter))) {
			if (dev->flags & DEV_MATCHED_USE_ID)
				continue;
			if (_match_du_to_dev(cmd, du, dev))
				break;
		}
		dev_iter_destroy(iter);
	}

	if (!cmd->print_device_id_not_found)
		return;

	/*
	 * Look for entries in devices file for which we found no device.
	 */
	dm_list_iterate_items(du, &cmd->use_devices) {
		/* Found a device for this entry. */
		if (du->dev && (du->dev->flags & DEV_MATCHED_USE_ID))
			continue;

		/* This shouldn't be possible. */
		if (du->dev && !(du->dev->flags & DEV_MATCHED_USE_ID)) {
			log_error("Device %s not matched to device_id", dev_name(du->dev));
			continue;
		}

		/* A detached device would get here which isn't uncommon. */

		if ((du->idtype == DEV_ID_TYPE_DEVNAME) && du->devname)
			log_warn("Devices file PVID %s last seen on %s not found.",
				 du->pvid ?: "none",
				 du->devname ?: "none");
		else if (du->idtype == DEV_ID_TYPE_DEVNAME)
			log_warn("Devices file PVID %s not found.",
				 du->pvid ?: "none");
		else if (du->devname)
			log_warn("Devices file %s %s PVID %s last seen on %s not found.",
				 idtype_to_str(du->idtype),
				 du->idname ?: "none",
				 du->pvid ?: "none",
				 du->devname);
		else
			log_warn("Devices file %s %s PVID %s not found.",
				 idtype_to_str(du->idtype),
				 du->idname ?: "none",
				 du->pvid ?: "none");
	}
}

static void _get_devs_with_serial_numbers(struct cmd_context *cmd, struct dm_list *serial_str_list, struct dm_list *devs)
{
	struct dev_iter *iter;
	struct device *dev;
	struct device_list *devl;
	struct dev_id *id;
	const char *idname;

	if (!(iter = dev_iter_create(NULL, 0)))
		return;
	while ((dev = dev_iter_get(cmd, iter))) {
		/* if serial has already been read for this dev then use it */
		dm_list_iterate_items(id, &dev->ids) {
			if (id->idtype == DEV_ID_TYPE_SYS_SERIAL) {
				if (str_list_match_item(serial_str_list, id->idname)) {
					if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
						goto next_continue;
					devl->dev = dev;
					dm_list_add(devs, &devl->list);
				}
				goto next_continue;
			}
		}

		/* just copying the no-data filters in similar device_ids_find_renamed_devs */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "sysfs"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "type"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "usable"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "mpath"))
			continue;

		if ((idname = device_id_system_read(cmd, dev, DEV_ID_TYPE_SYS_SERIAL))) {
			if (str_list_match_item(serial_str_list, idname)) {
				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					goto next_free;
				if (!(id = zalloc(sizeof(struct dev_id))))
					goto next_free;
				id->idtype = DEV_ID_TYPE_SYS_SERIAL;
				id->idname = (char *)idname;
				id->dev = dev;
				dm_list_add(&dev->ids, &id->list);
				devl->dev = dev;
				dm_list_add(devs, &devl->list);
				idname = NULL;
			}
		}
 next_free:
		if (idname)
			free((char *)idname);
 next_continue:
		continue;
	}
	dev_iter_destroy(iter);
}

/*
 * This is called after devices are scanned to compare what was found on disks
 * vs what's in the devices file.  The devices file could be outdated and need
 * correcting; the authoritative data is what's on disk.  Now that we have read
 * the device labels and know the PVID's from disk we can check the PVID's in
 * use_devices entries from the devices file.
 */

void device_ids_validate(struct cmd_context *cmd, struct dm_list *scanned_devs,
			 int *device_ids_invalid, int noupdate)
{
	struct dm_list wrong_devs;
	struct device *dev;
	struct device_list *devl;
	struct dev_use *du;
	char *tmpdup;
	int checked = 0;
	int update_file = 0;

	dm_list_init(&wrong_devs);

	if (!cmd->enable_devices_file)
		return;

	log_debug("validating devices file entries");

	/*
	 * Validate entries with proper device id types.
	 * idname is the authority for pairing du and dev.
	 */
	dm_list_iterate_items(du, &cmd->use_devices) {
		if (!du->dev)
			continue;

		/* For this idtype the idname match is unreliable. */
		if (du->idtype == DEV_ID_TYPE_DEVNAME)
			continue;

		dev = du->dev;

		/*
		 * scanned_devs are the devices that have been scanned,
		 * so they are the only devs we can verify PVID for.
		 */
		if (scanned_devs && !device_list_find_dev(scanned_devs, dev))
			continue;

		/*
		 * The matched device could not be read so we do not have
		 * the PVID from disk and cannot verify the devices file entry.
		 */
		if (dev->flags & DEV_SCAN_NOT_READ)
			continue;

		/*
		 * du and dev may have been matched, but the dev could still
		 * have been excluded by other filters during label scan.
		 * This shouldn't generally happen, but if it does the user
		 * probably wants to do something about it.
		 */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "persistent")) {
			log_warn("Devices file %s is excluded: %s.",
				 dev_name(dev), dev_filtered_reason(dev));
			continue;
		}

		checked++;

		/*
		 * If the PVID doesn't match, don't assume that the serial
		 * number is correct, since serial numbers may not be unique.
		 * Search for the PVID on other devs in device_ids_check_serial.
		 */
		if ((du->idtype == DEV_ID_TYPE_SYS_SERIAL) && du->pvid &&
		    memcmp(dev->pvid, du->pvid, ID_LEN)) {
			log_debug("suspect device id serial %s for %s", du->idname, dev_name(dev));
			str_list_add(cmd->mem, &cmd->device_ids_check_serial, dm_pool_strdup(cmd->mem, du->idname));
			*device_ids_invalid = 1;
			continue;
		}

		/*
		 * If the du pvid from the devices file does not match the
		 * pvid read from disk, replace the du pvid with the pvid from
		 * disk and update the pvid in the devices file entry.
		 */
		if (dev->pvid[0]) {
			if (!du->pvid || memcmp(dev->pvid, du->pvid, ID_LEN)) {
				log_warn("Device %s has PVID %s (devices file %s)",
					 dev_name(dev), dev->pvid, du->pvid ?: "none");
				if (!(tmpdup = strdup(dev->pvid)))
					continue;
				free(du->pvid);
				du->pvid = tmpdup;
				update_file = 1;
				*device_ids_invalid = 1;
			}
		} else {
			if (du->pvid && (du->pvid[0] != '.')) {
				log_warn("Device %s has no PVID (devices file %s)",
					 dev_name(dev), du->pvid);
				free(du->pvid);
				du->pvid = NULL;
				update_file = 1;
				*device_ids_invalid = 1;
			}
		}

		/*
		 * Avoid thrashing changes to the devices file during
		 * startup due to device names that are still being
		 * established.  Commands that may run during startup
		 * should set this flag.
		 */
		if (cmd->ignore_device_name_mismatch)
			continue;

		if (!du->devname || strcmp(dev_name(du->dev), du->devname)) {
			log_warn("Device %s has updated name (devices file %s)",
				 dev_name(du->dev), du->devname ?: "none");
			if (!(tmpdup = strdup(dev_name(du->dev))))
				continue;
			free(du->devname);
			du->devname = tmpdup;
			update_file = 1;
			*device_ids_invalid = 1;
		}
	}

	/*
	 * Validate entries with unreliable devname id type.
	 * pvid match overrides devname id match.
	 */
	dm_list_iterate_items(du, &cmd->use_devices) {
		if (!du->dev)
			continue;

		if (du->idtype != DEV_ID_TYPE_DEVNAME)
			continue;

		dev = du->dev;

		/*
		 * scanned_devs are the devices that have been scanned,
		 * so they are the only devs we can verify PVID for.
		 */
		if (scanned_devs && !device_list_find_dev(scanned_devs, dev))
			continue;

		/*
		 * The matched device could not be read so we do not have
		 * the PVID from disk and cannot verify the devices file entry.
		 */
		if (dev->flags & DEV_SCAN_NOT_READ)
			continue;

		if (dm_list_empty(&dev->aliases))
			continue;

		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "persistent")) {
			log_warn("Devices file %s is excluded: %s.",
				 dev_name(dev), dev_filtered_reason(dev));
			/* FIXME: what if this dev is wrongly matched and should be checked below? */
			continue;
		}

		if (!du->pvid || du->pvid[0] == '.')
			continue;

		checked++;

		/*
		 * A good match based on pvid.
		 */
		if (dev->pvid[0] && !memcmp(dev->pvid, du->pvid, ID_LEN)) {
			const char *devname = dev_name(dev);

			if (strcmp(devname, du->idname)) {
				/* shouldn't happen since this was basis for match */
				log_error("du for pvid %s unexpected idname %s mismatch dev %s",
					  du->pvid, du->idname, devname);
				*device_ids_invalid = 1;
				continue;
			}

			if (!du->devname || strcmp(devname, du->devname)) {
				log_warn("Device %s has updated name (devices file %s)",
					 devname, du->devname ?: "none");
				if (!(tmpdup = strdup(devname)))
					continue;
				free(du->devname);
				du->devname = tmpdup;
				update_file = 1;
				*device_ids_invalid = 1;
			}
			continue;
		}

		/*
		 * An incorrect match, the pvid read from dev does not match
		 * du->pvid for the du dev was matched to.
		 * du->idname is wrong, du->devname is probably wrong.
		 * undo the incorrect match between du and dev
		 */

		if (dev->pvid[0])
			log_warn("Devices file PVID %s not found on device %s (device PVID %s).",
				 du->pvid, dev_name(dev), dev->pvid[0] ? dev->pvid : "none");
		else
			log_warn("Devices file PVID %s not found on device %s.",
				 du->pvid, dev_name(dev));

		if ((devl = dm_pool_zalloc(cmd->mem, sizeof(*devl)))) {
			/* If this dev matches no du, drop it at the end. */
			devl->dev = dev;
			dm_list_add(&wrong_devs, &devl->list);
		}

		if (du->idname) {
			free(du->idname);
			du->idname = NULL;
		}

		/*
		 * Keep the old devname hint in place to preserve some clue about
		 * the previous location of the PV which may help the user understand
		 * what happened.
		 */
		/*
		if (du->devname) {
			free(du->devname);
			du->devname = NULL;
		}
		*/
		dev->flags &= ~DEV_MATCHED_USE_ID;
		dev->id = NULL;
		du->dev = NULL;
		update_file = 1;
		*device_ids_invalid = 1;
	}

	/*
	 * devs that were wrongly matched to a du and are not being
	 * used in another correct du should be dropped.
	 */
	dm_list_iterate_items(devl, &wrong_devs) {
		if (!get_du_for_dev(cmd, devl->dev)) {
			log_debug("Drop incorrectly matched %s", dev_name(devl->dev));
			cmd->filter->wipe(cmd, cmd->filter, devl->dev, NULL);
			lvmcache_del_dev(devl->dev);
		}
	}

	/*
	 * Check for other problems for which we want to set *device_ids_invalid,
	 * even if we don't have a way to fix them right here.  In particular,
	 * issues that may be fixed shortly by device_ids_find_renamed_devs.
	 *
	 * The device_ids_invalid flag is only used to tell the caller not
	 * to write hints, which could be based on invalid device info.
	 * (There may be a better way to deal with that then returning
	 * this flag.)
	 */
	dm_list_iterate_items(du, &cmd->use_devices) {
		if (*device_ids_invalid)
			break;

		if (!du->idname || (du->idname[0] == '.'))
			*device_ids_invalid = 1;

		if ((du->idtype == DEV_ID_TYPE_DEVNAME) && !du->dev && du->pvid)
			*device_ids_invalid = 1;
	}

	/*
	 * When a new devname/pvid mismatch is discovered, a new search for the
	 * pvid should be permitted (searched_devnames may exist to suppress
	 * searching for other pvids.)
	 */
	if (update_file)
		unlink_searched_devnames(cmd);

	/* FIXME: for wrong devname cases, wait to write new until device_ids_find_renamed_devs? */

	/*
	 * try lock and device_ids_write(), the update is not required and will
	 * be done by a subsequent command if it's not done here.
	 */
	if (update_file && noupdate) {
		log_debug("device ids validate checked %d update disabled.", checked);
	} else if (update_file) {
		log_debug("device ids validate checked %d trying to update devices file.", checked);
		_device_ids_update_try(cmd);
	} else {
		log_debug("device ids validate checked %d found no update is needed.", checked);
	}
}

/*
 * Validate entries with suspect sys_serial values.  A sys_serial du (devices
 * file entry) matched a device with the same serial number, but the PVID did
 * not match.  Check if multiple devices have the same serial number, and if so
 * pair the devs to the du's based on PVID.  This requires searching all devs
 * for the given serial number, and then reading the PVID from all those devs.
 * This may involve reading labels from devs outside the devices file.
 * (This could also be done for duplicate wwids if needed.)
 */
void device_ids_check_serial(struct cmd_context *cmd, struct dm_list *scan_devs,
			     int *update_needed, int noupdate)
{
	struct dm_list dus_check; /* dev_use_list */
	struct dm_list devs_check; /* device_list */
	struct dm_list prev_devs; /* device_id_list */
	struct dev_use_list *dul;
	struct device_list *devl, *devl2;
	struct device_id_list *dil;
	struct device *dev;
	struct dev_use *du;
	char *tmpdup;
	int update_file = 0;
	int has_pvid;
	int found;
	int count;
	int err;

	dm_list_init(&dus_check);
	dm_list_init(&devs_check);
	dm_list_init(&prev_devs);

	/*
	 * Create list of du's with a suspect serial number.  These du's will
	 * be rematched to a device using pvid.  The device_ids_check_serial
	 * list was created by device_ids_validate() when it found that the
	 * PVID on the dev did not match the PVID in the du that was paired
	 * with the dev.
	 */
	dm_list_iterate_items(du, &cmd->use_devices) {
		if (du->dev && (du->idtype == DEV_ID_TYPE_SYS_SERIAL) &&
		    str_list_match_item(&cmd->device_ids_check_serial, du->idname)) {
			if (!(dul = dm_pool_zalloc(cmd->mem, sizeof(*dul))))
				continue;
			dul->du = du;
			dm_list_add(&dus_check, &dul->list);
		}
	}

	/*
	 * Create list of devs on the system with suspect serial numbers.
	 * Read the serial number of each dev in dev cache, and return
	 * devs that match the suspect serial numbers.
	 */
	log_debug("Finding all devs with suspect serial numbers.");
	_get_devs_with_serial_numbers(cmd, &cmd->device_ids_check_serial, &devs_check);

	/*
	 * Read the PVID from any devs_check entries that have not been scanned
	 * yet (this is where some devs outside the devices file may be read.)
	 * If the dev has no PVID or is excluded by filters, then there's no
	 * point in trying to match it to one of the dus_check entries.
	 */
	log_debug("Reading and filtering %d devs with suspect serial numbers.", dm_list_size(&devs_check));
	dm_list_iterate_items_safe(devl, devl2, &devs_check) {
		const char *idname;
		if (!(idname = _dev_idname(devl->dev, DEV_ID_TYPE_SYS_SERIAL))) {
			log_debug("serial missing for %s", dev_name(devl->dev));
			continue;
		}
		if (devl->dev->flags & DEV_SCAN_FOUND_LABEL) {
			log_debug("serial %s pvid %s %s", idname, devl->dev->pvid, dev_name(devl->dev));
			continue;
		}
		if (devl->dev->flags & DEV_SCAN_FOUND_NOLABEL) {
			log_debug("serial %s nolabel %s", idname, dev_name(devl->dev));
			continue;
		}

		dev = devl->dev;
		has_pvid = 0;

		err = label_read_pvid(dev, &has_pvid);
		if (!err || !has_pvid) {
			log_debug("serial %s no pvid %s", idname, dev_name(devl->dev));
			dm_list_del(&devl->list);
			continue;
		}

		/* data-based filters use data read by label_read_pvid */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "partitioned") ||
		    !cmd->filter->passes_filter(cmd, cmd->filter, dev, "signature") ||
		    !cmd->filter->passes_filter(cmd, cmd->filter, dev, "md") ||
		    !cmd->filter->passes_filter(cmd, cmd->filter, dev, "fwraid")) {
			log_debug("serial %s pvid %s filtered %s", idname, devl->dev->pvid, dev_name(devl->dev));
			dm_list_del(&devl->list);
		}
	}

	log_debug("Checking %d PVs with suspect serial numbers.", dm_list_size(&devs_check));

	/*
	 * Unpair du's and dev's that were matched using suspect serial numbers
	 * so that things can be matched again using PVID.  If current pairings
	 * are correct they will just be matched again.  Save the previous
	 * pairings so that we can detect when a wrong pairing was corrected.
	 */
	dm_list_iterate_items(dul, &dus_check) {
		if (!dul->du->dev)
			continue;
		if (!dul->du->pvid)
			continue;
		/* save previously matched devs so they can be dropped from
		   lvmcache at the end if they are no longer used */
		if (!(dil = dm_pool_zalloc(cmd->mem, sizeof(*dil))))
			continue;
		du = dul->du;
		dil->dev = du->dev;
		memcpy(dil->pvid, du->pvid, ID_LEN);
		dm_list_add(&prev_devs, &dil->list);
		du->dev->flags &= ~DEV_MATCHED_USE_ID;
		du->dev = NULL;
	}

	/*
	 * Match du to a dev based on PVID.
	 */
	dm_list_iterate_items(dul, &dus_check) {
		if (!dul->du->pvid)
			continue;
		log_debug("Matching suspect serial device id %s PVID %s prev %s",
			  dul->du->idname, dul->du->pvid, dul->du->devname);
		found = 0;
		dm_list_iterate_items(devl, &devs_check) {
			if (!memcmp(dul->du->pvid, devl->dev->pvid, ID_LEN)) {
				/* pair dev and du */
				du = dul->du;
				dev = devl->dev;
				du->dev = dev;
				dev->flags |= DEV_MATCHED_USE_ID;

				log_debug("Match suspect serial device id %s PVID %s to %s",
					  du->idname, du->pvid, dev_name(dev));

				/* update file if this dev pairing is new or different */
				if (!(dil = device_id_list_find_dev(&prev_devs, dev)))
					update_file = 1;
				else if (memcmp(dil->pvid, du->pvid, ID_LEN))
					update_file = 1;
				found = 1;
				break;
			}
		}
		if (!found)
			log_debug("Match PVID failed in %d devs checked.", dm_list_size(&devs_check));
	}

	/*
	 * Handle du's with suspect serial numbers that did not have a match
	 * based on PVID in the previous loop.  If the du matches a device
	 * based on the serial number, and there is only one instance of that
	 * serial number on the system, then assume that the PVID in the
	 * devices file is outdated and pair the du and dev, and update the
	 * PVID in the devices file.  (This is what's done for du and dev with
	 * matching wwid but unmatching PVID.)
	 */
	dm_list_iterate_items(dul, &dus_check) {
		du = dul->du;

		/* matched in previous loop using pvid */
		if (du->dev)
			continue;

		log_debug("Matching suspect serial device id %s unmatched PVID %s prev %s",
			  du->idname, du->pvid, du->devname);
		dev = NULL;
		count = 0;
		/* count the number of devs using this serial number */
		dm_list_iterate_items(devl, &devs_check) {
			if (_dev_has_id(devl->dev, DEV_ID_TYPE_SYS_SERIAL, du->idname)) {
				dev = devl->dev;
				count++;
			}
			if (count > 1)
				break;
		}
		if (count != 1) {
			log_warn("No device matches devices file PVID %s with duplicate serial number %s previously %s.",
				 du->pvid, du->idname, du->devname);
			continue;
		}

		log_warn("Device %s with serial number %s has PVID %s (devices file %s)",
			 dev_name(dev), du->idname, dev->pvid, du->pvid ?: "none");
		if (!(tmpdup = strdup(dev->pvid)))
			continue;
		free(du->pvid);
		du->pvid = tmpdup;
		du->dev = dev;
		dev->flags |= DEV_MATCHED_USE_ID;
		update_file = 1;
	}

	/*
	 * label_scan() was done based on the original du/dev matches, so if
	 * there were some changes made to the du/dev matches above, then we
	 * may need to correct the results of the label_scan:
	 *
	 * . if some devices were scanned in label_scan, but those devs are no
	 * longer matched to any du, then we need to clear the scanned info
	 * from those devs from lvmcache.
	 *
	 * . if some devices were not scanned in label_scan, but those devs are
	 * now matched to a du, then we need to run label_scan on those devs to
	 * populate lvmcache with info from them (the caller does this.)
	 */

	/*
	 * Find devs that were previously matched to a du but now are not.
	 * Clear the filter state and lvmcache info for them.
	 */
	dm_list_iterate_items(dil, &prev_devs) {
		if (!get_du_for_dev(cmd, dil->dev)) {
			log_debug("Drop incorrectly matched serial %s", dev_name(dil->dev));
			cmd->filter->wipe(cmd, cmd->filter, dil->dev, NULL);
       			lvmcache_del_dev(dil->dev);
		}
	}

	/*
	 * Find devs that are now matched to a du but were not previously
	 * scanned by label_scan (DEV_SCAN_FOUND_LABEL).  The caller will
	 * call label_scan on the devs returned in the list.
	 */
	dm_list_iterate_items(dul, &dus_check) {
		if (!(dev = dul->du->dev))
			continue;
		if (!(dev->flags & DEV_SCAN_FOUND_LABEL)) {
			if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
				continue;
			devl->dev = dev;
			dm_list_add(scan_devs, &devl->list);
		}
	}

	/*
	 * Look for dus_check entries that were originally matched to a dev
	 * but now are not.  Warn about these like device_ids_match() would.
	 */
	dm_list_iterate_items(dul, &dus_check) {
		if (!dul->du->dev) {
			du = dul->du;
			log_warn("Devices file %s %s PVID %s not found.",
				 idtype_to_str(du->idtype),
				 du->idname ?: "none",
				 du->pvid ?: "none");
			if (du->devname) {
				free(du->devname);
				du->devname = NULL;
				update_file = 1;
			}
		}
	}

	if (update_file && update_needed)
		*update_needed = 1;

	if (update_file && !noupdate)
		_device_ids_update_try(cmd);
}

/*
 * Devices with IDNAME=devname that are mistakenly included by filter-deviceid
 * due to a devname change are fully scanned and added to lvmcache.
 * device_ids_validate() catches this by seeing that the pvid on the device
 * doesn't match what's in the devices file, and then excludes the dev, and
 * drops the lvmcache info for the dev.  It would be nicer to catch the issue
 * earlier, before the dev is fully scanned (and populated in lvmcache).  This
 * could be done by checking the devices file for the pvid right after the dev
 * header is read and before scanning more metadata.  label_scan could read the
 * pvid from the pv_header and check it prior to calling _text_read().
 * Currently it's _text_read() that first gets the pvid from the dev, and
 * passes it to lvmcache_add() which sets it in dev->pvid.
 *
 * This function searches devs for missing PVIDs, and for those found
 * updates the du structs (devices file entries) and writes an updated
 * devices file.
 *
 * TODO: should we disable find_renamed_devs entirely when the command
 * is using a non-system devices file?
 */

void device_ids_find_renamed_devs(struct cmd_context *cmd, struct dm_list *dev_list,
				  int *search_count, int noupdate)
{
	struct device *dev;
	struct dev_use *du;
	struct dev_id *id;
	struct dev_iter *iter;
	struct device_list *devl;           /* holds struct device */
	struct device_id_list *dil, *dil2;  /* holds struct device + pvid */
	struct dm_list search_pvids;        /* list of device_id_list */
	struct dm_list search_devs ;        /* list of device_list */
	const char *devname;
	int update_file = 0;
	int other_idtype = 0;
	int other_pvid = 0;
	int no_pvid = 0;
	int found = 0;
	int not_found = 0;
	int search_none;
	int search_auto;

	dm_list_init(&search_pvids);
	dm_list_init(&search_devs);

	if (!cmd->enable_devices_file)
		return;

	search_none = !strcmp(cmd->search_for_devnames, "none");
	search_auto = !strcmp(cmd->search_for_devnames, "auto");

	dm_list_iterate_items(du, &cmd->use_devices) {
		if (!du->pvid)
			continue;
		if (du->idtype != DEV_ID_TYPE_DEVNAME)
			continue;

		/*
		 * if the old incorrect devname is now a device that's
		 * filtered and not scanned, e.g. an mpath component,
		 * then we want to look for the pvid on a new device.
		 */
		if (du->dev && !du->dev->filtered_flags)
			continue;

		if (!(dil = dm_pool_zalloc(cmd->mem, sizeof(*dil))))
			continue;

		if (!search_none) {
			memcpy(dil->pvid, du->pvid, ID_LEN);
			dm_list_add(&search_pvids, &dil->list);
		}
		log_debug("Search for PVID %s.", du->pvid);
		if (search_count)
			(*search_count)++;
	}

	if (dm_list_empty(&search_pvids))
		return;

	/*
	 * A previous command searched for devnames and found nothing, so it
	 * created the searched file to tell us not to bother.  Without this, a
	 * device that's permanently detached (and identified by devname) would
	 * cause every command to search for it.  If the detached device is
	 * later attached, it will generate a pvscan, and pvscan will unlink
	 * the searched file, so a subsequent lvm command will do the search
	 * again.  In future perhaps we could add a policy to automatically
	 * remove a devices file entry that's not been found for some time.
	 *
	 * TODO: like the hint file, add a hash of all devnames to the searched
	 * file so it can be ignored and removed if the devs/hash change.
	 * If hints are enabled, the hints invalidation could also remove the
	 * searched file.
	 */
	if (_searched_devnames_exists(cmd)) {
		log_debug("Search for PVIDs skipped for %s", _searched_file);
		return;
	}

	/*
	 * Now we want to look at devs on the system that were previously
	 * rejected by filter-deviceid (based on a devname device id) to check
	 * if the missing PVID is on a device with a new name.
	 */
	log_debug("Search for PVIDs filtering.");

	/*
	 * Initial list of devs to search, eliminating any that have already
	 * been matched, or don't pass filters that do not read dev.  We do not
	 * want to modify the command's existing filter chain (the persistent
	 * filter), in the process of doing this search outside the deviceid
	 * filter.
	 */
	if (!(iter = dev_iter_create(NULL, 0)))
		return;
	while ((dev = dev_iter_get(cmd, iter))) {
		if (dev->flags & DEV_MATCHED_USE_ID)
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "sysfs"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "type"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "usable"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "mpath"))
			continue;
		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(&search_devs, &devl->list);
	}
	dev_iter_destroy(iter);

	log_debug("Search for PVIDs reading labels on %d devs.", dm_list_size(&search_devs));

	/*
	 * Read the dev to get the pvid, and run the filters that will use the
	 * data that has been read to get the pvid.  Like above, we do not want
	 * to modify the command's existing filter chain or the persistent
	 * filter values.
	 */
	dm_list_iterate_items(devl, &search_devs) {
		int has_pvid;
		dev = devl->dev;

		/*
		 * We only need to check devs that would use ID_TYPE_DEVNAME
		 * themselves as alternatives to the missing ID_TYPE_DEVNAME
		 * entry. i.e. a ID_TYPE_DEVNAME entry would not appear on a
		 * device that has a wwid and would use ID_TYPE_SYS_WWID.  So,
		 * if a dev in the search_devs list has a proper/stable device
		 * id (e.g. wwid, serial, loop, mpath), then we don't need to
		 * read it to check for missing PVIDs.
		 * 
		 * search_for_devnames="all" means we should search every
		 * device, so we skip this optimization.
		 *
		 * TODO: in auto mode should we look in other non-system
		 * devices files and skip any devs included in those?
		 *
		 * Note that a user can override a stable id type and use
		 * devname for a device's id, in which case this optimization
		 * can prevent a search from finding a renamed dev.  So, if a
		 * user forces a devname id, then they should probably also
		 * set search_for_devnames=all.
		 */
		if (search_auto && _dev_has_stable_id(cmd, dev)) {
			other_idtype++;
			continue;
		}

		/*
		 * Reads 4K from the start of the disk.
		 * Returns 0 if the dev cannot be read.
		 * Looks for LVM header, and sets dev->pvid if the device is a PV.
		 * Sets has_pvid=1 if the dev has an lvm PVID.
		 * This loop may look at and skip many non-LVM devices.
		 */
		if (!label_read_pvid(dev, &has_pvid)) {
			no_pvid++;
			continue;
		}

		if (!has_pvid) {
			no_pvid++;
			continue;
		}

		/*
		 * These filters will use the block of data from bcache that
		 * was read label_read_pvid(), and may read other
		 * data blocks beyond that.
		 */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "partitioned"))
			goto next;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "signature"))
			goto next;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "md"))
			goto next;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "fwraid"))
			goto next;

		/*
		 * Check if the the PVID is one we are searching for.
		 * Loop below looks at search_pvid entries that have dil->dev set.
		 * This continues checking after all search_pvids entries have been
		 * matched in order to check if the PVID is on duplicate devs.
		 */
		dm_list_iterate_items_safe(dil, dil2, &search_pvids) {
			if (!memcmp(dil->pvid, dev->pvid, ID_LEN)) {
				if (dil->dev) {
					log_warn("WARNING: found PVID %s on multiple devices %s %s.",
						 dil->pvid, dev_name(dil->dev), dev_name(dev));
					log_warn("WARNING: duplicate PVIDs should be changed to be unique.");
					log_warn("WARNING: use lvmdevices to select a device for PVID %s.", dil->pvid);
					dm_list_del(&dil->list);
				} else {
					log_warn("Devices file PVID %s found on %s.", dil->pvid, dev_name(dev));
					dil->dev = dev;
				}
			} else {
				other_pvid++;
			}
		}
         next:
		label_scan_invalidate(dev);
	}

	log_debug("Search for PVIDs other_pvid %d no_pvid %d other_idtype %d.", other_pvid, no_pvid, other_idtype);

	/*
	 * The use_devices entries (repesenting the devices file) are
	 * updated for the new devices on which the PVs reside.  The new
	 * correct devs are set as dil->dev on search_pvids entries.
	 *
	 * The du/dev/id are set up and linked for the new devs.
	 *
	 * The command's full filter chain is updated for the new devs now that
	 * filter-deviceid will pass.
	 */
	dm_list_iterate_items(dil, &search_pvids) {
		char *dup_devname1, *dup_devname2, *dup_devname3;

		if (!dil->dev || dm_list_empty(&dil->dev->aliases)) {
			not_found++;
			continue;
		}

		dev = dil->dev;
		devname = dev_name(dev);
		found++;

		if (!(du = get_du_for_pvid(cmd, dil->pvid))) {
			/* shouldn't happen */
			continue;
		}
		if (du->idtype != DEV_ID_TYPE_DEVNAME) {
			/* shouldn't happen */
			continue;
		}

		dup_devname1 = strdup(devname);
		dup_devname2 = strdup(devname);
		dup_devname3 = strdup(devname);
		id = zalloc(sizeof(struct dev_id));
		if (!dup_devname1 || !dup_devname2 || !dup_devname3 || !id) {
			free(dup_devname1);
			free(dup_devname2);
			free(dup_devname3);
			free(id);
			stack;
			continue;
		}

		if (!noupdate)
			log_warn("Devices file PVID %s updating IDNAME to %s.", dev->pvid, devname);

		free(du->idname);
		free(du->devname);
		free_dids(&dev->ids);

		du->idname = dup_devname1;
		du->devname = dup_devname2;
		id->idtype = DEV_ID_TYPE_DEVNAME;
		id->idname = dup_devname3;
		id->dev = dev;
		du->dev = dev;
		dev->id = id;
		dev->flags |= DEV_MATCHED_USE_ID;
		dm_list_add(&dev->ids, &id->list);
		dev_get_partition_number(dev, &du->part);
		update_file = 1;
	}

	dm_list_iterate_items(dil, &search_pvids) {
		if (!dil->dev)
			continue;
		dev = dil->dev;

		cmd->filter->wipe(cmd, cmd->filter, dev, NULL);

		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			/* I don't think this would happen */
			log_warn("WARNING: new device %s for PVID %s is excluded: %s.",
				 dev_name(dev), dil->pvid, dev_filtered_reason(dev));
			if (du) /* Should not happen 'du' is NULL */
				du->dev = NULL;
			dev->flags &= ~DEV_MATCHED_USE_ID;
		}
	}

	/*
	 * try lock and device_ids_write(), the update is not required and will
	 * be done by a subsequent command if it's not done here.
	 *
	 * This command could have already done an earlier device_ids_update_try
	 * (successfully or not) in device_ids_validate().
	 */
	if (update_file && noupdate) {
		log_debug("Search for PVIDs update disabled");
	} else if (update_file) {
		log_debug("Search for PVIDs updating devices file");
		_device_ids_update_try(cmd);
	} else {
		log_debug("Search for PVIDs found no updates");
	}

	/*
	 * The entries in search_pvids with a dev set are the new devs found
	 * for the PVIDs that we want to return to the caller in a device_list
	 * format.
	 */
	dm_list_iterate_items(dil, &search_pvids) {
		if (!dil->dev)
			continue;
		dev = dil->dev;

		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(dev_list, &devl->list);
	}

	/*
	 * Prevent more devname searches by subsequent commands, in case the
	 * pvids not found were from devices that are permanently detached.
	 * If a new PV appears, pvscan will run and do unlink_searched_file.
	 */
	if (not_found && !found)
		_touch_searched_devnames(cmd);
}

int devices_file_touch(struct cmd_context *cmd)
{
	struct stat buf;
	char dirpath[PATH_MAX];
	int fd;

	if (dm_snprintf(dirpath, sizeof(dirpath), "%s/devices", cmd->system_dir) < 0) {
		log_error("Failed to copy devices dir path");
		return 0;
	}

	if (stat(dirpath, &buf)) {
		log_error("Cannot create devices file, missing devices directory %s.", dirpath);
		return 0;
	}

	fd = open(cmd->devices_file_path, O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		log_debug("Failed to create %s %d", cmd->devices_file_path, errno);
		return 0;
	}
	if (close(fd))
		stack;
	return 1;
}

int devices_file_exists(struct cmd_context *cmd)
{
	struct stat buf;

	if (!cmd->devices_file_path[0])
		return 0;

	if (stat(cmd->devices_file_path, &buf))
		return 0;

	return 1;
}

/*
 * If a command also uses the global lock, the global lock
 * is acquired first, then the devices file is locked.
 *
 * There are three categories of commands in terms of
 * reading/writing the devices file:
 *
 * 1. Commands that we know intend to modify the file,
 *    lvmdevices --add|--del, vgimportdevices,
 *    pvcreate/vgcreate/vgextend, pvchange --uuid,
 *    vgimportclone.
 *
 * 2. Most other commands that do not modify the file.
 *
 * 3. Commands from 2 that find something to correct in
 *    the devices file during device_ids_validate().
 *    These corrections are not essential and can be
 *    skipped, they will just be done by a subsequent
 *    command if they are not done.
 *
 * Locking for each case:
 *
 * 1. lock ex, read file, write file, unlock
 *
 *    (In general, the command sets edit_devices_file or
 *    create_edit_devices_file, then setup_devices() is called,
 *    maybe directly, or by way of calling the traditional
 *    process_each->label_scan->setup_devices.  setup_devices
 *    sees {create}_edit_devices_file which causes it to do
 *    lock_devices_file(EX) before creating/reading the file.)
 *
 * 2. lock sh, read file, unlock, (validate ok)
 *
 * 3. lock sh, read file, unlock, validate wants update,
 *    lock ex (nonblocking - skip update if fails),
 *    read file, check file is unchanged from prior read,
 *    write file, unlock
 */

static int _lock_devices_file(struct cmd_context *cmd, int mode, int nonblock, int *held)
{
	const char *lock_dir;
	const char *filename;
	int fd;
	int op = mode;
	int ret;

	if (!cmd->enable_devices_file || cmd->nolocking)
		return 1;

	_using_devices_file = 1;

	if (_devices_file_locked == mode) {
		/* can happen when a command holds an ex lock and does an update in device_ids_validate */
		/* can happen when vgimportdevices calls this directly, followed later by setup_devices */
		if (held)
			*held = 1;
		return 1;
	}

	if (_devices_file_locked) {
		/* shouldn't happen */
		log_warn("WARNING: devices file already locked %d", mode);
		return 0;
	}

	if (!(lock_dir = find_config_tree_str(cmd, global_locking_dir_CFG, NULL)))
		return_0;
	if (!(filename = cmd->devicesfile ?: find_config_tree_str(cmd, devices_devicesfile_CFG, NULL)))
		return_0;
	if (dm_snprintf(_devices_lockfile, sizeof(_devices_lockfile), "%s/D_%s", lock_dir, filename) < 0)
		return_0;

	if (nonblock)
		op |= LOCK_NB;

	if (_devices_fd != -1) {
		/* shouldn't happen */
		log_warn("WARNING: devices file lock file already open %d", _devices_fd);
		return 0;
	}

	fd = open(_devices_lockfile, O_CREAT|O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		log_debug("lock_devices_file open errno %d", errno);
		if (cmd->sysinit || cmd->ignorelockingfailure)
			return 1;
		return 0;
	}

	ret = flock(fd, op);
	if (!ret) {
		_devices_fd = fd;
		_devices_file_locked = mode;
		return 1;
	}

	log_debug("lock_devices_file flock errno %d", errno);

	if (close(fd))
		stack;
	if (cmd->sysinit || cmd->ignorelockingfailure)
		return 1;
	return 0;
}

int lock_devices_file(struct cmd_context *cmd, int mode)
{
	return _lock_devices_file(cmd, mode, 0, NULL);
}

int lock_devices_file_try(struct cmd_context *cmd, int mode, int *held)
{
	return _lock_devices_file(cmd, mode, 1, held);
}

void unlock_devices_file(struct cmd_context *cmd)
{
	int ret;

	if (!cmd->enable_devices_file || cmd->nolocking || !_using_devices_file)
		return;

	if (!_devices_file_locked && cmd->sysinit)
		return;

	if (_devices_fd == -1) {
		/* shouldn't happen */
		log_warn("WARNING: devices file unlock no fd");
		return;
	}

	if (!_devices_file_locked)
		log_warn("WARNING: devices file unlock not locked");

	ret = flock(_devices_fd, LOCK_UN);
	if (ret)
		log_warn("WARNING: devices file unlock errno %d", errno);

	_devices_file_locked = 0;

	if (close(_devices_fd))
		stack;
	_devices_fd = -1;
}

void devices_file_init(struct cmd_context *cmd)
{
	dm_list_init(&cmd->use_devices);
	dm_list_init(&cmd->device_ids_check_serial);
}

void devices_file_exit(struct cmd_context *cmd)
{
	if (!cmd->enable_devices_file)
		return;
	free_dus(&cmd->use_devices);
	if (_devices_fd == -1)
		return;
	unlock_devices_file(cmd);
}

