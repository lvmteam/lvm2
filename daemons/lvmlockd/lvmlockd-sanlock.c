/*
 * Copyright (C) 2014-2015 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 */

#define _XOPEN_SOURCE 500  /* pthread */
#define _ISOC99_SOURCE

#include "tools/tool.h"

#include "libdaemon/server/daemon-server.h"
#include "lib/mm/xlate.h"

#include "lvmlockd-internal.h"
#include "daemons/lvmlockd/lvmlockd-client.h"

#include "sanlock.h"
#include "sanlock_rv.h"
#include "sanlock_admin.h"
#include "sanlock_resource.h"

/* FIXME: copied from sanlock header until the sanlock update is more widespread */
#define SANLK_ADD_NODELAY      0x00000002
/* FIXME: copied from sanlock header until the sanlock update is more widespread */
#define SANLK_GET_HOST_LOCAL   0x00000001

#include <stddef.h>
#include <poll.h>
#include <errno.h>
#include <syslog.h>
#include <sys/sysmacros.h>

#define ONE_MB 1048576

/*
-------------------------------------------------------------------------------
For each VG, lvmlockd creates a sanlock lockspace that holds the leases for
that VG.  There's a lease for the VG lock, and there's a lease for each active
LV.  sanlock maintains (reads/writes) these leases, which exist on storage.
That storage is a hidden LV within the VG: /dev/vg/lvmlock.  lvmlockd gives the
path of this internal LV to sanlock, which then reads/writes the leases on it.

# lvs -a cc -o+uuid
  LV        VG   Attr       LSize   LV UUID
  lv1       cc   -wi-a-----   2.00g 7xoDtu-yvNM-iwQx-C94t-BbYs-UzBl-o8hAIa
  lv2       cc   -wi-a----- 100.00g exxNPX-wZdO-uCNy-yiGa-aJGT-JKVl-arfcYT
  [lvmlock] cc   -wi-ao---- 256.00m iLpDel-hR0T-hJ3u-rnVo-PcDh-mcjt-sF9egM

# sanlock status
s lvm_cc:1:/dev/mapper/cc-lvmlock:0
r lvm_cc:exxNPX-wZdO-uCNy-yiGa-aJGT-JKVl-arfcYT:/dev/mapper/cc-lvmlock:71303168:13 p 26099
r lvm_cc:7xoDtu-yvNM-iwQx-C94t-BbYs-UzBl-o8hAIa:/dev/mapper/cc-lvmlock:70254592:3 p 26099

This shows that sanlock is maintaining leases on /dev/mapper/cc-lvmlock.

sanlock acquires a lockspace lease when the lockspace is joined, i.e. when the
VG is started by 'vgchange --lock-start cc'.  This lockspace lease exists at
/dev/mapper/cc-lvmlock offset 0, and sanlock regularly writes to it to maintain
ownership of it.  Joining the lockspace (by acquiring the lockspace lease in
it) then allows standard resource leases to be acquired in the lockspace for
whatever the application wants.  lvmlockd uses resource leases for the VG lock
and LV locks.

sanlock acquires a resource lease for each actual lock that lvm commands use.
Above, there are two LV locks that are held because the two LVs are active.
These are on /dev/mapper/cc-lvmlock at offsets 71303168 and 70254592.  sanlock
does not write to these resource leases except when acquiring and releasing
them (e.g. lvchange -ay/-an).  The renewal of the lockspace lease maintains
ownership of all the resource leases in the lockspace.

If the host loses access to the disk that the sanlock lv lives on, then sanlock
can no longer renew its lockspace lease.  The lockspace lease will eventually
expire, at which point the host will lose ownership of it, and of all resource
leases it holds in the lockspace.  Eventually, other hosts will be able to
acquire those leases.  sanlock ensures that another host will not be able to
acquire one of the expired leases until the current host has quit using it.

It is important that the host "quit using" the leases it is holding if the
sanlock storage is lost and they begin expiring.  If the host cannot quit using
the leases and release them within a limited time, then sanlock will use the
local watchdog to forcibly reset the host before any other host can acquire
them.  This is severe, but preferable to possibly corrupting the data protected
by the lease.  It ensures that two nodes will not be using the same lease at
once.  For LV leases, that means that another host will not be able to activate
the LV while another host still has it active.

sanlock notifies the application that it cannot renew the lockspace lease.  The
application needs to quit using all leases in the lockspace and release them as
quickly as possible.  In the initial version, lvmlockd ignored this
notification, so sanlock would eventually reach the point where it would use
the local watchdog to reset the host.  However, it's better to attempt a
response.  If that response succeeds, the host can avoid being reset.  If the
response fails, then sanlock will eventually reset the host as the last resort.
sanlock gives the application about 40 seconds to complete its response and
release its leases before resetting the host.

An application can specify the path and args of a program that sanlock should
run to notify it if the lockspace lease cannot be renewed.  This program should
carry out the application's response to the expiring leases: attempt to quit
using the leases and then release them.  lvmlockd gives this command to sanlock
for each VG when that VG is started: 'lvmlockctl --kill vg_name'

If sanlock loses access to lease storage in that VG, it runs lvmlockctl --kill,
which:

1. Uses syslog to explain what is happening.

2. Notifies lvmlockd that the VG is being killed, so lvmlockd can
   immediately return an error for this condition if any new lock
   requests are made.  (This step would not be strictly necessary.)

3. Attempts to quit using the VG.  This is not yet implemented, but
   will eventually use blkdeactivate on the VG (or a more forceful
   equivalent.)

4. If step 3 was successful at terminating all use of the VG, then
   lvmlockd is told to release all the leases for the VG.  If this
   is all done without about 40 seconds, the host can avoid being
   reset.

Until steps 3 and 4 are fully implemented, manual steps can be substituted.
This is primarily for testing since the problem needs to be noticed and
responded to in a very short time.  The manual alternative to step 3 is to kill
any processes using file systems on LV's in the VG, unmount all file systems on
the LVs, and deactivate all the LVs.  Once this is done, the manual alternative
to step 4 is to run 'lvmlockctl --drop vg_name', which tells lvmlockd to
release all the leases for the VG.
-------------------------------------------------------------------------------
*/


/*
 * Each lockspace thread has its own sanlock daemon connection.
 * If they shared one, sanlock acquire/release calls would be
 * serialized.  Some aspects of sanlock expect a single connection
 * from each pid: signals due to a sanlock_request, and
 * acquire/release/convert/inquire.  The later can probably be
 * addressed with a flag to indicate that the pid field should be
 * interpreted as 'ci' (which the caller would need to figure
 * out somehow.)
 */

struct lm_sanlock {
	struct sanlk_lockspace ss;
	int sector_size;
	int align_size;
	int sock; /* sanlock daemon connection */
	uint32_t ss_flags; /* sector and align flags for lockspace */
	uint32_t rs_flags; /* sector and align flags for resource */
};

struct rd_sanlock {
	struct val_blk *vb;
	union {
		struct sanlk_resource rs;
		char buf[sizeof(struct sanlk_resource) + sizeof(struct sanlk_disk)];
	};
};

struct sanlk_resourced {
	union {
		struct sanlk_resource rs;
		char buf[sizeof(struct sanlk_resource) + sizeof(struct sanlk_disk)];
	};
};

int lm_data_size_sanlock(void)
{
	return sizeof(struct rd_sanlock);
}

/*
 * lock_args format
 *
 * vg_lock_args format for sanlock is
 * vg_version_string:undefined:lock_lv_name
 *
 * lv_lock_args format for sanlock is
 * lv_version_string:undefined:offset
 *
 * version_string is MAJOR.MINOR.PATCH
 * undefined may contain ":"
 *
 * If a new version of the lock_args string cannot be
 * handled by an old version of lvmlockd, then the
 * new lock_args string should contain a larger major number.
 */

#define VG_LOCK_ARGS_MAJOR 1
#define VG_LOCK_ARGS_MINOR 0
#define VG_LOCK_ARGS_PATCH 0

#define LV_LOCK_ARGS_MAJOR 1
#define LV_LOCK_ARGS_MINOR 0
#define LV_LOCK_ARGS_PATCH 0

/*
 * offset 0 is lockspace
 * offset align_size * 1 is unused
 * offset align_size * 2 is unused
 * ...
 * offset align_size * 64 is unused
 * offset align_size * 65 is gl lock
 * offset align_size * 66 is vg lock
 * offset align_size * 67 is first lv lock
 * offset align_size * 68 is second lv lock
 * ...
 */

#define GL_LOCK_BEGIN UINT64_C(65)
#define VG_LOCK_BEGIN UINT64_C(66)
#define LV_LOCK_BEGIN UINT64_C(67)

static uint64_t daemon_test_lv_count;

static void _fclose(FILE *fp, char *name)
{
	if (fclose(fp))
		log_warn("fclose error %d %s", errno, name ?: "");
}

static void _close(int fd)
{
	if (close(fd))
		log_warn("close error %d fd %d", errno, fd);
}

/*
 * Copy a null-terminated string "str" into a fixed
 * size struct field "buf" which is not null terminated.
 * (ATM SANLK_NAME_LEN is only 48 bytes.
 * Use memccpy() instead of strncpy().
 */
static void strcpy_name_len(char *buf, const char *str, size_t len)
{
	memccpy(buf, str, 0, len);
}

static int lock_lv_name_from_args(char *vg_args, char *lock_lv_name)
{
	return last_string_from_args(vg_args, lock_lv_name);
}

static int lock_lv_offset_from_args(char *lv_args, uint64_t *lock_lv_offset)
{
	char offset_str[MAX_ARGS+1];
	int rv;

	memset(offset_str, 0, sizeof(offset_str));

	rv = last_string_from_args(lv_args, offset_str);
	if (rv < 0)
		return rv;

	errno = 0;
	*lock_lv_offset = strtoull(offset_str, NULL, 10);
	if (errno)
		return -1;
	return 0;
}

static int check_args_version(char *args, unsigned int our_major)
{
	unsigned int major = 0;
	int rv;

	rv = version_from_args(args, &major, NULL, NULL);
	if (rv < 0) {
		log_error("check_args_version %s error %d", args, rv);
		return rv;
	}

	if (major > our_major) {
		log_error("check_args_version %s major %u %u", args, major, our_major);
		return -1;
	}

	return 0;
}

#define MAX_LINE 64

static int read_host_id_file(void)
{
	FILE *file;
	char line[MAX_LINE];
	char key_str[MAX_LINE];
	char val_str[MAX_LINE];
	char *key, *val, *sep;
	int host_id = 0;

	file = fopen(daemon_host_id_file, "r");
	if (!file)
		goto out;

	while (fgets(line, MAX_LINE, file)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;

		key = line;
		sep = strstr(line, "=");
		val = sep + 1;

		if (!sep || !val)
			continue;

		*sep = '\0';
		memset(key_str, 0, sizeof(key_str));
		memset(val_str, 0, sizeof(val_str));

		if (sscanf(key, "%63s", key_str) != 1)
			goto out;
		if (sscanf(val, "%63s", val_str) != 1)
			goto out;

		if (!strcmp(key_str, "host_id")) {
			host_id = atoi(val_str);
			break;
		}
	}

out:
	if (file)
		_fclose(file, (char *)daemon_host_id_file);

	log_debug("host_id %d from %s", host_id, daemon_host_id_file);
	return host_id;
}

