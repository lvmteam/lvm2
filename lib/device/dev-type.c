/*
 * Copyright (C) 2013 Red Hat, Inc. All rights reserved.
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
#include "dev-type.h"
#include "xlate.h"
#include "config.h"
#include "metadata.h"

#include <libgen.h>
#include <ctype.h>

#include "device-types.h"

struct dev_types *create_dev_types(const char *proc_dir,
				   const struct dm_config_node *cn)
{
	struct dev_types *dt;
	char line[80];
	char proc_devices[PATH_MAX];
	FILE *pd = NULL;
	int i, j = 0;
	int line_maj = 0;
	int blocksection = 0;
	size_t dev_len = 0;
	const struct dm_config_value *cv;
	const char *name;
	char *nl;

	if (!(dt = dm_zalloc(sizeof(struct dev_types)))) {
		log_error("Failed to allocate device type register.");
		return NULL;
	}

	if (!*proc_dir) {
		log_verbose("No proc filesystem found: using all block device types");
		for (i = 0; i < NUMBER_OF_MAJORS; i++)
			dt->dev_type_array[i].max_partitions = 1;
		return dt;
	}

	if (dm_snprintf(proc_devices, sizeof(proc_devices),
			 "%s/devices", proc_dir) < 0) {
		log_error("Failed to create /proc/devices string");
		goto bad;
	}

	if (!(pd = fopen(proc_devices, "r"))) {
		log_sys_error("fopen", proc_devices);
		goto bad;
	}

	while (fgets(line, sizeof(line), pd) != NULL) {
		i = 0;
		while (line[i] == ' ')
			i++;

		/* If it's not a number it may be name of section */
		line_maj = atoi(((char *) (line + i)));

		if (line_maj < 0 || line_maj >= NUMBER_OF_MAJORS) {
			/*
			 * Device numbers shown in /proc/devices are actually direct
			 * numbers passed to registering function, however the kernel
			 * uses only 12 bits, so use just 12 bits for major.
			 */
			if ((nl = strchr(line, '\n'))) *nl = '\0';
			log_warn("WARNING: /proc/devices line: %s, replacing major with %d.",
				 line, line_maj & (NUMBER_OF_MAJORS - 1));
			line_maj &= (NUMBER_OF_MAJORS - 1);
		}

		if (!line_maj) {
			blocksection = (line[i] == 'B') ? 1 : 0;
			continue;
		}

		/* We only want block devices ... */
		if (!blocksection)
			continue;

		/* Find the start of the device major name */
		while (line[i] != ' ' && line[i] != '\0')
			i++;
		while (line[i] == ' ')
			i++;

		/* Look for md device */
		if (!strncmp("md", line + i, 2) && isspace(*(line + i + 2)))
			dt->md_major = line_maj;

		/* Look for blkext device */
		if (!strncmp("blkext", line + i, 6) && isspace(*(line + i + 6)))
			dt->blkext_major = line_maj;

		/* Look for drbd device */
		if (!strncmp("drbd", line + i, 4) && isspace(*(line + i + 4)))
			dt->drbd_major = line_maj;

		/* Look for EMC powerpath */
		if (!strncmp("emcpower", line + i, 8) && isspace(*(line + i + 8)))
			dt->emcpower_major = line_maj;

		if (!strncmp("power2", line + i, 6) && isspace(*(line + i + 6)))
			dt->power2_major = line_maj;

		/* Look for device-mapper device */
		/* FIXME Cope with multiple majors */
		if (!strncmp("device-mapper", line + i, 13) && isspace(*(line + i + 13)))
			dt->device_mapper_major = line_maj;

		/* Major is SCSI device */
		if (!strncmp("sd", line + i, 2) && isspace(*(line + i + 2)))
			dt->dev_type_array[line_maj].flags |= PARTITION_SCSI_DEVICE;

		/* Go through the valid device names and if there is a
		   match store max number of partitions */
		for (j = 0; _dev_known_types[j].name[0]; j++) {
			dev_len = strlen(_dev_known_types[j].name);
			if (dev_len <= strlen(line + i) &&
			    !strncmp(_dev_known_types[j].name, line + i, dev_len) &&
			    (line_maj < NUMBER_OF_MAJORS)) {
				dt->dev_type_array[line_maj].max_partitions =
					_dev_known_types[j].max_partitions;
				break;
			}
		}

		if (!cn)
			continue;

		/* Check devices/types for local variations */
		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Expecting string in devices/types "
					  "in config file");
				if (fclose(pd))
					log_sys_error("fclose", proc_devices);
				goto bad;
			}
			dev_len = strlen(cv->v.str);
			name = cv->v.str;
			cv = cv->next;
			if (!cv || cv->type != DM_CFG_INT) {
				log_error("Max partition count missing for %s "
					  "in devices/types in config file",
					  name);
				if (fclose(pd))
					log_sys_error("fclose", proc_devices);
				goto bad;
			}
			if (!cv->v.i) {
				log_error("Zero partition count invalid for "
					  "%s in devices/types in config file",
					  name);
				if (fclose(pd))
					log_sys_error("fclose", proc_devices);
				goto bad;
			}
			if (dev_len <= strlen(line + i) &&
			    !strncmp(name, line + i, dev_len) &&
			    (line_maj < NUMBER_OF_MAJORS)) {
				dt->dev_type_array[line_maj].max_partitions = cv->v.i;
				break;
			}
		}
	}

	if (fclose(pd))
		log_sys_error("fclose", proc_devices);

	return dt;
