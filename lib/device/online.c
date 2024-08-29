/*
 * Copyright (C) 2021 Red Hat, Inc. All rights reserved.
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
#include "lib/device/online.h"

#include <dirent.h>

/*
 * file contains:
 * <major>:<minor>\n
 * vg:<vgname>\n
 * dev:<devname>\n\0
 *
 * It's possible that vg and dev may not exist.
 */

static int _copy_pvid_file_field(const char *field, char *buf, int bufsize, char *out, int outsize)
{
	char *p;
	int i = 0;
	
	if (!(p = strstr(buf, field)))
		return 0;

	p += strlen(field);

	while (1) {
		if (*p == '\n')
			break;
		if (*p == '\0')
			break;

		if (p >= (buf + bufsize))
			return 0;
		if (i >= outsize-1)
			return 0;

		out[i] = *p;

		i++;
		p++;
	}

	return i ? 1 : 0;
}

#define MAX_PVID_FILE_SIZE 512

int online_pvid_file_read(char *path, unsigned *major, unsigned *minor, char *vgname, char *devname)
{
	char buf[MAX_PVID_FILE_SIZE] = { 0 };
	int fd, rv;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		log_warn("WARNING: Failed to open %s.", path);
		return 0;
	}

	rv = read(fd, buf, sizeof(buf) - 1);
	if (close(fd))
		log_sys_debug("close", path);
	if (!rv || rv < 0) {
		log_warn("WARNING: No info in %s.", path);
		return 0;
	}
	buf[rv] = 0; /* \0 terminated buffer */

	if (sscanf(buf, "%u:%u", major, minor) != 2) {
		log_warn("WARNING: No device numbers in %s.", path);
		return 0;
	}

	if (vgname) {
		if (!strstr(buf, "vg:")) {
			log_debug("No vgname in %s", path);
			vgname[0] = '\0';
			goto copy_dev;
		}

		if (!_copy_pvid_file_field("vg:", buf, MAX_PVID_FILE_SIZE, vgname, NAME_LEN)) {
			log_warn("WARNING: Ignoring invalid vg field in %s.", path);
			vgname[0] = '\0';
			goto copy_dev;
		}

		if (!validate_name(vgname)) {
			log_warn("WARNING: Ignoring invalid vgname in %s (%s).", path, vgname);
			vgname[0] = '\0';
			goto copy_dev;
		}
	}

 copy_dev:
	if (devname) {
		if (!strstr(buf, "dev:")) {
			log_debug("No devname in %s", path);
			devname[0] = '\0';
			goto out;
		}

		if (!_copy_pvid_file_field("dev:", buf, MAX_PVID_FILE_SIZE, devname, NAME_LEN)) {
			log_warn("WARNING: Ignoring invalid devname field in %s.", path);
			devname[0] = '\0';
			goto out;
		}

		if (strncmp(devname, "/dev/", 5)) {
			log_warn("WARNING: Ignoring invalid devname in %s (%s).", path, devname);
			devname[0] = '\0';
			goto out;
		}
	}
 out:
	return 1;
}

void free_po_list(struct dm_list *list)
{
	struct pv_online *po, *po2;

	dm_list_iterate_items_safe(po, po2, list) {
		dm_list_del(&po->list);
		free(po);
	}
}

int get_pvs_online(struct dm_list *pvs_online, const char *vgname)
{
	char path[PATH_MAX];
	char file_vgname[NAME_LEN];
	char file_devname[NAME_LEN];
	DIR *dir;
	struct dirent *de;
	struct pv_online *po;
	unsigned file_major, file_minor;

	if (!(dir = opendir(PVS_ONLINE_DIR)))
		return 0;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		if (strlen(de->d_name) != ID_LEN)
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, de->d_name);

		file_major = 0;
		file_minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));
		memset(file_devname, 0, sizeof(file_devname));

		if (!online_pvid_file_read(path, &file_major, &file_minor, file_vgname, file_devname))
			continue;

		if (vgname && strcmp(file_vgname, vgname))
			continue;

		if (!(po = zalloc(sizeof(*po))))
			continue;

		memcpy(po->pvid, de->d_name, ID_LEN);
		if (file_major || file_minor)
			po->devno = MKDEV(file_major, file_minor);
		if (file_vgname[0])
			dm_strncpy(po->vgname, file_vgname, sizeof(po->vgname));
		if (file_devname[0])
			dm_strncpy(po->devname, file_devname, sizeof(po->devname));

		log_debug("Found PV online %s for VG %s %s", path, vgname, file_devname);
		dm_list_add(pvs_online, &po->list);
	}

	if (closedir(dir))
		log_sys_debug("closedir", PVS_ONLINE_DIR);

	log_debug("Found PVs online %d for %s", dm_list_size(pvs_online), vgname ?: "all");

	return 1;
}

/*
 * When a PV goes offline, remove the vg online file for that VG
 * (even if other PVs for the VG are still online).  This means
 * that the vg will be activated again when it becomes complete.
 */