#if LOCKDSANLOCK_SUPPORT >= 410
static int read_info_file(struct lockspace *ls, uint32_t *host_id, uint64_t *generation, int *sector_size, int *align_size)
{
	char line[MAX_LINE];
	char path[PATH_MAX] = { 0 };
	FILE *fp;

	if (dm_snprintf(path, sizeof(path), "/var/lib/lvm/lvmlockd_info_%s", ls->vg_name) < 0)
		return -1;

	if (!(fp = fopen(path, "r"))) {
		log_debug("Failed to open info file");
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#')
			continue;
		if (!strncmp(line, "host_id ", 8)) {
			if (sscanf(line, "host_id %u", host_id) != 1)
				goto fail;
		} else if (!strncmp(line, "generation ", 11)) {
			if (sscanf(line, "generation %llu", (unsigned long long *)generation) != 1)
				goto fail;
		} else if (!strncmp(line, "sector_size ", 12)) {
			if (sscanf(line, "sector_size %d", sector_size) != 1)
				goto fail;
		} else if (!strncmp(line, "align_size ", 11)) {
			if (sscanf(line, "align_size %d", align_size) != 1)
				goto fail;
		}
	}

	_fclose(fp, path);
	log_debug("info file: read %u %llu %d %d", *host_id, (unsigned long long)*generation, *sector_size, *align_size);
	return 0;

fail:
	_fclose(fp, path);
	log_debug("Invalid info file values");
	return -1;
}
#endif

static int write_info_file(struct lockspace *ls)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	char path[PATH_MAX] = { 0 };
	FILE *fp;
	time_t t = time(NULL);

	if (dm_snprintf(path, sizeof(path), "/var/lib/lvm/lvmlockd_info_%s", ls->vg_name) < 0)
		return -1;

	if (!(fp = fopen(path, "w"))) {
		log_debug("Failed to create info file");
		return -1;
	}

	fprintf(fp, "# vg %s %s created %s", ls->vg_name, ls->vg_uuid, ctime(&t));
	fprintf(fp, "host_id %u\n", ls->host_id);
	fprintf(fp, "generation %llu\n", (unsigned long long)ls->generation);
	fprintf(fp, "sector_size %d\n", lms->sector_size);
	fprintf(fp, "align_size %d\n", lms->align_size);

	if (fflush(fp))
		log_warn("Failed to write/flush %s", path);
	_fclose(fp, path);

	log_debug("info file: wrote %u %llu %d %d", ls->host_id, (unsigned long long)ls->generation, lms->sector_size, lms->align_size);
	return 0;
}

static void remove_info_file(struct lockspace *ls)
{
	char path[PATH_MAX] = { 0 };

	if (dm_snprintf(path, sizeof(path), "/var/lib/lvm/lvmlockd_info_%s", ls->vg_name) < 0)
		return;

	(void) unlink(path);
}

/* Prepare valid /dev/mapper/vgname-lvname with all the mangling */
static int build_dm_path(char *path, size_t path_len,
			 const char *vg_name, const char *lv_name)
{
	struct dm_pool *mem;
	char *dm_name;
	int rv = 0;

	if (!(mem = dm_pool_create("namepool", 1024))) {
		log_error("Failed to create mempool.");
		return -ENOMEM;
	}

	if (!(dm_name = dm_build_dm_name(mem, vg_name, lv_name, NULL))) {
		log_error("Failed to build dm name for %s/%s.", vg_name, lv_name);
		rv = -EINVAL;
		goto fail;
	}

	if ((dm_snprintf(path, path_len, "%s/%s", dm_dir(), dm_name) < 0)) {
		log_error("Failed to create path %s/%s.", dm_dir(), dm_name);
		rv = -EINVAL;
	}

fail:
	dm_pool_destroy(mem);

	return rv;
}

static void _read_sysfs_size(dev_t devno, const char *name, uint64_t *val)
{
	char path[PATH_MAX];
	char buf[32];
	FILE *fp;
	size_t len;

	*val = 0;

	snprintf(path, sizeof(path), "/sys/dev/block/%d:%d/%s",
		 (int)major(devno), (int)minor(devno), name);

	if (!(fp = fopen(path, "r")))
		return;

	if (!fgets(buf, sizeof(buf), fp))
		goto out;

	if ((len = strlen(buf)) && buf[len - 1] == '\n')
		buf[--len] = '\0';

	if (strlen(buf))
		*val = strtoull(buf, NULL, 0);
out:
	_fclose(fp, path);
}

/* Select sector/align size for a new VG based on what the device reports for
   sector size of the lvmlock LV. */

static int get_sizes_device(char *path, uint64_t *dev_size, int *sector_size, int *align_size, int *align_mb)
{
	unsigned int physical_block_size = 0;
	unsigned int logical_block_size = 0;
	uint64_t val;
	struct stat st;
	int rv;

	rv = stat(path, &st);
	if (rv < 0) {
		log_error("Failed to stat device to get block size %s %d", path, errno);
		return -1;
	}

	_read_sysfs_size(st.st_rdev, "size", &val);
	*dev_size = val * 512;

	_read_sysfs_size(st.st_rdev, "queue/physical_block_size", &val);
	physical_block_size = (unsigned int)val;

	_read_sysfs_size(st.st_rdev, "queue/logical_block_size", &val);
	logical_block_size = (unsigned int)val;

	if ((physical_block_size == 512) && (logical_block_size == 512)) {
		*sector_size = 512;
		*align_size = ONE_MB;
		*align_mb = 1;
		return 0;
	}

	if ((physical_block_size == 4096) && (logical_block_size == 4096)) {
		*sector_size = 4096;
		*align_size = 8 * ONE_MB;
		*align_mb = 8;
		return 0;
	}

	if (physical_block_size && (physical_block_size != 512) && (physical_block_size != 4096)) {
		log_warn("WARNING: Invalid block sizes physical %u logical %u for %s",
			 physical_block_size, logical_block_size, path);
		physical_block_size = 0;
	}

	if (logical_block_size && (logical_block_size != 512) && (logical_block_size != 4096)) {
		log_warn("WARNING: Invalid block sizes physical %u logical %u for %s",
			 physical_block_size, logical_block_size, path);
		logical_block_size = 0;
	}

	if (!physical_block_size && !logical_block_size) {
		log_error("Failed to get a block size for %s", path);
		return -1;
	}

	if (!physical_block_size || !logical_block_size) {
		log_warn("WARNING: Incomplete block size information physical %u logical %u for %s",
			 physical_block_size, logical_block_size, path);
		if (!physical_block_size)
			physical_block_size = logical_block_size;
		if (!logical_block_size)
			logical_block_size = physical_block_size;
	}

	if ((logical_block_size == 4096) && (physical_block_size == 512)) {
		log_warn("WARNING: Mixed block sizes physical %u logical %u (using 4096) for %s",
			 physical_block_size, logical_block_size, path);
		*sector_size = 4096;
		*align_size = 8 * ONE_MB;
		*align_mb = 8;
		return 0;
	}

	if ((physical_block_size == 4096) && (logical_block_size == 512)) {
		log_warn("WARNING: Mixed block sizes physical %u logical %u (using 4096) for %s",
			 physical_block_size, logical_block_size, path);
		*sector_size = 4096;
		*align_size = 8 * ONE_MB;
		*align_mb = 8;
		return 0;
	}

	if (physical_block_size == 512) {
		*sector_size = 512;
		*align_size = ONE_MB;
		*align_mb = 1;
		return 0;
	}

	if (physical_block_size == 4096) {
		*sector_size = 4096;
		*align_size = 8 * ONE_MB;
		*align_mb = 8;
		return 0;
	}

	log_error("Failed to get a block size for %s", path);
	return -1;
}

static int _lease_corrupt_error(int rv)
{
	if (rv == SANLK_LEADER_MAGIC ||
	    rv == SANLK_LEADER_VERSION ||
	    rv == SANLK_LEADER_DIFF ||
	    rv == SANLK_LEADER_LOCKSPACE ||
	    rv == SANLK_LEADER_RESOURCE ||
	    rv == SANLK_LEADER_CHECKSUM ||
	    rv == SANLK_DBLOCK_CHECKSUM)
		return 1;
	return 0;
}

/* Get the sector/align sizes that were used to create an existing VG.
   sanlock encoded this in the lockspace/resource structs on disk. */

static int read_lockspace_info(char *path, uint32_t host_id, int *sector_size, int *align_size, int *align_mb,
			       uint32_t *ss_flags, uint32_t *rs_flags, struct sanlk_host *hs)
{
	struct sanlk_lockspace ss;
	uint32_t io_timeout = 0;
	int rv;

	memset(&ss, 0, sizeof(ss));
	memcpy(ss.host_id_disk.path, path, SANLK_PATH_LEN);
	ss.host_id_disk.offset = 0;
	ss.host_id = host_id;

#if LOCKDSANLOCK_SUPPORT >= 400
	if (hs)
		rv = sanlock_read_lockspace_host(&ss, 0, &io_timeout, hs);
	else
#endif
		rv = sanlock_read_lockspace(&ss, 0, &io_timeout);

	if (_lease_corrupt_error(rv)) {
		log_error("read_lockspace_info %s %u corrupt lease error %d", path, host_id, rv);
		return -ELOCKREPAIR;
	}

	if (rv < 0) {
		log_error("read_lockspace_info %s %u error %d", path, host_id, rv);
		return rv;
	}

	if ((ss.flags & SANLK_LSF_SECTOR4K) && (ss.flags & SANLK_LSF_ALIGN8M)) {
		*sector_size = 4096;
		*align_mb = 8;
		*align_size = 8 * ONE_MB;
		*ss_flags = SANLK_LSF_SECTOR4K | SANLK_LSF_ALIGN8M;
		*rs_flags = SANLK_RES_SECTOR4K | SANLK_RES_ALIGN8M;

	} else if ((ss.flags & SANLK_LSF_SECTOR4K) && (ss.flags & SANLK_LSF_ALIGN4M)) {
		*sector_size = 4096;
		*align_mb = 4;
		*align_size = 4 * ONE_MB;
		*ss_flags = SANLK_LSF_SECTOR4K | SANLK_LSF_ALIGN4M;
		*rs_flags = SANLK_RES_SECTOR4K | SANLK_RES_ALIGN4M;

	} else if ((ss.flags & SANLK_LSF_SECTOR4K) && (ss.flags & SANLK_LSF_ALIGN2M)) {
		*sector_size = 4096;
		*align_mb = 2;
		*align_size = 2 * ONE_MB;
		*ss_flags = SANLK_LSF_SECTOR4K | SANLK_LSF_ALIGN2M;
		*rs_flags = SANLK_RES_SECTOR4K | SANLK_RES_ALIGN2M;

	} else if ((ss.flags & SANLK_LSF_SECTOR4K) && (ss.flags & SANLK_LSF_ALIGN1M)) {
		*sector_size = 4096;
		*align_mb = 1;
		*align_size = ONE_MB;
		*ss_flags = SANLK_LSF_SECTOR4K | SANLK_LSF_ALIGN1M;
		*rs_flags = SANLK_RES_SECTOR4K | SANLK_RES_ALIGN1M;

	} else if ((ss.flags & SANLK_LSF_SECTOR512) && (ss.flags & SANLK_LSF_ALIGN1M)) {
		*sector_size = 512;
		*align_mb = 1;
		*align_size = ONE_MB;
		*ss_flags = SANLK_LSF_SECTOR512 | SANLK_LSF_ALIGN1M;
		*rs_flags = SANLK_RES_SECTOR512 | SANLK_RES_ALIGN1M;
	}

	log_debug("read_lockspace_info %s %u found sector_size %d align_size %d",
		  path, host_id, *sector_size, *align_size);
	return 0;
}

/*
 * vgcreate
 *
 * For init_vg, vgcreate passes the internal lv name as vg_args.
 * This constructs the full/proper vg_args format, containing the
 * version and lv name, and returns the real lock_args in vg_args.
 */

#define MAX_VERSION 16