bad:
	dm_free(dt);
	return NULL;
}

int dev_subsystem_part_major(struct dev_types *dt, struct device *dev)
{
	dev_t primary_dev;

	if (MAJOR(dev->dev) == dt->device_mapper_major)
		return 1;

	if (MAJOR(dev->dev) == dt->drbd_major)
		return 1;

	if (MAJOR(dev->dev) == dt->emcpower_major)
		return 1;

	if (MAJOR(dev->dev) == dt->power2_major)
		return 1;

	if ((MAJOR(dev->dev) == dt->blkext_major) &&
	    (dev_get_primary_dev(dt, dev, &primary_dev) > 0) &&
	    (MAJOR(primary_dev) == dt->md_major))
		return 1;

	return 0;
}

const char *dev_subsystem_name(struct dev_types *dt, struct device *dev)
{
	if (MAJOR(dev->dev) == dt->md_major)
		return "MD";

	if (MAJOR(dev->dev) == dt->drbd_major)
		return "DRBD";

	if (MAJOR(dev->dev) == dt->emcpower_major)
		return "EMCPOWER";

	if (MAJOR(dev->dev) == dt->power2_major)
		return "POWER2";

	if (MAJOR(dev->dev) == dt->blkext_major)
		return "BLKEXT";

	return "";
}

int major_max_partitions(struct dev_types *dt, int major)
{
	if (major >= NUMBER_OF_MAJORS)
		return 0;

	return dt->dev_type_array[major].max_partitions;
}

int major_is_scsi_device(struct dev_types *dt, int major)
{
	if (major >= NUMBER_OF_MAJORS)
		return 0;

	return (dt->dev_type_array[major].flags & PARTITION_SCSI_DEVICE) ? 1 : 0;
}

/* See linux/genhd.h and fs/partitions/msdos */
#define PART_MAGIC 0xAA55
#define PART_MAGIC_OFFSET UINT64_C(0x1FE)
#define PART_OFFSET UINT64_C(0x1BE)

struct partition {
	uint8_t boot_ind;
	uint8_t head;
	uint8_t sector;
	uint8_t cyl;
	uint8_t sys_ind;	/* partition type */
	uint8_t end_head;
	uint8_t end_sector;
	uint8_t end_cyl;
	uint32_t start_sect;
	uint32_t nr_sects;
} __attribute__((packed));

static int _is_partitionable(struct dev_types *dt, struct device *dev)
{
	int parts = major_max_partitions(dt, MAJOR(dev->dev));

	/* All MD devices are partitionable via blkext (as of 2.6.28) */
	if (MAJOR(dev->dev) == dt->md_major)
		return 1;

	if ((parts <= 1) || (MINOR(dev->dev) % parts))
		return 0;

	return 1;
}