void online_vg_file_remove(const char *vgname)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s", VGS_ONLINE_DIR, vgname) < 0) {
		log_debug("Path %s/%s is too long.", VGS_ONLINE_DIR, vgname);
		return;
	}

	log_debug("Unlink vg online: %s", path);

	if (unlink(path) && (errno != ENOENT))
		log_sys_debug("unlink", path);
}

int online_vg_file_create(struct cmd_context *cmd, const char *vgname)
{
	char path[PATH_MAX];
	int fd;

	if (dm_snprintf(path, sizeof(path), "%s/%s", VGS_ONLINE_DIR, vgname) < 0) {
		log_error_pvscan(cmd, "Path %s/%s is too long.", VGS_ONLINE_DIR, vgname);
		return 0;
	}

	log_debug("Create vg online: %s", path);

	fd = open(path, O_CREAT | O_EXCL | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		log_debug("Failed to create %s: %d", path, errno);
		return 0;
	}

	/* We don't care about syncing, these files are not even persistent. */

	if (close(fd))
		log_sys_debug("close", path);

	return 1;
}

int online_pvid_file_create(struct cmd_context *cmd, struct device *dev, const char *vgname)
{
	char path[PATH_MAX];
	char buf[MAX_PVID_FILE_SIZE] = { 0 };
	char file_vgname[NAME_LEN];
	char file_devname[NAME_LEN];
	char devname[NAME_LEN];
	int devnamelen;
	unsigned file_major = 0, file_minor = 0;
	unsigned major, minor;
	int fd;
	int rv;
	int len;
	int len1 = 0;
	int len2 = 0;
	int len3 = 0;

	major = MAJOR(dev->dev);
	minor = MINOR(dev->dev);

	if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, dev->pvid) < 0) {
		log_error_pvscan(cmd, "Path %s/%s is too long.", PVS_ONLINE_DIR, dev->pvid);
		return 0;
	}

	if ((len1 = dm_snprintf(buf, sizeof(buf), "%u:%u\n", major, minor)) < 0) {
		log_error_pvscan(cmd, "Cannot create online file path for %s %u:%u.", dev_name(dev), major, minor);
		return 0;
	}

	if (vgname) {
		if ((len2 = dm_snprintf(buf + len1, sizeof(buf) - len1, "vg:%s\n", vgname)) < 0) {
			log_print_unless_silent("Incomplete online file for %s %d:%d vg %s.", dev_name(dev), major, minor, vgname);
			/* can still continue without vgname */
			len2 = 0;
		}
	}

	devnamelen = dm_snprintf(devname, sizeof(devname), "%s", dev_name(dev));
	if ((devnamelen > 5) && (devnamelen < NAME_LEN-1)) {
		if ((len3 = dm_snprintf(buf + len1 + len2, sizeof(buf) - len1 - len2, "dev:%s\n", devname)) < 0) {
			log_print_unless_silent("Incomplete devname in online file for %s.", dev_name(dev));
			/* can continue without devname */
			len3 = 0;
		}
	}

	len = len1 + len2 + len3;

	log_debug("Create pv online: %s %u:%u %s.", path, major, minor, dev_name(dev));

	fd = open(path, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		if (errno == EEXIST)
			goto check_duplicate;
		log_error_pvscan(cmd, "Failed to create online file for %s path %s error %d", dev_name(dev), path, errno);
		return 0;
	}

	while (len > 0) {
		rv = write(fd, buf, len);
		if (rv < 0) {
			/* file exists so it still works in part */
			log_warn("Cannot write online file for %s to %s error %d",
				  dev_name(dev), path, errno);
			if (close(fd))
				log_sys_debug("close", path);
			return 1;
		}
		len -= rv;
	}

	/* We don't care about syncing, these files are not even persistent. */

	if (close(fd))
		log_sys_debug("close", path);

	return 1;

check_duplicate:

	/*
	 * If a PVID online file already exists for this PVID, check if the
	 * file contains a different device number, and if so we may have a
	 * duplicate PV.
	 *
	 * FIXME: disable autoactivation of the VG somehow?
	 * The VG may or may not already be activated when a duplicate appears.
	 * Perhaps write a new field in the pv online or vg online file?
	 */

	memset(file_vgname, 0, sizeof(file_vgname));
	memset(file_devname, 0, sizeof(file_devname));

	online_pvid_file_read(path, &file_major, &file_minor, file_vgname, file_devname);

	if ((file_major == major) && (file_minor == minor)) {
		log_debug("Existing online file for %d:%d", major, minor);
		return 1;
	}

	/* Don't know how vgname might not match, but it's not good so fail. */

	if ((file_major != major) || (file_minor != minor))
		log_error_pvscan(cmd, "PV %s %d:%d is duplicate for PVID %s on %d:%d %s.",
			         dev_name(dev), major, minor, dev->pvid, file_major, file_minor, file_devname);

	if (file_vgname[0] && vgname && strcmp(file_vgname, vgname))
		log_error_pvscan(cmd, "PV %s has unexpected VG %s vs %s.",
			         dev_name(dev), vgname, file_vgname);

	return 0;
}