int lm_init_vg_sanlock(char *ls_name, char *vg_name, uint32_t flags, char *vg_args, int opt_align_mb)
{
	struct sanlk_lockspace ss;
	struct sanlk_resourced rd;
	struct sanlk_disk disk;
	char lock_lv_name[MAX_ARGS+1];
	char lock_args_version[MAX_VERSION+1];
	const char *gl_name = NULL;
	uint32_t rs_flags;
	uint32_t daemon_version = 0;
	uint32_t daemon_proto = 0;
	uint64_t offset;
	uint64_t dev_size;
	int sector_size = 0;
	int align_size = 0;
	int align_mb = 0;
	int i, rv;

	memset(&ss, 0, sizeof(ss));
	memset(&rd, 0, sizeof(rd));
	memset(&disk, 0, sizeof(disk));
	memset(lock_args_version, 0, sizeof(lock_args_version));

	if (!vg_args || !vg_args[0] || !strcmp(vg_args, "none")) {
		log_error("S %s init_vg_san vg_args missing", ls_name);
		return -EARGS;
	}

	snprintf(lock_args_version, MAX_VERSION, "%u.%u.%u",
		 VG_LOCK_ARGS_MAJOR, VG_LOCK_ARGS_MINOR, VG_LOCK_ARGS_PATCH);

	/* see comment above about input vg_args being only lock_lv_name */
	dm_strncpy(lock_lv_name, vg_args, sizeof(lock_lv_name));

	if (strlen(lock_lv_name) + strlen(lock_args_version) + 2 > MAX_ARGS)
		return -EARGS;

	if ((rv = build_dm_path(disk.path, SANLK_PATH_LEN, vg_name, lock_lv_name)))
		return rv;

	log_debug("S %s init_vg_san path %s align %d", ls_name, disk.path, opt_align_mb);

	if (daemon_test) {
		if (!gl_lsname_sanlock[0])
			strncpy(gl_lsname_sanlock, ls_name, MAX_NAME);
		rv = snprintf(vg_args, MAX_ARGS, "%s:%s", lock_args_version, lock_lv_name);
		if (rv >= MAX_ARGS)
			log_debug("init_vg_san vg_args may be too long %d %s", rv, vg_args);
		return 0;
	}

	rv = sanlock_version(0, &daemon_version, &daemon_proto);
	if (rv < 0) {
		log_error("S %s init_vg_san failed to connect to sanlock daemon", ls_name);
		return -EMANAGER;
	}

	log_debug("sanlock daemon version %08x proto %08x",
		  daemon_version, daemon_proto);

	/* Nothing formatted on disk yet, use what the device reports. */
	rv = get_sizes_device(disk.path, &dev_size, &sector_size, &align_size, &align_mb);
	if (rv < 0) {
		if (rv == -EACCES) {
			log_error("S %s init_vg_san sanlock error -EACCES: no permission to access %s",
				  ls_name, disk.path);
			return -EDEVOPEN;
		} else {
			log_error("S %s init_vg_san sanlock error %d trying to get sector/align size of %s",
				  ls_name, rv, disk.path);
			return -EARGS;
		}
	}

	/* Non-default lease size is requested. */
	if ((sector_size == 4096) && opt_align_mb && (opt_align_mb != 8)) {
		if (opt_align_mb != 1 && opt_align_mb != 2 && opt_align_mb != 4) {
			log_error("S %s init_vg_sanlock invalid align input %u", ls_name, opt_align_mb);
			return -EARGS;
		}
		align_mb = opt_align_mb;
		align_size = align_mb * ONE_MB;
	}

	log_debug("S %s init_vg_san %s dev_size %llu sector_size %u align_size %u",
		  ls_name, disk.path, (unsigned long long)dev_size, sector_size, align_size);

	strcpy_name_len(ss.name, ls_name, SANLK_NAME_LEN);
	memcpy(ss.host_id_disk.path, disk.path, SANLK_PATH_LEN);
	ss.host_id_disk.offset = 0;

	if (sector_size == 512) {
		ss.flags = SANLK_LSF_SECTOR512 | SANLK_LSF_ALIGN1M;
		rs_flags = SANLK_RES_SECTOR512 | SANLK_RES_ALIGN1M;
	} else if (sector_size == 4096) {
		if (align_mb == 8) {
			ss.flags = SANLK_LSF_SECTOR4K | SANLK_LSF_ALIGN8M;
			rs_flags = SANLK_RES_SECTOR4K | SANLK_RES_ALIGN8M;
		} else if (align_mb == 4) {
			ss.flags = SANLK_LSF_SECTOR4K | SANLK_LSF_ALIGN4M;
			rs_flags = SANLK_RES_SECTOR4K | SANLK_RES_ALIGN4M;
		} else if (align_mb == 2) {
			ss.flags = SANLK_LSF_SECTOR4K | SANLK_LSF_ALIGN2M;
			rs_flags = SANLK_RES_SECTOR4K | SANLK_RES_ALIGN2M;
		} else if (align_mb == 1) {
			ss.flags = SANLK_LSF_SECTOR4K | SANLK_LSF_ALIGN1M;
			rs_flags = SANLK_RES_SECTOR4K | SANLK_RES_ALIGN1M;
		}
		else {
			log_error("Invalid sanlock align_size %d %d", align_size, align_mb);
			return -EARGS;
		}
	} else {
		log_error("Invalid sanlock sector_size %d", sector_size);
		return -EARGS;
	}

	rv = sanlock_write_lockspace(&ss, 0, 0, sanlock_io_timeout);
	if (rv < 0) {
		log_error("S %s init_vg_san write_lockspace error %d %s",
			  ls_name, rv, ss.host_id_disk.path);
		return rv;
	}
	
	/*
	 * We want to create the global lock in the first sanlock vg.
	 * If other sanlock vgs exist, then one of them must contain
	 * the gl.  If gl_lsname_sanlock is not set, then perhaps
	 * the sanlock vg with the gl has been removed or has not yet
	 * been seen. (Would vgcreate get this far in that case?)
	 * If dlm vgs exist, then we choose to use the dlm gl and
	 * not a sanlock gl.
	 */

	if (flags & LD_AF_ENABLE)
		gl_name = R_NAME_GL;
	else if (flags & LD_AF_DISABLE)
		gl_name = R_NAME_GL_DISABLED;
	else if (!gl_use_sanlock || gl_lsname_sanlock[0] || !lockspaces_empty())
		gl_name = R_NAME_GL_DISABLED;
	else
		gl_name = R_NAME_GL;

	memcpy(rd.rs.lockspace_name, ss.name, SANLK_NAME_LEN);
	strcpy_name_len(rd.rs.name, gl_name, SANLK_NAME_LEN);
	memcpy(rd.rs.disks[0].path, disk.path, SANLK_PATH_LEN);
	rd.rs.disks[0].offset = align_size * GL_LOCK_BEGIN;
	rd.rs.num_disks = 1;
	rd.rs.flags = rs_flags;

	rv = sanlock_write_resource(&rd.rs, 0, 0, 0);
	if (rv < 0) {
		log_error("S %s init_vg_san write_resource gl error %d %s",
			  ls_name, rv, rd.rs.disks[0].path);
		return rv;
	}

	memcpy(rd.rs.lockspace_name, ss.name, SANLK_NAME_LEN);
	strcpy_name_len(rd.rs.name, R_NAME_VG, SANLK_NAME_LEN);
	memcpy(rd.rs.disks[0].path, disk.path, SANLK_PATH_LEN);
	rd.rs.disks[0].offset = align_size * VG_LOCK_BEGIN;
	rd.rs.num_disks = 1;
	rd.rs.flags = rs_flags;

	rv = sanlock_write_resource(&rd.rs, 0, 0, 0);
	if (rv < 0) {
		log_error("S %s init_vg_san write_resource vg error %d %s",
			  ls_name, rv, rd.rs.disks[0].path);
		return rv;
	}

	if (!strcmp(gl_name, R_NAME_GL))
		dm_strncpy(gl_lsname_sanlock, ls_name, sizeof(gl_lsname_sanlock));
 
	rv = snprintf(vg_args, MAX_ARGS, "%s:%s", lock_args_version, lock_lv_name);
	if (rv >= MAX_ARGS)
		log_debug("init_vg_san vg_args may be too long %d %s", rv, vg_args);

	log_debug("S %s init_vg_san done vg_args %s", ls_name, vg_args);

	/*
	 * Go through all lv resource slots and initialize them with the
	 * correct lockspace name but a special resource name that indicates
	 * it is unused.
	 */

	memset(&rd, 0, sizeof(rd));
	rd.rs.num_disks = 1;
	rd.rs.flags = rs_flags;
	memcpy(rd.rs.disks[0].path, disk.path, SANLK_PATH_LEN);
	strcpy_name_len(rd.rs.lockspace_name, ls_name, SANLK_NAME_LEN);
	strcpy_name_len(rd.rs.name, "#unused", SANLK_NAME_LEN);

	offset = align_size * LV_LOCK_BEGIN;

	log_debug("S %s init_vg_san clearing lv lease areas", ls_name);

	for (i = 0; ; i++) {
		if (dev_size && (offset + align_size > dev_size))
			break;

		rd.rs.disks[0].offset = offset;

		rv = sanlock_write_resource(&rd.rs, 0, 0, 0);
		if (rv == -EMSGSIZE || rv == -ENOSPC) {
			/* This indicates the end of the device is reached. */
			rv = -EMSGSIZE;
			break;
		}

		if (rv < 0) {
			log_error("clear lv resource area %llu error %d",
				  (unsigned long long)offset, rv);
			return rv;
		}
		offset += align_size;
	}

	return 0;
}

/*
 * lvcreate
 *
 * The offset at which the lv lease is written is passed
 * all the way back to the lvcreate command so that it
 * can be saved in the lv's lock_args in the vg metadata.
 */

int lm_init_lv_sanlock(struct lockspace *ls, char *ls_name, char *vg_name, char *lv_name, char *vg_args, char *lv_args, char *prev_args)
{
	char disk_path[SANLK_PATH_LEN];
	struct lm_sanlock *lms;
	struct sanlk_resourced rd;
	char lock_lv_name[MAX_ARGS+1];
	char lock_args_version[MAX_VERSION+1];
	uint64_t offset;
	uint64_t prev_offset = 0;
	int sector_size = 0;
	int align_size = 0;
	int align_mb;
	uint32_t ss_flags;
	uint32_t rs_flags = 0;
	uint32_t tries = 1;
	int rv;

	memset(&rd, 0, sizeof(rd));
	memset(lock_lv_name, 0, sizeof(lock_lv_name));
	memset(lock_args_version, 0, sizeof(lock_args_version));
	memset(disk_path, 0, sizeof(disk_path));

	snprintf(lock_args_version, MAX_VERSION, "%u.%u.%u",
		 LV_LOCK_ARGS_MAJOR, LV_LOCK_ARGS_MINOR, LV_LOCK_ARGS_PATCH);

	if (daemon_test) {
		align_size = 1024 * 1024;
		snprintf(lv_args, MAX_ARGS, "%s:%llu",
			 lock_args_version,
			 (unsigned long long)((align_size * LV_LOCK_BEGIN) + (align_size * daemon_test_lv_count)));
		daemon_test_lv_count++;
		return 0;
	}

	rv = lock_lv_name_from_args(vg_args, lock_lv_name);
	if (rv < 0) {
		log_error("S %s init_lv_san lock_lv_name_from_args error %d %s",
			  ls_name, rv, vg_args);
		return rv;
	}

	if ((rv = build_dm_path(disk_path, SANLK_PATH_LEN, vg_name, lock_lv_name))) {
		log_error("S %s init_lv_san lock_lv_name path error %d %s",
			  ls_name, rv, vg_args);
		return rv;
	}

	if (ls) {
		lms = (struct lm_sanlock *)ls->lm_data;
		align_size = lms->align_size;
		rs_flags = lms->rs_flags;
		offset = ls->free_lock_offset;
	} else {
		/* FIXME: optimize repeated init_lv for vgchange --locktype sanlock,
		   to avoid finding align_size/rs_flags each time. */

		/* using host_id 1 to get sizes since we don't need host-specific info */

		rv = read_lockspace_info(disk_path, 1, &sector_size, &align_size, &align_mb, &ss_flags, &rs_flags, NULL);
		if (rv < 0) {
			log_error("S %s init_lv_san read_lockspace_info error %d %s",
				  ls_name, rv, disk_path);
			return rv;
		}

		/*
		 * With a prev offset, start search after that.
		 * Without a prev offset, start search from the beginning. 
		 */
		if (prev_args && !lock_lv_offset_from_args(prev_args, &prev_offset))
			offset = prev_offset + align_size;
		else
			offset = align_size * LV_LOCK_BEGIN;
	}

	if (offset < (align_size * LV_LOCK_BEGIN)) {
		log_error("S %s init_lv_san invalid offset %llu", ls_name, (unsigned long long)offset);
		return -1;
	}

	strcpy_name_len(rd.rs.lockspace_name, ls_name, SANLK_NAME_LEN);
	rd.rs.num_disks = 1;
	memcpy(rd.rs.disks[0].path, disk_path, SANLK_PATH_LEN-1);
	rd.rs.flags = rs_flags;

	while (1) {
		rd.rs.disks[0].offset = offset;

		memset(rd.rs.name, 0, SANLK_NAME_LEN);

		rv = sanlock_read_resource(&rd.rs, 0);
		if (rv == -EMSGSIZE || rv == -ENOSPC) {
			/* This indicates the end of the device is reached. */
			log_debug("S %s init_lv_san read limit offset %llu",
				  ls_name, (unsigned long long)offset);
			rv = -EMSGSIZE;
			return rv;
		}

		if (rv && rv != SANLK_LEADER_MAGIC) {
			log_error("S %s init_lv_san read error %d offset %llu",
				  ls_name, rv, (unsigned long long)offset);
			break;
		}

		if (!strncmp(rd.rs.name, lv_name, SANLK_NAME_LEN)) {
			log_error("S %s init_lv_san resource name %s already exists at %llu",
				  ls_name, lv_name, (unsigned long long)offset);
			return -EEXIST;
		}

		/*
		 * If we read newly extended space, it will not be initialized
		 * with an "#unused" resource, but will return SANLK_LEADER_MAGIC
		 * indicating an uninitialized paxos structure on disk.
		 */
		if ((rv == SANLK_LEADER_MAGIC) || !strcmp(rd.rs.name, "#unused")) {
			log_debug("S %s init_lv_san %s found unused area at %llu try %u",
				  ls_name, lv_name, (unsigned long long)offset, tries);

			strcpy_name_len(rd.rs.name, lv_name, SANLK_NAME_LEN);
			rd.rs.flags = rs_flags;

			rv = sanlock_write_resource(&rd.rs, 0, 0, 0);
			if (!rv) {
				snprintf(lv_args, MAX_ARGS, "%s:%llu",
				         lock_args_version, (unsigned long long)offset);
			} else {
				log_error("S %s init_lv_san write error %d offset %llu",
					  ls_name, rv, (unsigned long long)rv);
			}
			break;
		}

		offset += align_size;
		tries++;
	}

	return rv;
}