static int _has_partition_table(struct device *dev)
{
	int ret = 0;
	unsigned p;
	struct {
		uint8_t skip[PART_OFFSET];
		struct partition part[4];
		uint16_t magic;
	} __attribute__((packed)) buf; /* sizeof() == SECTOR_SIZE */

	if (!dev_read(dev, UINT64_C(0), sizeof(buf), &buf))
		return_0;

	/* FIXME Check for other types of partition table too */

	/* Check for msdos partition table */
	if (buf.magic == xlate16(PART_MAGIC)) {
		for (p = 0; p < 4; ++p) {
			/* Table is invalid if boot indicator not 0 or 0x80 */
			if (buf.part[p].boot_ind & 0x7f) {
				ret = 0;
				break;
			}
			/* Must have at least one non-empty partition */
			if (buf.part[p].nr_sects)
				ret = 1;
		}
	}

	return ret;
}

int dev_is_partitioned(struct dev_types *dt, struct device *dev)
{
	if (!_is_partitionable(dt, dev))
		return 0;

	return _has_partition_table(dev);
}

/*
 * Get primary dev for the dev supplied.
 * Returns:
 *   0 if the dev is already a primary dev
 *   1 if the dev is a partition, primary dev in result
 *   -1 on error
 */
int dev_get_primary_dev(struct dev_types *dt, struct device *dev, dev_t *result)
{
	const char *sysfs_dir = dm_sysfs_dir();
	int major = (int) MAJOR(dev->dev);
	int minor = (int) MINOR(dev->dev);
	char path[PATH_MAX+1];
	char temp_path[PATH_MAX+1];
	char buffer[64];
	struct stat info;
	FILE *fp = NULL;
	int parts, residue, size, ret = -1;

	/*
	 * Try to get the primary dev out of the
	 * list of known device types first.
	 */
	if ((parts = dt->dev_type_array[major].max_partitions) > 1) {
		if ((residue = minor % parts)) {
			*result = MKDEV((dev_t)major, (minor - residue));
			ret = 1;
		} else {
			*result = dev->dev;
			ret = 0; /* dev is not a partition! */
		}
		goto out;
	}

	/*
	 * If we can't get the primary dev out of the list of known device
	 * types, try to look at sysfs directly then. This is more complex
	 * way and it also requires certain sysfs layout to be present
	 * which might not be there in old kernels!
	 */

	/* check if dev is a partition */
	if (dm_snprintf(path, PATH_MAX, "%s/dev/block/%d:%d/partition",
			sysfs_dir, major, minor) < 0) {
		log_error("dm_snprintf partition failed");
		goto out;
	}

	if (stat(path, &info) == -1) {
		if (errno != ENOENT)
			log_sys_error("stat", path);
		*result = dev->dev;
		ret = 0; goto out; /* dev is not a partition! */

	}

	/*
	 * extract parent's path from the partition's symlink, e.g.:
	 * - readlink /sys/dev/block/259:0 = ../../block/md0/md0p1
	 * - dirname ../../block/md0/md0p1 = ../../block/md0
	 * - basename ../../block/md0/md0  = md0
	 * Parent's 'dev' sysfs attribute  = /sys/block/md0/dev
	 */
	if ((size = readlink(dirname(path), temp_path, PATH_MAX)) < 0) {
		log_sys_error("readlink", path);
		goto out;
	}

	temp_path[size] = '\0';

	if (dm_snprintf(path, PATH_MAX, "%s/block/%s/dev",
			sysfs_dir, basename(dirname(temp_path))) < 0) {
		log_error("dm_snprintf dev failed");
		goto out;
	}

	/* finally, parse 'dev' attribute and create corresponding dev_t */
	if (stat(path, &info) == -1) {
		if (errno == ENOENT)
			log_error("sysfs file %s does not exist", path);
		else
			log_sys_error("stat", path);
		goto out;
	}

	fp = fopen(path, "r");
	if (!fp) {
		log_sys_error("fopen", path);
		goto out;
	}

	if (!fgets(buffer, sizeof(buffer), fp)) {
		log_sys_error("fgets", path);
		goto out;
	}

	if (sscanf(buffer, "%d:%d", &major, &minor) != 2) {
		log_error("sysfs file %s not in expected MAJ:MIN format: %s",
			  path, buffer);
		goto out;
	}
	*result = MKDEV((dev_t)major, minor);
	ret = 1;
out:
	if (fp && fclose(fp))
		log_sys_error("fclose", path);

	return ret;
}