int online_pvid_file_exists(const char *pvid)
{
	char path[PATH_MAX] = { 0 };
	struct stat buf;
	int rv;

	if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, pvid) < 0) {
		log_debug(INTERNAL_ERROR "Path %s/%s is too long.", PVS_ONLINE_DIR, pvid);
		return 0;
	}

	log_debug("Check pv online: %s", path);

	rv = stat(path, &buf);
	if (!rv) {
		log_debug("Check pv online %s: yes", pvid);
		return 1;
	}
	log_debug("Check pv online %s: no", pvid);
	return 0;
}

int get_pvs_lookup(struct dm_list *pvs_online, const char *vgname)
{
	char lookup_path[PATH_MAX] = { 0 };
	char path[PATH_MAX] = { 0 };
	char line[64];
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	char file_vgname[NAME_LEN];
	char file_devname[NAME_LEN];
	struct pv_online *po;
	unsigned file_major, file_minor;
	FILE *fp;

	if (dm_snprintf(lookup_path, sizeof(lookup_path), "%s/%s", PVS_LOOKUP_DIR, vgname) < 0)
		return_0;

	if (!(fp = fopen(lookup_path, "r")))
		return_0;

	while (fgets(line, sizeof(line), fp)) {
		memcpy(pvid, line, ID_LEN);
		if (strlen(pvid) != ID_LEN)
			goto_bad;

		snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, pvid);

		file_major = 0;
		file_minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));
		memset(file_devname, 0, sizeof(file_devname));

		if (!online_pvid_file_read(path, &file_major, &file_minor, file_vgname, file_devname))
			goto_bad;

		/*
		 * PVs without metadata will not have a vgname in their pvid
		 * file, but the purpose of using the lookup file is that we
		 * know the PV is for this VG even without the pvid vgname
		 * field.
		 */
		if (vgname && file_vgname[0] && strcmp(file_vgname, vgname)) {
			/* Should never happen */
			log_error("Incorrect VG lookup file %s PVID %s %s.", vgname, pvid, file_vgname);
			goto bad;
		}

		if (!(po = zalloc(sizeof(*po))))
			goto_bad;

		memcpy(po->pvid, pvid, ID_LEN);
		if (file_major || file_minor)
			po->devno = MKDEV(file_major, file_minor);
		if (file_vgname[0])
			dm_strncpy(po->vgname, file_vgname, sizeof(po->vgname));
		if (file_devname[0])
			dm_strncpy(po->devname, file_devname, sizeof(po->devname));

		log_debug("Found PV online lookup %s for VG %s on %s.", path, vgname, file_devname);
		dm_list_add(pvs_online, &po->list);
	}

	log_debug("Found PVs online lookup %d for %s.", dm_list_size(pvs_online), vgname);

	if (fclose(fp))
		log_sys_debug("fclose", lookup_path);

	return 1;

bad:
	free_po_list(pvs_online);
	if (fclose(fp))
		log_sys_debug("fclose", lookup_path);

	return 0;
}

void online_dir_setup(struct cmd_context *cmd)
{
	if (!dir_create_recursive(PVS_ONLINE_DIR, 0755))
		stack;
	if (!dir_create_recursive(VGS_ONLINE_DIR, 0755))
		stack;
	if (!dir_create_recursive(PVS_LOOKUP_DIR, 0755))
		stack;
}

void online_lookup_file_remove(const char *vgname)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_LOOKUP_DIR, vgname) < 0) {
		log_debug("Path %s/%s is too long.", PVS_LOOKUP_DIR, vgname);
		return;
	}

	log_debug("Unlink pvs_lookup: %s", path);

	if (unlink(path) && (errno != ENOENT))
		log_sys_debug("unlink", path);
}

static int _online_pvid_file_remove(char *pvid)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, pvid) < 0)
		return_0;
	if (!unlink(path))
		return 1;
	return 0;
}

/*
 * Reboot automatically clearing tmpfs on /run is the main method of removing
 * online files.  It's important to note that removing the online files for a
 * VG is not a technical requirement for anything and could easily be skipped
 * if it had any downside.  It's only done to clean up the space used in /run
 * by the online files, e.g. if there happens to be an extreme amount of
 * vgcreate/pvscan/vgremove between reboots that are leaving a large number of
 * useless online files consuming tmpfs space.
 */
void online_vgremove(struct volume_group *vg)
{
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct pv_list *pvl;

	/*
	 * online files may not exist for the vg if there has been no
	 * pvscans or autoactivation.
	 */

	online_vg_file_remove(vg->name);
	online_lookup_file_remove(vg->name);

	dm_list_iterate_items(pvl, &vg->pvs) {
		memcpy(pvid, &pvl->pv->id.uuid, ID_LEN);
		_online_pvid_file_remove(pvid);
	}
}