/*
 * Read the lockspace and each resource, replace the lockspace name,
 * and write it back.
 */

int lm_rename_vg_sanlock(char *ls_name, char *vg_name, uint32_t flags, char *vg_args)
{
	struct sanlk_lockspace ss;
	struct sanlk_resourced rd;
	struct sanlk_disk disk;
	char lock_lv_name[MAX_ARGS+1];
	uint64_t offset;
	uint32_t io_timeout;
	int sector_size = 0;
	int align_size = 0;
	int i, rv;

	memset(&disk, 0, sizeof(disk));
	memset(lock_lv_name, 0, sizeof(lock_lv_name));

	if (!vg_args || !vg_args[0] || !strcmp(vg_args, "none")) {
		log_error("S %s rename_vg_san vg_args missing", ls_name);
		return -EINVAL;
	}

	rv = lock_lv_name_from_args(vg_args, lock_lv_name);
	if (rv < 0) {
		log_error("S %s init_lv_san lock_lv_name_from_args error %d %s",
			  ls_name, rv, vg_args);
		return rv;
	}

	if ((rv = build_dm_path(disk.path, SANLK_PATH_LEN, vg_name, lock_lv_name)))
		return rv;

	log_debug("S %s rename_vg_san path %s", ls_name, disk.path);

	if (daemon_test)
		return 0;

	/* FIXME: device is not always ready for us here */
	sleep(1);

	/*
	 * Lockspace
	 */

	memset(&ss, 0, sizeof(ss));
	memcpy(ss.host_id_disk.path, disk.path, SANLK_PATH_LEN);
	ss.host_id_disk.offset = 0;

	rv = sanlock_read_lockspace(&ss, 0, &io_timeout);
	if (rv < 0) {
		log_error("S %s rename_vg_san read_lockspace error %d %s",
			  ls_name, rv, ss.host_id_disk.path);
		return rv;
	}

	if (ss.flags & SANLK_LSF_SECTOR512) {
		sector_size = 512;
		align_size = ONE_MB;
	} else if (ss.flags & SANLK_LSF_SECTOR4K) {
		sector_size = 4096;
		if (ss.flags & SANLK_LSF_ALIGN8M)
			align_size = 8 * ONE_MB;
		else if (ss.flags & SANLK_LSF_ALIGN4M)
			align_size = 4 * ONE_MB;
		else if (ss.flags & SANLK_LSF_ALIGN2M)
			align_size = 2 * ONE_MB;
		else if (ss.flags & SANLK_LSF_ALIGN1M)
			align_size = ONE_MB;
	} else {
		/* use the old method */
		align_size = sanlock_align(&ss.host_id_disk);
		if (align_size <= 0) {
			log_error("S %s rename_vg_san unknown sector/align size for %s",
				 ls_name, ss.host_id_disk.path);
			return -1;
		}
		sector_size = (align_size == ONE_MB) ? 512 : 4096;
	}

	if (!sector_size || !align_size)
		return -1;

	strcpy_name_len(ss.name, ls_name, SANLK_NAME_LEN);

	rv = sanlock_write_lockspace(&ss, 0, 0, sanlock_io_timeout);
	if (rv < 0) {
		log_error("S %s rename_vg_san write_lockspace error %d %s",
			  ls_name, rv, ss.host_id_disk.path);
		return rv;
	}

	/*
	 * GL resource
	 */

	memset(&rd, 0, sizeof(rd));
	memcpy(rd.rs.disks[0].path, disk.path, SANLK_PATH_LEN);
	rd.rs.disks[0].offset = align_size * GL_LOCK_BEGIN;
	rd.rs.num_disks = 1;

	rv = sanlock_read_resource(&rd.rs, 0);
	if (rv < 0) {
		log_error("S %s rename_vg_san read_resource gl error %d %s",
			  ls_name, rv, rd.rs.disks[0].path);
		return rv;
	}

	memcpy(rd.rs.lockspace_name, ss.name, SANLK_NAME_LEN);

	rv = sanlock_write_resource(&rd.rs, 0, 0, 0);
	if (rv < 0) {
		log_error("S %s rename_vg_san write_resource gl error %d %s",
			  ls_name, rv, rd.rs.disks[0].path);
		return rv;
	}

	/*
	 * VG resource
	 */

	memset(&rd, 0, sizeof(rd));
	memcpy(rd.rs.disks[0].path, disk.path, SANLK_PATH_LEN);
	rd.rs.disks[0].offset = align_size * VG_LOCK_BEGIN;
	rd.rs.num_disks = 1;

	rv = sanlock_read_resource(&rd.rs, 0);
	if (rv < 0) {
		log_error("S %s rename_vg_san write_resource vg error %d %s",
			  ls_name, rv, rd.rs.disks[0].path);
		return rv;
	}

	memcpy(rd.rs.lockspace_name, ss.name, SANLK_NAME_LEN);

	rv = sanlock_write_resource(&rd.rs, 0, 0, 0);
	if (rv < 0) {
		log_error("S %s rename_vg_san write_resource vg error %d %s",
			  ls_name, rv, rd.rs.disks[0].path);
		return rv;
	}

	/*
	 * LV resources
	 */

	offset = align_size * LV_LOCK_BEGIN;

	for (i = 0; ; i++) {
		memset(&rd, 0, sizeof(rd));
		memcpy(rd.rs.disks[0].path, disk.path, SANLK_PATH_LEN);
		rd.rs.disks[0].offset = offset;
		rd.rs.num_disks = 1;

		rv = sanlock_read_resource(&rd.rs, 0);
		if (rv == -EMSGSIZE || rv == -ENOSPC) {
			/* This indicates the end of the device is reached. */
			rv = -EMSGSIZE;
			break;
		}

		if (rv < 0) {
			log_error("S %s rename_vg_san read_resource resource area %llu error %d",
				  ls_name, (unsigned long long)offset, rv);
			break;
		}

		memcpy(rd.rs.lockspace_name, ss.name, SANLK_NAME_LEN);

		rv = sanlock_write_resource(&rd.rs, 0, 0, 0);
		if (rv) {
			log_error("S %s rename_vg_san write_resource resource area %llu error %d",
				  ls_name, (unsigned long long)offset, rv);
			break;
		}
		offset += align_size;
	}

	return 0;
}

/* lvremove */
int lm_free_lv_sanlock(struct lockspace *ls, struct resource *r)
{
	struct rd_sanlock *rds = (struct rd_sanlock *)r->lm_data;
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct sanlk_resource *rs = &rds->rs;
	uint64_t offset = rds->rs.disks[0].offset;
	int rv;

	log_debug("%s:%s free_lv_san %llu", ls->name, r->name, (unsigned long long)offset);

	if (daemon_test)
		return 0;

	strcpy_name_len(rs->name, "#unused", SANLK_NAME_LEN);

	if (!offset && !lock_lv_offset_from_args(r->lv_args, &offset)) {
		rds->rs.disks[0].offset = offset;
		log_debug("%s:%s free_lv_san lock_args offset %llu", ls->name, r->name, (unsigned long long)offset);
	}

	if (offset < (lms->align_size * LV_LOCK_BEGIN)) {
		log_error("%s:%s free_lv_san invalid offset %llu",
			  ls->name, r->name, (unsigned long long)offset);
		return -1;
	}

	rv = sanlock_write_resource(rs, 0, 0, 0);
	if (rv < 0)
		log_error("%s:%s free_lv_san %llu write error %d",
			  ls->name, r->name, (unsigned long long)offset, rv);

	return rv;
}

int lm_ex_disable_gl_sanlock(struct lockspace *ls)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct sanlk_resourced rd1;
	struct sanlk_resourced rd2;
	struct sanlk_resource *rs1;
	struct sanlk_resource *rs2;
	struct sanlk_resource **rs_args;
	int rv;

	if (daemon_test)
		return 0;

	rs_args = malloc(2 * sizeof(struct sanlk_resource *));
	if (!rs_args)
		return -ENOMEM;

	rs1 = &rd1.rs;
	rs2 = &rd2.rs;

	memset(&rd1, 0, sizeof(rd1));
	memset(&rd2, 0, sizeof(rd2));

	strcpy_name_len(rd1.rs.lockspace_name, ls->name, SANLK_NAME_LEN);
	strcpy_name_len(rd1.rs.name, R_NAME_GL, SANLK_NAME_LEN);

	strcpy_name_len(rd2.rs.lockspace_name, ls->name, SANLK_NAME_LEN);
	strcpy_name_len(rd2.rs.name, R_NAME_GL_DISABLED, SANLK_NAME_LEN);

	rd1.rs.num_disks = 1;
	memcpy(rd1.rs.disks[0].path, lms->ss.host_id_disk.path, SANLK_PATH_LEN-1);
	rd1.rs.disks[0].offset = lms->align_size * GL_LOCK_BEGIN;
	
	rd1.rs.flags = lms->rs_flags;
	rd2.rs.flags = lms->rs_flags;

	rv = sanlock_acquire(lms->sock, -1, 0, 1, &rs1, NULL);
	if (rv < 0) {
		log_error("S %s ex_disable_gl_san acquire error %d",
			  ls->name, rv);
		goto out;
	}

	rs_args[0] = rs1;
	rs_args[1] = rs2;

	rv = sanlock_release(lms->sock, -1, SANLK_REL_RENAME, 2, rs_args);
	if (rv < 0) {
		log_error("S %s ex_disable_gl_san release_rename error %d",
			  ls->name, rv);
	}