#if 0
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

#include <errno.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/genhd.h>

int _get_partition_type(struct dev_filter *filter, struct device *d);

#define MINOR_PART(dev) (MINOR((dev)->dev) % max_partitions(MINOR((dev)->dev)))

int is_extended_partition(struct device *d)
{
	return (MINOR_PART(d) > 4) ? 1 : 0;
}

struct device *dev_primary(struct dev_mgr *dm, struct device *d)
{
	struct device *ret;

	ret = dev_by_dev(dm, d->dev - MINOR_PART(dm, d));
	/* FIXME: Needs replacing with a 'refresh' */
	if (!ret) {
		init_dev_scan(dm);
		ret = dev_by_dev(dm, d->dev - MINOR_PART(dm, d));
	}

	return ret;

}

int partition_type_is_lvm(struct dev_mgr *dm, struct device *d)
{
	int pt;

	pt = _get_partition_type(dm, d);

	if (!pt) {
		if (is_whole_disk(dm, d))
			/* FIXME: Overloaded pt=0 in error cases */
			return 1;
		else {
			log_error
			    ("%s: missing partition table "
			     "on partitioned device", d->name);
			return 0;
		}
	}

	if (is_whole_disk(dm, d)) {
		log_error("%s: looks to possess partition table", d->name);
		return 0;
	}

	/* check part type */
	if (pt != LVM_PARTITION && pt != LVM_NEW_PARTITION) {
		log_error("%s: invalid partition type 0x%x "
			  "(must be 0x%x)", d->name, pt, LVM_NEW_PARTITION);
		return 0;
	}

	if (pt == LVM_PARTITION) {
		log_error
		    ("%s: old LVM partition type found - please change to 0x%x",
		     d->name, LVM_NEW_PARTITION);
		return 0;
	}

	return 1;
}

int _get_partition_type(struct dev_mgr *dm, struct device *d)
{
	int pv_handle = -1;
	struct device *primary;
	ssize_t read_ret;
	ssize_t bytes_read = 0;
	char *buffer;
	unsigned short *s_buffer;
	struct partition *part;
	loff_t offset = 0;
	loff_t extended_offset = 0;
	int part_sought;
	int part_found = 0;
	int first_partition = 1;
	int extended_partition = 0;
	int p;

	if (!(primary = dev_primary(dm, d))) {
		log_error
		    ("Failed to find main device containing partition %s",
		     d->name);
		return 0;
	}

	if (!(buffer = dm_malloc(SECTOR_SIZE))) {
		log_error("Failed to allocate partition table buffer");
		return 0;
	}

	/* Get partition table */
	if ((pv_handle = open(primary->name, O_RDONLY)) < 0) {
		log_error("%s: open failed: %s", primary->name,
			  strerror(errno));
		return 0;
	}

	s_buffer = (unsigned short *) buffer;
	part = (struct partition *) (buffer + 0x1be);
	part_sought = MINOR_PART(dm, d);

	do {
		bytes_read = 0;

		if (llseek(pv_handle, offset * SECTOR_SIZE, SEEK_SET) == -1) {
			log_error("%s: llseek failed: %s",
				  primary->name, strerror(errno));
			return 0;
		}

		while ((bytes_read < SECTOR_SIZE) &&
		       (read_ret =
			read(pv_handle, buffer + bytes_read,
			     SECTOR_SIZE - bytes_read)) != -1)
			bytes_read += read_ret;

		if (read_ret == -1) {
			log_error("%s: read failed: %s", primary->name,
				  strerror(errno));
			return 0;
		}

		if (s_buffer[255] == 0xAA55) {
			if (is_whole_disk(dm, d))
				return -1;
		} else
			return 0;

		extended_partition = 0;

		/* Loop through primary partitions */
		for (p = 0; p < 4; p++) {
			if (part[p].sys_ind == DOS_EXTENDED_PARTITION ||
			    part[p].sys_ind == LINUX_EXTENDED_PARTITION
			    || part[p].sys_ind == WIN98_EXTENDED_PARTITION) {
				extended_partition = 1;
				offset = extended_offset + part[p].start_sect;
				if (extended_offset == 0)
					extended_offset = part[p].start_sect;
				if (first_partition == 1)
					part_found++;
			} else if (first_partition == 1) {
				if (p == part_sought) {
					if (part[p].sys_ind == 0) {
						/* missing primary? */
						return 0;
					}
				} else
					part_found++;
			} else if (!part[p].sys_ind)
				part_found++;

			if (part_sought == part_found)
				return part[p].sys_ind;

		}
		first_partition = 0;
	}
	while (extended_partition == 1);

	return 0;
}
#endif