out:
	free(rs_args);
	return rv;
}

/*
 * enable/disable exist because each vg contains a global lock,
 * but we only want to use the gl from one of them.  The first
 * sanlock vg created, has its gl enabled, and subsequent
 * sanlock vgs have their gl disabled.  If the vg containing the
 * gl is removed, the gl from another sanlock vg needs to be
 * enabled.  Or, if gl in multiple vgs are somehow enabled, we
 * want to be able to disable one of them.
 *
 * Disable works by naming/renaming the gl resource to have a
 * name that is different from the predefined name.
 * When a host attempts to acquire the gl with its standard
 * predefined name, it will fail because the resource's name
 * on disk doesn't match.
 */

int lm_able_gl_sanlock(struct lockspace *ls, int enable)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct sanlk_resourced rd;
	const char *gl_name;
	int rv;

	if (enable)
		gl_name = R_NAME_GL;
	else
		gl_name = R_NAME_GL_DISABLED;

	if (daemon_test)
		goto out;

	memset(&rd, 0, sizeof(rd));

	strcpy_name_len(rd.rs.lockspace_name, ls->name, SANLK_NAME_LEN);
	strcpy_name_len(rd.rs.name, gl_name, SANLK_NAME_LEN);

	rd.rs.num_disks = 1;
	memcpy(rd.rs.disks[0].path, lms->ss.host_id_disk.path, SANLK_PATH_LEN-1);
	rd.rs.disks[0].offset = lms->align_size * GL_LOCK_BEGIN;
	rd.rs.flags = lms->rs_flags;

	rv = sanlock_write_resource(&rd.rs, 0, 0, 0);
	if (rv < 0) {
		log_error("S %s able_gl %d write_resource gl error %d %s",
			  ls->name, enable, rv, rd.rs.disks[0].path);
		return rv;
	}
out:
	log_debug("S %s able_gl %s", ls->name, gl_name);

	ls->sanlock_gl_enabled = enable;

	if (enable)
		dm_strncpy(gl_lsname_sanlock, ls->name, sizeof(gl_lsname_sanlock));

	if (!enable && !strcmp(gl_lsname_sanlock, ls->name))
		memset(gl_lsname_sanlock, 0, sizeof(gl_lsname_sanlock));

	return 0;
}

static int gl_is_enabled(struct lockspace *ls, struct lm_sanlock *lms)
{
	char strname[SANLK_NAME_LEN + 1];
	struct sanlk_resourced rd;
	uint64_t offset;
	int rv;

	if (daemon_test)
		return 1;

	memset(&rd, 0, sizeof(rd));

	strcpy_name_len(rd.rs.lockspace_name, ls->name, SANLK_NAME_LEN);

	/* leave rs.name empty, it is what we're checking */

	rd.rs.num_disks = 1;
	memcpy(rd.rs.disks[0].path, lms->ss.host_id_disk.path, SANLK_PATH_LEN-1);

	offset = lms->align_size * GL_LOCK_BEGIN;
	rd.rs.disks[0].offset = offset;

	rv = sanlock_read_resource(&rd.rs, 0);
	if (rv < 0) {
		log_error("gl_is_enabled read_resource align_size %d offset %llu error %d",
			  lms->align_size, (unsigned long long)offset, rv);
		return rv;
	}

	memset(strname, 0, sizeof(strname));
	memcpy(strname, rd.rs.name, SANLK_NAME_LEN);

	if (!strcmp(strname, R_NAME_GL_DISABLED)) {
		return 0;
	}

	if (!strcmp(strname, R_NAME_GL)) {
		return 1;
	}

	log_error("gl_is_enabled invalid gl name %s", strname);
	return -1;
}

int lm_gl_is_enabled(struct lockspace *ls)
{
	int rv;
	rv = gl_is_enabled(ls, ls->lm_data);
	ls->sanlock_gl_enabled = rv;
	return rv;
}

/*
 * This is called at the beginning of lvcreate to
 * ensure there is free space for a new LV lock.
 * If not, lvcreate will extend the lvmlock lv
 * before continuing with creating the new LV.
 * This way, lm_init_lv_san() should find a free
 * lock (unless the autoextend of lvmlock lv has
 * been disabled.)
 */

int lm_find_free_lock_sanlock(struct lockspace *ls, uint64_t lv_size_bytes)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct sanlk_resourced rd;
	uint64_t offset;
	uint64_t start_offset;
	uint32_t tries = 0;
	int rv;
	int round = 0;

	if (daemon_test) {
		ls->free_lock_offset = (ONE_MB * LV_LOCK_BEGIN) + (ONE_MB * (daemon_test_lv_count + 1));
		return 0;
	}

	memset(&rd, 0, sizeof(rd));

	strcpy_name_len(rd.rs.lockspace_name, ls->name, SANLK_NAME_LEN);
	rd.rs.num_disks = 1;
	memcpy(rd.rs.disks[0].path, lms->ss.host_id_disk.path, SANLK_PATH_LEN-1);
	rd.rs.flags = lms->rs_flags;

	if (ls->free_lock_offset)
		offset = ls->free_lock_offset;
	else
		offset = lms->align_size * LV_LOCK_BEGIN;

	start_offset = offset;

	while (1) {
		if (offset >= start_offset && round) {
			/* This indicates the all space are allocated. */
			log_debug("S %s init_lv_san read back to start offset %llu",
				ls->name, (unsigned long long)offset);
			rv = -EMSGSIZE;
			return rv;
		}

		rd.rs.disks[0].offset = offset;

		memset(rd.rs.name, 0, SANLK_NAME_LEN);

		/*
		 * End of the device. Older lvm versions didn't pass lv_size_bytes
		 * and just relied on sanlock_read_resource returning an error when
		 * reading beyond the device.
		 */
		if (lv_size_bytes && (offset + lms->align_size > lv_size_bytes)) {
			/* end of the device */
			log_debug("S %s find_free_lock_san read limit offset %llu lv_size_bytes %llu",
				  ls->name, (unsigned long long)offset, (unsigned long long)lv_size_bytes);

			/* remember the NO SPACE offset, if no free area left,
			 * search from this offset after extend */
			ls->free_lock_offset = offset;

			offset = lms->align_size * LV_LOCK_BEGIN;
			round = 1;
			continue;
		}

		rv = sanlock_read_resource(&rd.rs, 0);
		if (rv == -EMSGSIZE || rv == -ENOSPC) {
			/*
			 * These errors indicate the end of the device is reached.
			 * Still check this in case lv_size_bytes is not provided.
			 */
			log_debug("S %s find_free_lock_san read limit offset %llu",
				  ls->name, (unsigned long long)offset);

			/* remember the NO SPACE offset, if no free area left,
			 * search from this offset after extend */
			ls->free_lock_offset = offset;

			offset = lms->align_size * LV_LOCK_BEGIN;
			round = 1;
			continue;
		}

		/*
		 * If we read newly extended space, it will not be initialized
		 * with an "#unused" resource, but will return an error about
		 * an invalid paxos structure on disk.
		 */
		if (rv == SANLK_LEADER_MAGIC) {
			log_debug("S %s find_free_lock_san found empty area at %llu try %u",
				  ls->name, (unsigned long long)offset, tries);
			ls->free_lock_offset = offset;
			return 0;
		}

		if (rv) {
			log_error("S %s find_free_lock_san read error %d offset %llu",
				  ls->name, rv, (unsigned long long)offset);
			break;
		}

		if (!strcmp(rd.rs.name, "#unused")) {
			log_debug("S %s find_free_lock_san found unused area at %llu try %u",
				  ls->name, (unsigned long long)offset, tries);
			ls->free_lock_offset = offset;
			return 0;
		}

		tries++;
		offset += lms->align_size;
	}

	return rv;
}

/*
 * host A: start_vg/add_lockspace
 * host B: vgremove
 *
 * The global lock cannot always be held around start_vg
 * on host A because the gl is in a vg that may not be
 * started yet, or may be in the vg we are starting.
 *
 * If B removes the vg, destroying the delta leases,
 * while A is a lockspace member, it will cause A's
 * sanlock delta lease renewal to fail, and lockspace
 * recovery.
 *
 * I expect this overlap would usually cause a failure
 * in the add_lockspace() on host A when it sees that
 * the lockspace structures have been clobbered by B.
 * Having add_lockspace() fail should be a fine result.
 *
 * If add_lockspace was somehow able to finish, the
 * subsequent renewal would probably fail instead.
 * This should also not create any major problems.
 */