#ifdef linux

static unsigned long _dev_topology_attribute(struct dev_types *dt,
					     const char *attribute,
					     struct device *dev)
{
	const char *sysfs_dir = dm_sysfs_dir();
	static const char sysfs_fmt_str[] = "%s/dev/block/%d:%d/%s";
	char path[PATH_MAX+1], buffer[64];
	FILE *fp;
	struct stat info;
	dev_t uninitialized_var(primary);
	unsigned long result = 0UL;

	if (!attribute || !*attribute)
		return_0;

	if (!sysfs_dir || !*sysfs_dir)
		return_0;

	if (dm_snprintf(path, PATH_MAX, sysfs_fmt_str, sysfs_dir,
			(int)MAJOR(dev->dev), (int)MINOR(dev->dev),
			attribute) < 0) {
		log_error("dm_snprintf %s failed", attribute);
		return 0;
	}

	/*
	 * check if the desired sysfs attribute exists
	 * - if not: either the kernel doesn't have topology support
	 *   or the device could be a partition
	 */
	if (stat(path, &info) == -1) {
		if (errno != ENOENT) {
			log_sys_error("stat", path);
			return 0;
		}
		if (dev_get_primary_dev(dt, dev, &primary) < 0)
			return 0;

		/* get attribute from partition's primary device */
		if (dm_snprintf(path, PATH_MAX, sysfs_fmt_str, sysfs_dir,
				(int)MAJOR(primary), (int)MINOR(primary),
				attribute) < 0) {
			log_error("primary dm_snprintf %s failed", attribute);
			return 0;
		}
		if (stat(path, &info) == -1) {
			if (errno != ENOENT)
				log_sys_error("stat", path);
			return 0;
		}
	}

	if (!(fp = fopen(path, "r"))) {
		log_sys_error("fopen", path);
		return 0;
	}

	if (!fgets(buffer, sizeof(buffer), fp)) {
		log_sys_error("fgets", path);
		goto out;
	}

	if (sscanf(buffer, "%lu", &result) != 1) {
		log_error("sysfs file %s not in expected format: %s", path,
			  buffer);
		goto out;
	}

	log_very_verbose("Device %s %s is %lu bytes.",
			 dev_name(dev), attribute, result);

out:
	if (fclose(fp))
		log_sys_error("fclose", path);

	return result >> SECTOR_SHIFT;
}

unsigned long dev_alignment_offset(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute(dt, "alignment_offset", dev);
}

unsigned long dev_minimum_io_size(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute(dt, "queue/minimum_io_size", dev);
}

unsigned long dev_optimal_io_size(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute(dt, "queue/optimal_io_size", dev);
}

unsigned long dev_discard_max_bytes(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute(dt, "queue/discard_max_bytes", dev);
}

unsigned long dev_discard_granularity(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute(dt, "queue/discard_granularity", dev);
}

#else

int get_primary_dev(struct device *dev, dev_t *result)
{
	return 0;
}

unsigned long dev_alignment_offset(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

unsigned long dev_minimum_io_size(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

unsigned long dev_optimal_io_size(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

unsigned long dev_discard_max_bytes(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

unsigned long dev_discard_granularity(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

#endif