int lm_prepare_lockspace_sanlock(struct lockspace *ls, uint64_t *prev_generation, int repair)
{
	struct stat st;
	struct lm_sanlock *lms = NULL;
	char lock_lv_name[MAX_ARGS+1];
	char lsname[SANLK_NAME_LEN + 1] = { 0 };
	char disk_path[SANLK_PATH_LEN];
	char killpath[SANLK_PATH_LEN];
	char killargs[SANLK_PATH_LEN];
	struct sanlk_host hs = { 0 };
	uint32_t ss_flags = 0;
	uint32_t rs_flags = 0;
	int sector_size = 0;
	int align_size = 0;
	int align_mb = 0;
	int retries = 0;
	int gl_found;
	int ret, rv;

	memset(disk_path, 0, sizeof(disk_path));
	memset(lock_lv_name, 0, sizeof(lock_lv_name));

	/*
	 * Construct the path to lvmlockctl by using the path to the lvm binary
	 * and appending "lockctl" to get /path/to/lvmlockctl.
	 */
	memset(killpath, 0, sizeof(killpath));
	snprintf(killpath, SANLK_PATH_LEN, "%slockctl", LVM_PATH);

	memset(killargs, 0, sizeof(killargs));
	snprintf(killargs, SANLK_PATH_LEN, "--kill %s", ls->vg_name);

	log_debug("S %s prepare_lockspace_san host_id %u repair %d", ls->name, ls->host_id, repair);

	rv = check_args_version(ls->vg_args, VG_LOCK_ARGS_MAJOR);
	if (rv < 0) {
		ret = -EARGS;
		goto fail;
	}

	rv = lock_lv_name_from_args(ls->vg_args, lock_lv_name);
	if (rv < 0) {
		log_error("S %s prepare_lockspace_san lock_lv_name_from_args error %d %s",
			  ls->name, rv, ls->vg_args);
		ret = -EARGS;
		goto fail;
	}

	if ((ret = build_dm_path(disk_path, SANLK_PATH_LEN, ls->vg_name, lock_lv_name)))
		goto fail;

	/*
	 * When a vg is started, the internal sanlock lv should be
	 * activated before lvmlockd is asked to add the lockspace.
	 * (sanlock needs to use the lv.)
	 *
	 * In the future we might be able to ask something on the system
	 * to activate the sanlock lv from here, and with that we might be
	 * able to start sanlock VGs without requiring a
	 * vgchange --lock-start command.
	 */

	/* FIXME: device is not always ready for us here */
	sleep(1);

	rv = stat(disk_path, &st);
	if (rv < 0) {
		log_error("S %s prepare_lockspace_san stat error %d disk_path %s",
			  ls->name, errno, disk_path);
		ret = -EARGS;
		goto fail;
	}

	if (!ls->host_id) {
		if (daemon_host_id)
			ls->host_id = daemon_host_id;
		else if (daemon_host_id_file)
			ls->host_id = read_host_id_file();
	}

	if (!ls->host_id || ls->host_id > 2000) {
		log_error("S %s prepare_lockspace_san invalid host_id %llu",
			  ls->name, (unsigned long long)ls->host_id);
		ret = -EHOSTID;
		goto fail;
	}

	lms = zalloc(sizeof(struct lm_sanlock));
	if (!lms) {
		ret = -ENOMEM;
		goto fail;
	}

	dm_strncpy(lsname, ls->name, sizeof(lsname));

	memcpy(lms->ss.name, lsname, SANLK_NAME_LEN);
	lms->ss.host_id_disk.offset = 0;
	lms->ss.host_id = ls->host_id;
	memcpy(lms->ss.host_id_disk.path, disk_path, SANLK_PATH_LEN-1);

	if (daemon_test) {
		if (!gl_lsname_sanlock[0]) {
			strncpy(gl_lsname_sanlock, lsname, MAX_NAME);
			log_debug("S %s prepare_lockspace_san use global lock", lsname);
		}
		lms->align_size = ONE_MB;
		lms->sector_size = 512;
		goto out;
	}

	lms->sock = sanlock_register();
	if (lms->sock < 0) {
		log_error("S %s prepare_lockspace_san register error %d", lsname, lms->sock);
		lms->sock = 0;
		ret = -EMANAGER;
		goto fail;
	}

	log_debug("S %s prepare_lockspace_san set killpath to %s %s", lsname, killpath, killargs);

	rv = sanlock_killpath(lms->sock, 0, killpath, killargs);
	if (rv < 0) {
		log_error("S %s killpath error %d", lsname, rv);
		ret = -EMANAGER;
		goto fail;
	}

	rv = sanlock_restrict(lms->sock, SANLK_RESTRICT_SIGKILL);
	if (rv < 0) {
		log_error("S %s restrict error %d", lsname, rv);
		ret = -EMANAGER;
		goto fail;
	}

#if LOCKDSANLOCK_SUPPORT >= 410
 repair_retry:
#endif
	sector_size = 0;
	align_size = 0;

	rv = read_lockspace_info(disk_path, lms->ss.host_id, &sector_size, &align_size, &align_mb, &ss_flags, &rs_flags, &hs);

#if LOCKDSANLOCK_SUPPORT >= 410
	if ((rv == -ELOCKREPAIR) && repair && !retries) {
		uint64_t generation = 0;
		uint32_t host_id = 0;

		rv = read_info_file(ls, &host_id, &generation, &sector_size, &align_size);
		if (rv < 0) {
			log_error("S %s prepare_lockspace_san cannot repair lockspace no info file", lsname);
			ret = -EINVAL;
		}

		if (host_id != lms->ss.host_id) {
			log_error("S %s prepare_lockspace_san cannot repair lockspace other info host_id", lsname);
			ret = -EINVAL;
		}

		hs.generation = generation;

		if (sector_size == 512) {
			lms->ss.flags |= SANLK_LSF_SECTOR512;
			lms->ss.flags |= SANLK_LSF_ALIGN1M;
		} else if (sector_size == 4096) {
			lms->ss.flags |= SANLK_LSF_SECTOR4K;
			if (align_size == ONE_MB)
				lms->ss.flags |= SANLK_LSF_ALIGN1M;
			else if (align_size == 2 * ONE_MB)
				lms->ss.flags |= SANLK_LSF_ALIGN2M;
			else if (align_size == 4 * ONE_MB)
				lms->ss.flags |= SANLK_LSF_ALIGN4M;
			else if (align_size == 8 * ONE_MB)
				lms->ss.flags |= SANLK_LSF_ALIGN8M;
		} else {
			log_error("S %s prepare_lockspace_san cannot repair lockspace invalid sector_size", lsname);
			ret = -EINVAL;
		}

		log_debug("S %s prepare_lockspace_san repair host %u lease", lsname, host_id);

		rv = sanlock_init_lockspace_host(&lms->ss, NULL, generation, 0, 0, 0);
		if (rv < 0) {
			log_error("S %s prepare_lockspace_san repair host %u lease error %d", lsname, host_id, rv);
			ret = -EMANAGER;
		}
		retries = 1;
		goto repair_retry;
	}
#else
	if ((rv == -ELOCKREPAIR) && repair && !retries)
		log_debug("S %s prepare_lockspace_san sanlock does not support repair.", lsname);
#endif

	if (rv < 0) {
		log_error("S %s prepare_lockspace_san cannot read lockspace info %d", lsname, rv);
		ret = -EMANAGER;
		goto fail;
	}

	*prev_generation = hs.generation;

	if (!sector_size) {
		log_debug("S %s prepare_lockspace_san using old size method", lsname);
		/* use the old method */
		align_size = sanlock_align(&lms->ss.host_id_disk);
		if (align_size <= 0) {
			log_error("S %s prepare_lockspace_san align error %d", lsname, align_size);
			ret = -EINVAL;
			goto fail;
		}
		sector_size = (align_size == ONE_MB) ? 512 : 4096;
		log_debug("S %s prepare_lockspace_san found old sector_size %d align_size %d", lsname, sector_size, align_size);
	}

	log_debug("S %s prepare_lockspace_san sector_size %d align_mb %d align_size %d prev_generation %llu",
		  lsname, sector_size, align_mb, align_size, (unsigned long long)hs.generation);

	if (sector_size == 4096) {
		if (((align_mb == 1) && (ls->host_id > 250)) ||
		    ((align_mb == 2) && (ls->host_id > 500)) ||
		    ((align_mb == 4) && (ls->host_id > 1000)) ||
		    ((align_mb == 8) && (ls->host_id > 2000))) {
			log_error("S %s prepare_lockspace_san invalid host_id %u for align %d MiB",
				  lsname, ls->host_id, align_mb);
			ret = -EHOSTID;
			goto fail;
		}
	}

	lms->align_size = align_size;
	lms->sector_size = sector_size;
	lms->ss_flags = ss_flags;
	lms->rs_flags = rs_flags;

	lms->ss.flags = ss_flags;

	gl_found = gl_is_enabled(ls, lms);
	if (gl_found < 0) {
		log_error("S %s prepare_lockspace_san gl_enabled error %d", lsname, gl_found);
		ret = -EARGS;
		goto fail;
	}

	ls->sanlock_gl_enabled = gl_found;

	if (gl_found) {
		if (gl_use_dlm) {
			log_error("S %s prepare_lockspace_san gl_use_dlm is set", lsname);
		} else if (gl_lsname_sanlock[0] && strcmp(gl_lsname_sanlock, lsname)) {
			log_error("S %s prepare_lockspace_san multiple sanlock global locks current %s",
				  lsname, gl_lsname_sanlock);
		} else {
			strncpy(gl_lsname_sanlock, lsname, MAX_NAME);
			log_debug("S %s prepare_lockspace_san use global lock %s",
				  lsname, gl_lsname_sanlock);
		}
	}

out:
	ls->lm_data = lms;
	log_debug("S %s prepare_lockspace_san done", lsname);
	return 0;

fail:
	if (lms && lms->sock)
		_close(lms->sock);
	free(lms);
	return ret;
}

int lm_add_lockspace_sanlock(struct lockspace *ls, int adopt_only, int adopt_ok, int nodelay)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct sanlk_host *hs = NULL;
	uint32_t flags = 0;
	int count = 0;
	int rv;

	if (daemon_test) {
		sleep(2);
		goto out;
	}

	if (nodelay)
		flags |= SANLK_ADD_NODELAY;

	rv = sanlock_add_lockspace_timeout(&lms->ss, flags, sanlock_io_timeout);
	if (rv == -EEXIST && (adopt_ok || adopt_only)) {
		/* We could alternatively just skip the sanlock call for adopt. */
		log_debug("S %s add_lockspace_san adopt found ls", ls->name);
		goto out;
	}
	if ((rv != -EEXIST) && adopt_only) {
		log_error("S %s add_lockspace_san add_lockspace adopt_only not found", ls->name);
		goto fail;
	}
	if (rv < 0) {
		/* retry for some errors? */
		log_error("S %s add_lockspace_san add_lockspace error %d", ls->name, rv);
		goto fail;
	}

	rv = sanlock_get_hosts(ls->name, 0, &hs, &count, SANLK_GET_HOST_LOCAL);
	if (rv < 0) {
		log_error("S %s add_lockspace_san add_lockspace get_host error %d", ls->name, rv);
		goto fail_rem;
	}

	if (!count || !hs) {
		log_error("S %s add_lockspace_san add_lockspace get_host empty", ls->name);
		goto fail_rem;
	}

	if ((uint32_t)hs->host_id != ls->host_id) {
		log_error("S %s add_lockspace_san add_lockspace get_host invalid host_id", ls->name);
		goto fail_rem;
	}

	if (!hs->generation) {
		log_error("S %s add_lockspace_san add_lockspace get_host zero generation", ls->name);
		goto fail_rem;
	}

	ls->generation = hs->generation;

	free(hs);

	write_info_file(ls);

	/*
	 * Don't let the lockspace be cleanly released if orphan locks
	 * exist, because the orphan locks are still protecting resources
	 * that are being used on the host, e.g. active lvs.  If the
	 * lockspace is cleanly released, another host could acquire the
	 * orphan leases.
	 */

	rv = sanlock_set_config(ls->name, 0, SANLK_CONFIG_USED_BY_ORPHANS, NULL);
	if (rv < 0) {
		log_error("S %s add_lockspace_san set_config error %d", ls->name, rv);
		goto fail_rem;
	}

out:
	log_debug("S %s add_lockspace_san done", ls->name);
	return 0;

fail_rem:
	sanlock_rem_lockspace(&lms->ss, 0);
fail:
	_close(lms->sock);
	free(lms);
	ls->lm_data = NULL;
	return rv;
}

int lm_rem_lockspace_sanlock(struct lockspace *ls, int free_vg)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	int rv;

	if (daemon_test)
		goto out;

	rv = sanlock_rem_lockspace(&lms->ss, 0);
	if (rv < 0) {
		log_error("S %s rem_lockspace_san error %d", ls->name, rv);
		return rv;
	}

	if (free_vg) {
		/*
		 * Destroy sanlock lockspace (delta leases).  Forces failure for any
		 * other host that is still using or attempts to use this lockspace.
		 * This shouldn't be generally necessary, but there may some races
		 * between nodes starting and removing a vg which this could help.
		 */
		strcpy_name_len(lms->ss.name, "#unused", SANLK_NAME_LEN);

		rv = sanlock_write_lockspace(&lms->ss, 0, 0, sanlock_io_timeout);
		if (rv < 0) {
			log_error("S %s rem_lockspace free_vg write_lockspace error %d %s",
				  ls->name, rv, lms->ss.host_id_disk.path);
		}

		remove_info_file(ls);
	}

	_close(lms->sock);
out:
	free(lms);
	ls->lm_data = NULL;

	/* FIXME: should we only clear gl_lsname when doing free_vg? */

	if (!strcmp(ls->name, gl_lsname_sanlock))
		memset(gl_lsname_sanlock, 0, sizeof(gl_lsname_sanlock));

	return 0;
}

int lm_add_resource_sanlock(struct lockspace *ls, struct resource *r)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct rd_sanlock *rds = (struct rd_sanlock *)r->lm_data;

	strcpy_name_len(rds->rs.lockspace_name, ls->name, SANLK_NAME_LEN);
	strcpy_name_len(rds->rs.name, r->name, SANLK_NAME_LEN);
	rds->rs.num_disks = 1;
	memcpy(rds->rs.disks[0].path, lms->ss.host_id_disk.path, SANLK_PATH_LEN);
	rds->rs.flags = lms->rs_flags;

	if (r->type == LD_RT_GL)
		rds->rs.disks[0].offset = GL_LOCK_BEGIN * lms->align_size;
	else if (r->type == LD_RT_VG)
		rds->rs.disks[0].offset = VG_LOCK_BEGIN * lms->align_size;

	/* LD_RT_LV offset is set in each lm_lock call from lv_args. */

	/*
	 * Disable sanlock lvb since lock versions are not currently used for
	 * anything, and it's nice to avoid the extra i/o used for lvb's.
	 */
#if LVMLOCKD_USE_SANLOCK_LVB
	if (r->type == LD_RT_GL || r->type == LD_RT_VG) {
		rds->vb = zalloc(sizeof(struct val_blk));
		if (!rds->vb)
			return -ENOMEM;
	}
#endif
	return 0;
}

int lm_rem_resource_sanlock(struct lockspace *ls, struct resource *r)
{
	struct rd_sanlock *rds = (struct rd_sanlock *)r->lm_data;

	/* FIXME: assert r->mode == UN or unlock if it's not? */
#ifdef LVMLOCKD_USE_SANLOCK_LVB
	free(rds->vb);
#endif
	memset(rds, 0, sizeof(struct rd_sanlock));
	r->lm_init = 0;
	return 0;
}

static const char *_host_flags_to_str(uint32_t flags)
{
	int val = flags & SANLK_HOST_MASK;

	if (val == SANLK_HOST_FREE)
		return "FREE";
	if (val == SANLK_HOST_LIVE)
		return "LIVE";
	if (val == SANLK_HOST_FAIL)
		return "FAIL";
	if (val == SANLK_HOST_DEAD)
		return "DEAD";
	if (val == SANLK_HOST_UNKNOWN)
		return "UNKNOWN";
	return "ERROR";
}

int lm_lock_sanlock(struct lockspace *ls, struct resource *r, int ld_mode,
		    struct val_blk *vb_out, int *retry, struct owner *owner,
		    int adopt_only, int adopt_ok, int repair)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct rd_sanlock *rds = (struct rd_sanlock *)r->lm_data;
	struct sanlk_resource *rs;
	struct sanlk_options opt;
	struct sanlk_host owner_host = { 0 };
	char *owner_name = NULL;
	uint64_t lock_lv_offset;
	uint32_t flags = 0;
	struct val_blk vb = { 0 };
	int added = 0;
	int retries = 0;
	int rv;

	if (!r->lm_init) {
		rv = lm_add_resource_sanlock(ls, r);
		if (rv < 0)
			return rv;
		r->lm_init = 1;
		added = 1;
	}

	rs = &rds->rs;

	/*
	 * While there are duplicate global locks, keep checking
	 * to see if any have been disabled.
	 */
	if (sanlock_gl_dup && ls->sanlock_gl_enabled &&
	    (r->type == LD_RT_GL || r->type == LD_RT_VG))
		ls->sanlock_gl_enabled = gl_is_enabled(ls, ls->lm_data);

	if (r->type == LD_RT_LV) {
		/*
		 * The lv may have been removed and recreated with a new lease
		 * offset, so we need to get the offset from lv_args each time
		 * instead of reusing the value that we last set in rds->rs.
		 * act->lv_args is copied to r->lv_args before every lm_lock().
		 */

		rv = check_args_version(r->lv_args, LV_LOCK_ARGS_MAJOR);
		if (rv < 0) {
			log_error("%s:%s lock_san wrong lv_args version %s",
				  ls->name, r->name, r->lv_args);
			return rv;
		}

		rv = lock_lv_offset_from_args(r->lv_args, &lock_lv_offset);
		if (rv < 0) {
			log_error("%s:%s lock_san lv_offset_from_args error %d %s",
				  ls->name, r->name, rv, r->lv_args);
			return rv;
		}

		if (!added && (rds->rs.disks[0].offset != lock_lv_offset)) {
			log_debug("%s:%s lock_san offset old %llu new %llu",
				  ls->name, r->name,
				  (unsigned long long)rds->rs.disks[0].offset,
				  (unsigned long long)lock_lv_offset);
		}

		rds->rs.disks[0].offset = lock_lv_offset;
	}

	if (ld_mode == LD_LK_SH) {
		rs->flags |= SANLK_RES_SHARED;
	} else if (ld_mode == LD_LK_EX) {
		rs->flags &= ~SANLK_RES_SHARED;
	} else {
		log_error("lock_san invalid mode %d", ld_mode);
		return -EINVAL;
	}

	/*
	 * Use PERSISTENT because if lvmlockd exits while holding
	 * a lock, it's not safe to simply clear/drop the lock while
	 * a command or lv is using it.
	 */

	rs->flags |= SANLK_RES_PERSISTENT;

	log_debug("%s:%s lock_san %s at %s:%llu",
		  ls->name, r->name, mode_str(ld_mode), rs->disks[0].path,
		  (unsigned long long)rs->disks[0].offset);

	if (daemon_test) {
		if (rds->vb) {
			vb_out->version = le16toh(rds->vb->version);
			vb_out->flags = le16toh(rds->vb->flags);
			vb_out->r_version = le32toh(rds->vb->r_version);
		}
		return 0;
	}

	if (rds->vb)
		flags |= SANLK_ACQUIRE_LVB;
	if (adopt_only)
		flags |= SANLK_ACQUIRE_ORPHAN_ONLY;
	if (adopt_ok)
		flags |= SANLK_ACQUIRE_ORPHAN;

	/*
	 * Don't block waiting for a failed lease to expire since it causes
	 * sanlock_acquire to block for a long time, which would prevent this
	 * thread from processing other lock requests.
	 */
	flags |= SANLK_ACQUIRE_OWNER_NOWAIT;

	memset(&opt, 0, sizeof(opt));
	sprintf(opt.owner_name, "%s", "lvmlockd");

 repair_retry:

#if LOCKDSANLOCK_SUPPORT >= 400
	rv = sanlock_acquire2(lms->sock, -1, flags, rs, &opt, &owner_host, &owner_name);
#else
	rv = sanlock_acquire(lms->sock, -1, flags, 1, &rs, &opt);
#endif

	/*
	 * errors: translate the sanlock error number to an lvmlockd error.
	 * We don't want to return an sanlock-specific error number from
	 * this function to code that doesn't recognize sanlock error numbers.
	 */

	if ((rv == -EMSGSIZE) && (r->type == LD_RT_LV)) {
		/*
		 * sanlock tried to read beyond the end of the device,
		 * so the offset of the lv lease is beyond the end of the
		 * device, which means that the lease lv was extended, and
		 * the lease for this lv was allocated in the new space.
		 * The lvm command will see this error, refresh the lvmlock
		 * lv, and try again.
		 */
		log_debug("%s:%s lock_san acquire offset %llu rv EMSGSIZE",
			  ls->name, r->name, (unsigned long long)rs->disks[0].offset);
		*retry = 0;
		return -EMSGSIZE;
	}

	if ((adopt_only || adopt_ok) && (rv == -EUCLEAN)) {
		/*
		 * The orphan lock exists but in a different mode than we asked
		 * for, so the caller should try again with the other mode.
		 */
		log_debug("%s:%s lock_san adopt mode %d try other mode",
			  ls->name, r->name, ld_mode);
		*retry = 0;
		return -EADOPT_RETRY;
	}

	if (adopt_only && (rv == -ENOENT)) {
		/*
		 * No orphan lock exists.
		 */
		log_debug("%s:%s lock_san adopt_only mode %d no orphan found",
			  ls->name, r->name, ld_mode);
		*retry = 0;
		return -EADOPT_NONE;
	}

	if (rv == SANLK_ACQUIRE_IDLIVE ||
	    rv == SANLK_ACQUIRE_OWNED ||
	    rv == SANLK_ACQUIRE_OTHER ||
	    rv == SANLK_ACQUIRE_OWNED_RETRY ||
	    rv == -EAGAIN) {

		/*
		 * EAGAIN: when a shared lock is held, and we request an ex lock.
		 *
		 * OWNED_RETRY: the lock is held by a failed but not yet dead host.
		 * Retrying will eventually find the host is dead (and the lock is
		 * granted), or another host has acquired it.
		 *
		 * Multiple hosts all requesting shared locks can also result in
		 * some transient errors here (shared locks involve acquiring the
		 * paxos lease ex for a short period, which means two hosts both
		 * requesting sh at once can cause one to fail here.)
		 * Retry here to attempt to cover these transient failures.
		 *
		 * The command also has its own configurable retry logic.
		 * The intention is to handle actual lock contention retries
		 * from the command, and the transient failures from concurrent
		 * shared requests here.  We don't actually know when a failure
		 * was related to the transient concurrent sh, so we just guess
		 * it was if we were requesting a sh lock.
		 */

		*retry = (ld_mode == LD_LK_SH) ? 1 : 0;

		if (rv == SANLK_ACQUIRE_OWNED_RETRY)
			*retry = 0;

		if (owner && owner_host.host_id) {
			const char *host_state;

			owner->host_id = (uint32_t)owner_host.host_id;
			owner->generation = (uint32_t)owner_host.generation;
			owner->timestamp = (uint32_t)owner_host.timestamp;

			if ((host_state = _host_flags_to_str(owner_host.flags)))
				dm_strncpy(owner->state, host_state, sizeof(owner->state));

			if (owner_name) {
				dm_strncpy(owner->name, owner_name, sizeof(owner->name));
				free(owner_name);
			}

			log_debug("%s:%s lock_san acquire mode %d lock held %d owner %u %u %u %s %s",
				  ls->name, r->name, ld_mode, rv,
				  owner->host_id, owner->generation, owner->timestamp,
				  owner->state, owner->name ?: "");
		} else {
			log_debug("%s:%s lock_san acquire mode %d lock held %d",
				  ls->name, r->name, ld_mode, rv);
		}
		return -EAGAIN;
	}

	if (rv == SANLK_AIO_TIMEOUT) {
		log_debug("%s:%s lock_san acquire mode %d io timeout", ls->name, r->name, ld_mode);
		*retry = 0;
		return -EIOTIMEOUT;
	}

	if (_lease_corrupt_error(rv) && repair && !retries) {
		log_debug("%s:%s lock_san acquire lease_corrupt %d repair", ls->name, r->name, rv);

		rv = sanlock_write_resource(rs, 0, 0, 0);
		if (rv < 0) {
			log_error("%s:%s lock_san acquire lease repair write_resource error %d", ls->name, r->name, rv);
			return rv;
		}
		retries = 1;
		goto repair_retry;
	}

	if (rv < 0) {
		log_error("%s:%s lock_san acquire error %d", ls->name, r->name, rv);

		/* if the gl has been disabled, remove and free the gl resource */
		if ((rv == SANLK_LEADER_RESOURCE) && (r->type == LD_RT_GL)) {
			if (!lm_gl_is_enabled(ls)) {
				log_error("%s:%s lock_san gl has been disabled",
					  ls->name, r->name);
				if (!strcmp(gl_lsname_sanlock, ls->name))
					memset(gl_lsname_sanlock, 0, sizeof(gl_lsname_sanlock));
				return -EUNATCH;
			}
		}

		if (added)
			lm_rem_resource_sanlock(ls, r);

		/* sanlock gets i/o errors trying to read/write the leases. */
		if (rv == -EIO ||
		    rv == SANLK_LEADER_READ ||
		    rv == SANLK_LEADER_WRITE ||
		    rv == SANLK_DBLOCK_READ ||
		    rv == SANLK_DBLOCK_WRITE)
			return -ELOCKIO;

		if (_lease_corrupt_error(rv)) {
			log_debug("%s:%s lock_san lease corrupt repair %d retries %d", ls->name, r->name, repair, retries);
			return -ELOCKREPAIR;
		}

		/*
		 * The sanlock lockspace can disappear if the lease storage fails,
		 * the delta lease renewals fail, the lockspace enters recovery,
		 * lvmlockd holds no leases in the lockspace, so sanlock can
		 * stop and free the lockspace.
		 */
		if (rv == -ENOSPC)
			return -ELOCKIO;

		/* The request conflicted with an orphan lock. */
		if (rv == -EUCLEAN)
			return -EORPHAN;

		/*
		 * generic error number for sanlock errors that we are not
		 * catching above.
		 */
		return -ELMERR;
	}

	/*
	 * sanlock acquire success (rv 0)
	 */

	if (rds->vb) {
		rv = sanlock_get_lvb(0, rs, (char *)&vb, sizeof(vb));
		if (rv < 0) {
			log_error("%s:%s lock_san get_lvb error %d", ls->name, r->name, rv);
			memset(rds->vb, 0, sizeof(struct val_blk));
			memset(vb_out, 0, sizeof(struct val_blk));
			/* the lock is still acquired, the vb values considered invalid */
			rv = 0;
			goto out;
		}

		/*
		 * 'vb' contains disk endian values, not host endian.
		 * It is copied directly to rrs->vb which is also kept
		 * in disk endian form.
		 * vb_out is returned to the caller in host endian form.
		 */

		memcpy(rds->vb, &vb, sizeof(vb));

		vb_out->version = le16toh(vb.version);
		vb_out->flags = le16toh(vb.flags);
		vb_out->r_version = le32toh(vb.r_version);
	}
out:
	return rv;
}

int lm_convert_sanlock(struct lockspace *ls, struct resource *r,
		       int ld_mode, uint32_t r_version)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct rd_sanlock *rds = (struct rd_sanlock *)r->lm_data;
	struct sanlk_resource *rs = &rds->rs;
	struct val_blk vb;
	uint32_t flags = 0;
	int rv;

	log_debug("%s:%s convert_san %s to %s",
		  ls->name, r->name, mode_str(r->mode), mode_str(ld_mode));

	if (daemon_test)
		goto rs_flag;

	if (rds->vb && r_version && (r->mode == LD_LK_EX)) {
		if (!rds->vb->version) {
			/* first time vb has been written */
			rds->vb->version = htole16(VAL_BLK_VERSION);
		}
		if (r_version)
			rds->vb->r_version = htole32(r_version);
		memcpy(&vb, rds->vb, sizeof(vb));

		log_debug("%s:%s convert_san set r_version %u",
			  ls->name, r->name, r_version);

		rv = sanlock_set_lvb(0, rs, (char *)&vb, sizeof(vb));
		if (rv < 0) {
			log_error("%s:%s convert_san set_lvb error %d",
				  ls->name, r->name, rv);
			return -ELMERR;
		}
	}

 rs_flag:
	if (ld_mode == LD_LK_SH)
		rs->flags |= SANLK_RES_SHARED;
	else
		rs->flags &= ~SANLK_RES_SHARED;

	if (daemon_test)
		return 0;

	/*
	 * Don't block waiting for a failed lease to expire since it causes
	 * sanlock_convert to block for a long time, which would prevent this
	 * thread from processing other lock requests.
	 *
	 * FIXME: SANLK_CONVERT_OWNER_NOWAIT is the same as SANLK_ACQUIRE_OWNER_NOWAIT.
	 * Change to use the CONVERT define when the latest sanlock version has it.
	 */
	flags |= SANLK_ACQUIRE_OWNER_NOWAIT;

	rv = sanlock_convert(lms->sock, -1, flags, rs);
	if (!rv)
		return 0;

	switch (rv) {
	case -EAGAIN:
	case SANLK_ACQUIRE_IDLIVE:
	case SANLK_ACQUIRE_OWNED:
	case SANLK_ACQUIRE_OWNED_RETRY:
	case SANLK_ACQUIRE_OTHER:
	case SANLK_AIO_TIMEOUT:
		/* expected errors from known/normal cases like lock contention or io timeouts */
		log_debug("%s:%s convert_san error %d", ls->name, r->name, rv);
		return -EAGAIN;
	default:
		log_error("%s:%s convert_san convert error %d", ls->name, r->name, rv);
		rv = -ELMERR;
	}

	return rv;
}

static int release_rename(struct lockspace *ls, struct resource *r)
{
	struct rd_sanlock rd1;
	struct rd_sanlock rd2;
	struct sanlk_resource *res1;
	struct sanlk_resource *res2;
	struct sanlk_resource **res_args;
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct rd_sanlock *rds = (struct rd_sanlock *)r->lm_data;
	int rv;

	log_debug("%s:%s release rename", ls->name, r->name);

	res_args = malloc(2 * sizeof(struct sanlk_resource *));
	if (!res_args)
		return -ENOMEM;

	rd1.rs = rds->rs;
	rd2.rs = rds->rs;

	res1 = &rd1.rs;
	res2 = &rd2.rs;

	if (memcmp(res1->name, r->name, SANLK_NAME_LEN))
		log_error("%s:%s unlock_san release rename bad name %.48s", ls->name, r->name, res1->name);

	strcpy_name_len(res2->name, "invalid_removed", SANLK_NAME_LEN);

	res_args[0] = res1;
	res_args[1] = res2;

	rv = sanlock_release(lms->sock, -1, SANLK_REL_RENAME, 2, res_args);
	if (rv < 0) {
		log_error("%s:%s unlock_san release rename error %d", ls->name, r->name, rv);
		rv = -ELMERR;
	}

	free(res_args);

	return rv;
}

/*
 * rds->vb is stored in le
 * 
 * r_version is r->version
 *
 * for GL locks lvmlockd just increments this value
 * each time the global lock is released from ex.
 *
 * for VG locks it is the seqno from the vg metadata.
 */

int lm_unlock_sanlock(struct lockspace *ls, struct resource *r,
		      uint32_t r_version, uint32_t lmu_flags)
{
	struct lm_sanlock *lms = (struct lm_sanlock *)ls->lm_data;
	struct rd_sanlock *rds = (struct rd_sanlock *)r->lm_data;
	struct sanlk_resource *rs = &rds->rs;
	struct val_blk vb;
	int rv;

	log_debug("%s:%s unlock_san %s r_version %u flags %x",
		  ls->name, r->name, mode_str(r->mode), r_version, lmu_flags);

	if (daemon_test) {
		if (rds->vb && r_version && (r->mode == LD_LK_EX)) {
			if (!rds->vb->version)
				rds->vb->version = htole16(VAL_BLK_VERSION);
			if (r_version)
				rds->vb->r_version = htole32(r_version);
		}
		return 0;
	}

	if (rds->vb && r_version && (r->mode == LD_LK_EX)) {
		if (!rds->vb->version) {
			/* first time vb has been written */
			rds->vb->version = htole16(VAL_BLK_VERSION);
		}
		if (r_version)
			rds->vb->r_version = htole32(r_version);
		memcpy(&vb, rds->vb, sizeof(vb));

		log_debug("%s:%s unlock_san set r_version %u",
			  ls->name, r->name, r_version);

		rv = sanlock_set_lvb(0, rs, (char *)&vb, sizeof(vb));
		if (rv < 0) {
			log_error("%s:%s unlock_san set_lvb error %d",
				  ls->name, r->name, rv);
			return -ELMERR;
		}
	}

	/*
	 * For vgremove (FREE_VG) we unlock-rename the vg and gl locks
	 * so they cannot be reacquired.
	 */
	if ((lmu_flags & LMUF_FREE_VG) &&
	    (r->type == LD_RT_GL || r->type == LD_RT_VG)) {
		return release_rename(ls, r);
	}

	rv = sanlock_release(lms->sock, -1, 0, 1, &rs);
	if (rv < 0)
		log_error("%s:%s unlock_san release error %d", ls->name, r->name, rv);

	/*
	 * sanlock may return an error here if it fails to release the lease on
	 * disk because of an io timeout.  But, sanlock will continue trying to
	 * release the lease after this call returns.  We shouldn't return an
	 * error here which would result in lvmlockd-core keeping the lock
	 * around.  By releasing the lock in lvmlockd-core at this point,
	 * lvmlockd may send another acquire request to lvmlockd.  If sanlock
	 * has not been able to release the previous instance of the lock yet,
	 * then it will return an error for the new request.  But, acquiring a
	 * new lock is able o fail gracefully, until sanlock is finally able to
	 * release the old lock.
	 */

	return 0;
}

int lm_vg_status_sanlock(struct lockspace *ls, struct action *act)
{
	struct sanlk_host *hs = NULL;
	const char *host_state;
	int count = 0;
	int rv;

	if (daemon_test)
		return 0;

	rv = sanlock_get_hosts(ls->name, 0, &hs, &count, SANLK_GET_HOST_LOCAL);
	if (rv < 0) {
		log_error("S %s vg_status_san get_hosts error %d", ls->name, rv);
		return rv;
	}

	if (!count || !hs) {
		log_error("S %s vg_status_san get_hosts empty", ls->name);
		return -EINVAL;
	}

	act->owner.host_id = (uint32_t)hs->host_id;
	act->owner.generation = (uint32_t)hs->generation;
	act->owner.timestamp = (uint32_t)hs->timestamp;

	if ((host_state = _host_flags_to_str(hs->flags)))
		dm_strncpy(act->owner.state, host_state, sizeof(act->owner.state));

	free(hs);
	return 0;
}

/*
 * On error, returns < 0
 * Else:
 * If other hosts are found, returns the number.
 * If no other hosts are found (only ourself), returns 0.
 */

int lm_hosts_sanlock(struct lockspace *ls, int notify)
{
	struct sanlk_host *hss = NULL;
	struct sanlk_host *hs;
	uint32_t state;
	int hss_count = 0;
	int found_self = 0;
	int found_others = 0;
	int i, rv;

	if (daemon_test)
		return 0;

	rv = sanlock_get_hosts(ls->name, 0, &hss, &hss_count, 0);

	if (rv == -EAGAIN) {
		/*
		 * No host info is available yet (perhaps lockspace was
		 * just started so other host state is unknown.)  Pretend
		 * there is one other host (busy).
		 */
		log_debug("S %s hosts_san no info, retry later", ls->name);
		return 1;
	}

	if (rv < 0) {
		log_error("S %s hosts_san get_hosts error %d", ls->name, rv);
		return -1;
	}

	if (!hss || !hss_count) {
		log_error("S %s hosts_san zero hosts", ls->name);
		return -1;
	}

	hs = hss;

	for (i = 0; i < hss_count; i++) {
		log_debug("S %s hosts_san host_id %llu gen %llu flags %x",
			  ls->name,
			  (unsigned long long)hs->host_id,
			  (unsigned long long)hs->generation,
			  hs->flags);

		if ((uint32_t)hs->host_id == ls->host_id) {
			found_self = 1;
			hs++;
			continue;
		}

		state = hs->flags & SANLK_HOST_MASK;
		if ((state == SANLK_HOST_LIVE) || (state == SANLK_HOST_UNKNOWN))
			found_others++;
		hs++;
	}
	free(hss);

	if (found_others && notify) {
		/*
		 * We could use the sanlock event mechanism to notify lvmlockd
		 * on other hosts to stop this VG.  lvmlockd would need to
		 * register for and listen for sanlock events in the main loop.
		 * The events are slow to propagate.  We'd need to retry for a
		 * while before all the hosts see the event and stop the VG.
		 * sanlock_set_event(ls->name, &he, SANLK_SETEV_ALL_HOSTS);
		 *
		 * Wait to try this until there appears to be real value/interest
		 * in doing it.
		 */
	}

	if (!found_self) {
		log_error("S %s hosts_san self not found others %d", ls->name, found_others);
		return -1;
	}

	return found_others;
}

int lm_get_lockspaces_sanlock(struct list_head *ls_rejoin)
{
	struct sanlk_lockspace *ss_all = NULL;
	struct sanlk_lockspace *ss;
	struct lockspace *ls;
	int ss_count = 0;
	int i, rv;

	rv = sanlock_get_lockspaces(&ss_all, &ss_count, 0);
	if (rv < 0)
		return rv;

	if (!ss_all || !ss_count)
		return 0;

	ss = ss_all;

	for (i = 0; i < ss_count; i++) {

		if (strncmp(ss->name, LVM_LS_PREFIX, strlen(LVM_LS_PREFIX)))
			continue;

		if (!(ls = alloc_lockspace()))
			return -ENOMEM;

		ls->lm_type = LD_LM_SANLOCK;
		ls->host_id = ss->host_id;
		memcpy(ls->name, ss->name, SANLK_NAME_LEN);
		memcpy(ls->vg_name, ss->name + strlen(LVM_LS_PREFIX), SANLK_NAME_LEN - strlen(LVM_LS_PREFIX));
		list_add_tail(&ls->list, ls_rejoin);

		ss++;
	}

	free(ss_all);
	return 0;
}

int lm_is_running_sanlock(void)
{
	uint32_t daemon_version;
	uint32_t daemon_proto;
	int rv;

	if (daemon_test)
		return gl_use_sanlock;

	rv = sanlock_version(0, &daemon_version, &daemon_proto);
	if (rv < 0)
		return 0;
	return 1;
}

