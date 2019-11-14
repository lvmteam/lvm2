/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
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

/*
 * There are four different ways that commands handle hints:
 *
 * 1. Commands that use hints to reduce scanning, and create new
 *    hints when needed:
 *
 *    fullreport, lvchange, lvcreate, lvdisplay, lvremove, lvresize,
 *    lvs, pvdisplay, lvpoll, pvs, vgchange, vgck, vgdisplay, vgs,
 *    lvextend, lvreduce, lvrename
 *
 * 2. Commands that just remove existing hints:
 *
 *    pvcreate, pvremove, vgcreate, vgremove, vgextend, vgreduce,
 *    vgcfgrestore, vgimportclone, vgmerge, vgsplit, pvchange
 *
 * 3. Commands that ignore hints:
 *
 *    lvconvert, lvmdiskscan, lvscan, pvresize, pvck, pvmove, pvscan,
 *    vgcfgbackup, vgexport, vgimport, vgscan, pvs -a, pvdisplay -a
 *
 * 4. Command that removes existing hints and creates new hints:
 *
 *    pvscan --cache
 *
 *
 * For 1, hints are used to reduce scanning by:
 * . get the list of all devices on the system from dev_cache_scan()
 * . remove devices from that list which are not listed in hints
 * . do scan the remaining list of devices
 *
 * label_scan() is where those steps are implemented:
 *      . dev_cache_scan() produces all_devs list
 *      . get_hints(all_devs, scan_devs, &newhints)
 *        moves some devs from all_devs to scan_devs list (or sets newhints
 *        if no hints are applied, and a new hints file should be created)
 *      . _scan_list(scan_devs) does the label scan
 *      . if newhints was set, call write_hint_file() to create new hints
 *        based on which devs _scan_list saw an lvm label on
 *
 * For 2, commands that change "global state" remove existing hints.
 * The hints become incorrect as a result of the changes the command
 * is making. "global state" is lvm state that is not isolated within a VG.
 * (This is basically: which devices are PVs, and which VG names are used.)
 *
 * Commands that change global state do not create new hints because
 * it's much simpler to create hints based solely on the result of a
 * full standard label scan, i.e. which devices had an lvm label.
 * (It's much more complicated to create hints based on making specific
 * changes to existing hints based on what the command has changed.)
 *
 * For 3, these commands are a combination of: uncommon commands that
 * don't need optimization, commands where the purpose is to read all
 * devices, commands dealing with global state where it's important to
 * not miss anything, commands where it's safer to know everything.
 *
 * For 4, this is the traditional way of forcing any locally cached
 * state to be cleared and regenerated.  This would be used to reset
 * hints after doing something that invalidates the hints in a way
 * that lvm couldn't detect itself, e.g. using dd to copy a PV to
 * a non-PV device.  (A user could also just rm /run/lvm/hints in
 * place of running pvscan --cache.)
 *
 *
 * Creating hints:
 *
 * A command in list 1 above calls get_hints() to try to read the
 * hints file.  get_hints() will sometimes not return any hints, in
 * which case the label_scan will scan all devices.  This happens if:
 *
 * a. the /run/lvm/hints file does not exist *
 * b. the /run/lvm/hints file is empty *
 * c. the /run/lvm/hints file content is not applicable *
 * d. the /run/lvm/newhints file exists *
 * e. the /run/lvm/nohints file exists
 * f. a shared nonblocking flock on /run/lvm/hints fails
 *
 * When get_hints(all_devs, scan_devs, &newhints) does not find hints to use,
 * it will sometimes set "newhints" so that the command will create a new
 * hints file after scanning all the devs.  [* These commands create a
 * new hint file after scanning.]
 *
 * After scanning a dev list that was reduced by applying hints, label_scan
 * calls validate_hints() to check if the hints were consistent with what
 * the scan saw on the devs.  Sometimes it's not, in which case the command
 * then scans the remaining devs, and creates /run/lvm/newhints to signal
 * to the next command that it should create new hints.
 *
 * Causes of each case above:
 * a) First command run, or a user removed the file
 * b) A command from list 2 cleared the hint file
 * c) See below
 * d) Another command from list 1 found invalid hints after scanning.
 *    A command from list 2 also creates a newhints file in addition
 *    to clearing the hint file.
 * e) A command from list 2 is blocking other commands from using
 *    hints while it makes global changes.
 * f) A command from list 2 is holding the ex flock to block
 *    other commands from using hints while it makes global changes.
 *
 * The content of the hint file is ignored and invalidated in get_hints if:
 *
 * . The lvm.conf filters or scan_lvs setting used by the command that
 *   created the hints do not match the settings used by this command.
 *   When these settings change, different PVs can become visible,
 *   making previous hints invalid.
 *
 * . The list of devices on the system changes.  When a new device
 *   appears on the system, it may have a PV that was not not around
 *   when the hints were created, and it needs to be scanned.
 *   (A hash of all dev names on the system is used to detect when
 *   the list of devices changes and hints need to be recreated.)
 *
 * The hint file is invalidated in validate_hints if:
 *
 * . The devs in the hint file have a different PVID or VG name
 *   than what was seen during the scan.
 *
 * . Duplicate PVs were seen in the scan.
 *
 * . Others may be added.
 *
 */

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/label/label.h"
#include "lib/misc/crc.h"
#include "lib/mm/xlate.h"
#include "lib/cache/lvmcache.h"
#include "lib/device/bcache.h"
#include "lib/commands/toolcontext.h"
#include "lib/activate/activate.h"
#include "lib/label/hints.h"
#include "lib/device/dev-type.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/sysmacros.h>

static const char *_hints_file = DEFAULT_RUN_DIR "/hints";
static const char *_nohints_file = DEFAULT_RUN_DIR "/nohints";
static const char *_newhints_file = DEFAULT_RUN_DIR "/newhints";

/*
 * Format of hints file.  Increase the major number when
 * making a change to the hint file format that older lvm
 * versions can't use.  Older lvm versions will not try to
 * use the hint file if the major number in it is larger
 * than they were built with.  Increase the minor number
 * when adding features that older lvm versions can just
 * ignore while continuing to use the other content.
 */
#define HINTS_VERSION_MAJOR 1
#define HINTS_VERSION_MINOR 1

#define HINT_LINE_LEN (PATH_MAX + NAME_LEN + ID_LEN + 64)
#define HINT_LINE_WORDS 4
static char _hint_line[HINT_LINE_LEN];

static int _hints_fd = -1;

#define NONBLOCK 1

#define NEWHINTS_NONE     0
#define NEWHINTS_FILE     1
#define NEWHINTS_INIT     2
#define NEWHINTS_REFRESH  3
#define NEWHINTS_EMPTY    4

static int _hints_exists(void)
{
	struct stat buf;

	if (!stat(_hints_file, &buf))
		return 1;
	if (errno != ENOENT)
		log_debug("hints_exist errno %d", errno);
	return 0;
}

static int _nohints_exists(void)
{
	struct stat buf;

	if (!stat(_nohints_file, &buf))
		return 1;
	if (errno != ENOENT)
		log_debug("nohints_exist errno %d", errno);
	return 0;
}

static int _newhints_exists(void)
{
	struct stat buf;

	if (!stat(_newhints_file, &buf))
		return 1;
	if (errno != ENOENT)
		log_debug("newhints_exist errno %d", errno);
	return 0;
}

static int _touch_newhints(void)
{
	FILE *fp;

	if (!(fp = fopen(_newhints_file, "w")))
		return_0;
	if (fclose(fp))
		stack;
	return 1;
}

static int _touch_nohints(void)
{
	FILE *fp;

	if (!(fp = fopen(_nohints_file, "w")))
		return_0;
	if (fclose(fp))
		stack;
	return 1;
}

static int _touch_hints(void)
{
	FILE *fp;

	if (!(fp = fopen(_hints_file, "w")))
		return_0;
	if (fclose(fp))
		stack;
	return 1;
}

static void _unlink_nohints(void)
{
	if (unlink(_nohints_file))
		log_debug("unlink_nohints errno %d", errno);
}

static void _unlink_hints(void)
{
	if (unlink(_hints_file))
		log_debug("unlink_hints errno %d", errno);
}

static void _unlink_newhints(void)
{
	if (unlink(_newhints_file))
		log_debug("unlink_newhints errno %d", errno);
}

static int _clear_hints(struct cmd_context *cmd)
{
	FILE *fp;
	time_t t;

	if (!(fp = fopen(_hints_file, "w"))) {
		log_warn("Failed to clear hint file.");
		/* shouldn't happen, but try to unlink in case */
		_unlink_hints();
		return 0;
	}

	t = time(NULL);

	fprintf(fp, "# Created empty by %s pid %d %s", cmd->name, getpid(), ctime(&t));

	if (fflush(fp))
		log_debug("clear_hints flush errno %d", errno);

	if (fclose(fp))
		log_debug("clear_hints close errno %d", errno);

	return 1;
}

static int _lock_hints(struct cmd_context *cmd, int mode, int nonblock)
{
	int fd;
	int op = mode;
	int ret;

	if (cmd->nolocking)
		return 1;

	if (nonblock)
		op |= LOCK_NB;

	if (_hints_fd != -1) {
		log_warn("lock_hints existing fd %d", _hints_fd);
		return 0;
	}

	fd = open(_hints_file, O_RDWR);
	if (fd < 0) {
		log_debug("lock_hints open errno %d", errno);
		return 0;
	}


	ret = flock(fd, op);
	if (!ret) {
		_hints_fd = fd;
		return 1;
	}

	if (close(fd))
		stack;
	return 0;
}

static void _unlock_hints(struct cmd_context *cmd)
{
	int ret;

	if (cmd->nolocking)
		return;

	if (_hints_fd == -1) {
		log_warn("unlock_hints no existing fd");
		return;
	}

	ret = flock(_hints_fd, LOCK_UN);
	if (ret)
		log_warn("unlock_hints flock errno %d", errno);

	if (close(_hints_fd))
		stack;
	_hints_fd = -1;
}

void hints_exit(struct cmd_context *cmd)
{
	if (_hints_fd == -1)
		return;
	return _unlock_hints(cmd);
}

static struct hint *_find_hint_name(struct dm_list *hints, const char *name)
{
	struct hint *hint;

	dm_list_iterate_items(hint, hints) {
		if (!strcmp(hint->name, name))
			return hint;
	}
	return NULL;
}

/*
 * Decide if a given device name should be included in the hint hash.
 * If it is, then the hash changes if the device is added or removed
 * from the system, which causes the hints to be regenerated.
 * If it is not, then the device being added/removed from the system
 * does not change the hint hash, which means hints remain unchanged.
 *
 * If we know that lvm does not want to scan this device, then it should
 * be excluded from the hint hash.  If a dev is excluded by the regex
 * filter or by scan_lvs setting, then we know lvm doesn't want to scan
 * it, so when it is added/removed the scanning results won't change, and
 * we don't want to regenerate hints.
 *
 * One effect of this is that the regex filter and scan_lvs setting also
 * need to be saved in the hint file, since if those settings change,
 * it may impact what devs lvm wants to scan, and therefore change what
 * the hints are.
 *
 * We do not need or want to apply all filters to a device here.  The full
 * filters still determine if a device is scanned and used.  This is simply
 * used to decide if the device name should be included in the hash,
 * where the changing hash triggers hints to be recreated.  So, by
 * including a device here which is excluded by the real filters, the result is
 * simply that we could end up recreating hints more often than necessary,
 * which is not a problem.  Not recreating hints when we should is a bigger
 * problem, so it's best to include devices here if we're unsure.
 *
 * Any filter used here obviously cannot rely on reading the device, since
 * the whole point of the hints is to avoid reading the device.
 *
 * It's common for the system to include a device path for a disconnected
 * device and report zero size for it (e.g. a loop device).  When the
 * device is connected, a new device name doesn't appear, but the dev size
 * for the existing device is now reported as non-zero.  So, if a device
 * is connected/disconnected, changing the size from/to zero, it is
 * included/excluded in the hint hash.
 */

static int _dev_in_hint_hash(struct cmd_context *cmd, struct device *dev)
{
	uint64_t devsize = 0;

	if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "regex"))
		return 0;

	if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "type"))
		return 0;

	/* exclude LVs from hint accounting when scan_lvs is 0 */
	if (!cmd->scan_lvs && dm_is_dm_major(MAJOR(dev->dev)) && dev_is_lv(dev))
		return 0;

	if (!dev_get_size(dev, &devsize) || !devsize)
		return 0;

	return 1;
}

/*
 * Hints were used to reduce devs that were scanned.  After the reduced
 * scanning is done, this is called to check if the hints may have been
 * incorrect or insufficient, in which case we want to continue scanning all
 * the other (unhinted) devices, as would be done when no hints are used.
 * This should not generally happen, but is done in an attempt to catch
 * any unusual situations where the hints become incorrect from something
 * unexpected.
 */
int validate_hints(struct cmd_context *cmd, struct dm_list *hints)
{
	struct hint *hint;
	struct dev_iter *iter;
	struct device *dev;
	int ret = 1;

	/* No commands are using hints. */
	if (!cmd->enable_hints)
		return 0;

	/* This command does not use hints. */
	if (!cmd->use_hints && !cmd->pvscan_recreate_hints)
		return 0;

	if (lvmcache_has_duplicate_devs()) {
		log_debug("Hints not used with duplicate pvs");
		ret = 0;
		goto out;
	}

	if (lvmcache_found_duplicate_vgnames()) {
		log_debug("Hints not used with duplicate vg names");
		ret = 0;
		goto out;
	}

	/*
	 * Check that the PVID saved in the hint for each device matches the
	 * PVID that the scan found on the device.  If not, then the hints
	 * became stale somehow (e.g. manually copying devices with dd) and
	 * need to be refreshed.
	 */
	if (!(iter = dev_iter_create(NULL, 0)))
		return 0;
	while ((dev = dev_iter_get(cmd, iter))) {
		if (!(hint = _find_hint_name(hints, dev_name(dev))))
			continue;

		/* The cmd hasn't needed this hint's dev so it's not been scanned. */
		if (!hint->chosen)
			continue;

		if (strcmp(dev->pvid, hint->pvid)) {
			log_debug("Invalid hint device %d:%d %s pvid %s had hint pvid %s",
				  major(hint->devt), minor(hint->devt), dev_name(dev),
				  dev->pvid, hint->pvid);
			ret = 0;
		}
	}
	dev_iter_destroy(iter);

	/*
	 * Check in lvmcache to see if the scan noticed any missing PVs
	 * which might mean the hints left out a device that we should
	 * have scanned.
	 *
	 * FIXME: the scan cannot currently detect missing PVs.
	 * They are only detected in vg_read when the PVIDs listed
	 * in the metadata are looked for and not found.  This could
	 * be addressed by at least saving the number of expected PVs
	 * during the scan (in the summary), and then comparing that
	 * number with the number of PVs found in the hints listing
	 * that VG name.
	 */

	/*
	 * The scan placed a summary of each VG (vginfo) and PV (info)
	 * into lvmcache lists.  Check in lvmcache to see if the VG name
	 * for each PV matches the vgname saved in the hint for the PV.
	 */
	dm_list_iterate_items(hint, hints) {
		struct lvmcache_vginfo *vginfo;

		/* The cmd hasn't needed this hint's dev so it's not been scanned. */
		if (!hint->chosen)
			continue;

		if (!hint->vgname[0] || (hint->vgname[0] == '-'))
			continue;

		if (!(vginfo = lvmcache_vginfo_from_vgname(hint->vgname, NULL))) {
			log_debug("Invalid hint device %d:%d %s pvid %s had vgname %s no VG info.",
				  major(hint->devt), minor(hint->devt), hint->name,
				  hint->pvid, hint->vgname);
			ret = 0;
			continue;
		}

		if (!lvmcache_vginfo_has_pvid(vginfo, hint->pvid)) {
			log_debug("Invalid hint device %d:%d %s pvid %s had vgname %s no PV info.",
				  major(hint->devt), minor(hint->devt), hint->name,
				  hint->pvid, hint->vgname);
			ret = 0;
			continue;
		}
	}

out:
	if (!ret) {
		/*
		 * Force next cmd to recreate hints.  If we can't
		 * create newhints, the next cmd should get here
		 * like we have.  We don't use _clear_hints because
		 * we don't want to take an ex lock here.
		 */
		if (!_touch_newhints())
			stack;
	}

	return ret;
}

/*
 * For devs that match entries in hints, move them from devs_in to devs_out.
 */
static void _apply_hints(struct cmd_context *cmd, struct dm_list *hints,
		char *vgname, struct dm_list *devs_in, struct dm_list *devs_out)
{
	struct hint *hint;
	struct device_list *devl, *devl2;
	struct dm_list *name_list;
	struct dm_str_list *name_sl;

	dm_list_iterate_items_safe(devl, devl2, devs_in) {
		if (!(name_list = dm_list_first(&devl->dev->aliases)))
			continue;
		name_sl = dm_list_item(name_list, struct dm_str_list);

		if (!(hint = _find_hint_name(hints, name_sl->str)))
			continue;

		/* if vgname is set, pick hints with matching vgname */
		if (vgname && hint->vgname[0] && (hint->vgname[0] != '-')) {
			if (strcmp(vgname, hint->vgname))
				continue;
		}

		dm_list_del(&devl->list);
		dm_list_add(devs_out, &devl->list);
		hint->chosen = 1;
	}
}

static void _filter_to_str(struct cmd_context *cmd, int filter_cfg, char **strp)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	char *str;
	int pos = 0;
	int len = 0;
	int ret;

	*strp = NULL;

	if (!(cn = find_config_tree_array(cmd, filter_cfg, NULL))) {
		/* shouldn't happen because default is a|*| */
		return;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING)
			continue;

		len += (strlen(cv->v.str) + 1);
	}
	len++;

	if (len == 1) {
		/* shouldn't happen because default is a|*| */
		return;
	}

	if (!(str = malloc(len)))
		return;
	memset(str, 0, len);

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING)
			continue;

		ret = snprintf(str + pos, len - pos, "%s", cv->v.str);

		if (ret >= len - pos)
			break;
		pos += ret;
	}

	*strp = str;
}

/*
 * Return 1 and needs_refresh 0: the hints can be used
 * Return 1 and needs_refresh 1: the hints can't be used and should be updated
 * Return 0: the hints can't be used
 *
 * recreate is set if hint file should be refreshed/recreated
 */
static int _read_hint_file(struct cmd_context *cmd, struct dm_list *hints, int *needs_refresh)
{
	char devpath[PATH_MAX];
	FILE *fp;
	struct dev_iter *iter;
	struct hint hint;
	struct hint *alloc_hint;
	struct device *dev;
	char *split[HINT_LINE_WORDS];
	char *name, *pvid, *devn, *vgname, *p, *filter_str = NULL;
	uint32_t read_hash = 0;
	uint32_t calc_hash = INITIAL_CRC;
	uint32_t read_count = 0;
	uint32_t calc_count = 0;
	int found = 0;
	int keylen;
	int hv_major, hv_minor;
	int major, minor;
	int ret = 1;
	int i;

	if (!(fp = fopen(_hints_file, "r")))
		return 0;

	log_debug("Reading hint file");

	for (i = 0; i < HINT_LINE_WORDS; i++)
		split[i] = NULL;

	while (fgets(_hint_line, sizeof(_hint_line), fp)) {
		memset(&hint, 0, sizeof(hint));
		if (_hint_line[0] == '#')
			continue;

		if ((p = strchr(_hint_line, '\n')))
			*p = '\0';

		/*
		 * Data in the hint file cannot be used if:
		 * - the hints file major version is larger than used by this cmd
		 * - filters used for hints don't match filters used by this cmd
		 * - scan_lvs setting used when creating hints doesn't match the
		 *   scan_lvs setting used by this cmd
		 * - the list of devs used when creating hints does not match the
		 *   list of devs used by this cmd
		 */

		keylen = strlen("hints_version:");
		if (!strncmp(_hint_line, "hints_version:", keylen)) {
			if (sscanf(_hint_line + keylen, "%d.%d", &hv_major, &hv_minor) != 2) {
				log_debug("ignore hints with unknown version %d.%d", hv_major, hv_minor);
				*needs_refresh = 1;
				break;
			}

			if (hv_major > HINTS_VERSION_MAJOR) {
				log_debug("ignore hints with newer major version %d.%d", hv_major, hv_minor);
				*needs_refresh = 1;
				break;
			}
			continue;
		}

		keylen = strlen("global_filter:");
		if (!strncmp(_hint_line, "global_filter:", keylen)) {
			_filter_to_str(cmd, devices_global_filter_CFG, &filter_str);
			if (!filter_str || strcmp(filter_str, _hint_line + keylen)) {
				log_debug("ignore hints with different global_filter");
				free(filter_str);
				*needs_refresh = 1;
				break;
			}
			free(filter_str);
			continue;
		}

		keylen = strlen("filter:");
		if (!strncmp(_hint_line, "filter:", keylen)) {
			_filter_to_str(cmd, devices_filter_CFG, &filter_str);
			if (!filter_str || strcmp(filter_str, _hint_line + keylen)) {
				log_debug("ignore hints with different filter");
				free(filter_str);
				*needs_refresh = 1;
				break;
			}
			free(filter_str);
			continue;
		}

		keylen = strlen("scan_lvs:");
		if (!strncmp(_hint_line, "scan_lvs:", keylen)) {
			int scan_lvs = 0;
			if ((sscanf(_hint_line + keylen, "%u", &scan_lvs) != 1) ||
			    scan_lvs != cmd->scan_lvs) {
				log_debug("ignore hints with different or unreadable scan_lvs");
				*needs_refresh = 1;
				break;
			}
			continue;
		}

		keylen = strlen("devs_hash:");
		if (!strncmp(_hint_line, "devs_hash:", keylen)) {
			if (sscanf(_hint_line + keylen, "%u %u", &read_hash, &read_count) != 2) {
				log_debug("ignore hints with invalid devs_hash");
				*needs_refresh = 1;
				break;
			}
			continue;
		}

		/*
		 * Ignore any other line prefixes that we don't recognize.
		 */
		keylen = strlen("scan:");
		if (strncmp(_hint_line, "scan:", keylen))
			continue;

		if (dm_split_words(_hint_line, HINT_LINE_WORDS, 0, split) < 1)
			continue;

		name = split[0];
		pvid = split[1];
		devn = split[2];
		vgname = split[3];

		if (name && !strncmp(name, "scan:", 5))
			if (!dm_strncpy(hint.name, name + 5, sizeof(hint.name)))
				continue;

		if (pvid && !strncmp(pvid, "pvid:", 5))
			if (!dm_strncpy(hint.pvid, pvid + 5, sizeof(hint.pvid)))
				continue;

		if (devn && sscanf(devn, "devn:%d:%d", &major, &minor) == 2)
			hint.devt = makedev(major, minor);

		if (vgname && (strlen(vgname) > 3) && (vgname[4] != '-'))
			if (!dm_strncpy(hint.vgname, vgname + 3, sizeof(hint.vgname)))
				continue;

		if (!(alloc_hint = malloc(sizeof(struct hint)))) {
			ret = 0;
			break;
		}
		memcpy(alloc_hint, &hint, sizeof(hint));

		log_debug("add hint %s %s %d:%d %s", hint.name, hint.pvid, major, minor, vgname);
		dm_list_add(hints, &alloc_hint->list);
		found++;
	}

	if (fclose(fp))
		stack;

	if (!ret)
		return 0;

	if (!found)
		return 1;

	if (*needs_refresh)
		return 1;

	/*
	 * Calculate and compare hash of devices that may be scanned.
	 */
	if (!(iter = dev_iter_create(NULL, 0)))
		return 0;
	while ((dev = dev_iter_get(cmd, iter))) {
		if (!_dev_in_hint_hash(cmd, dev))
			continue;
		memset(devpath, 0, sizeof(devpath));
		strncpy(devpath, dev_name(dev), PATH_MAX);
		calc_hash = calc_crc(calc_hash, (const uint8_t *)devpath, strlen(devpath));
		calc_count++;
	}
	dev_iter_destroy(iter);

	if (read_hash && (read_hash != calc_hash)) {
		/* The count is just informational. */
		log_debug("ignore hints with read_hash %u count %u calc_hash %u count %u",
			  read_hash, read_count, calc_hash, calc_count);
		*needs_refresh = 1;
		return 1;
	}

	log_debug("accept hints found %d", dm_list_size(hints));
	return 1;
}

/*
 * Include any device in the hints that label_scan saw which had an lvm label
 * header. label_scan set DEV_SCAN_FOUND_LABEL on the dev if it saw an lvm
 * header.  We only create new hints here after a complete label_scan at the
 * start of the command.  (It makes things far simpler to always just recreate
 * hints from a clean, full scan, than to try to make granular updates to the
 * content of an existing hint file.)
 *
 * Hints are not valid from one command to the next if the commands are using
 * different filters or different scan_lvs settings.  These differences would
 * cause the two commands to consider different devices for scanning.
 *
 * If the set of devices on the system changes from one cmd to the next
 * (excluding those skipped by filters or scan_lvs), the hints are ignored
 * since there may be a new device that is now present that should be scanned
 * that was not present when the hints were created.  The change in the set of
 * devices is detected by creating a hash of all dev names.  When a device is
 * added or removed from this system, this hash changes triggering hints to be
 * recreated.
 *
 * (This hash detection depends on the two commands iterating through dev names
 * in the same order, which happens because the devs are inserted into the
 * btree using devno.  If the btree implementation changes, then we need
 * to sort the dev names here before iterating through them.)
 *
 * N.B. the config setting pv_min_size should technically be included in
 * the hint file like the filter and scan_lvs setting, since increasing
 * pv_min_size can cause new devices to be scanned that were not before.
 * It is left out since it is not often changed, but could be easily added.
 */

int write_hint_file(struct cmd_context *cmd, int newhints)
{
	char devpath[PATH_MAX];
	FILE *fp;
	struct lvmcache_info *info;
	struct dev_iter *iter;
	struct device *dev;
	const char *vgname;
	char *filter_str = NULL;
	uint32_t hash = INITIAL_CRC;
	uint32_t count = 0;
	time_t t;
	int ret = 1;

	/* This function should not be called if !enable_hints or !use_hints. */

	/* No commands are using hints. */
	if (!cmd->enable_hints)
		return 0;

	/* This command does not use hints. */
	if (!cmd->use_hints && !cmd->pvscan_recreate_hints)
		return 0;

	if (lvmcache_has_duplicate_devs() || lvmcache_found_duplicate_vgnames()) {
		/*
		 * When newhints is EMPTY, it means get_hints() found an empty
		 * hint file.  So we scanned all devs and found duplicate pvids
		 * or duplicate vgnames (which is probably why the hints were
		 * empty.)  Since the hint file is already empty, we don't need
		 * to recreate an empty file.
		 */
		if (newhints == NEWHINTS_EMPTY)
			return 1;
	}

	log_debug("Writing hint file %d", newhints);

	if (!(fp = fopen(_hints_file, "w"))) {
		ret = 0;
		goto out_unlock;
	}

	t = time(NULL);

	if (lvmcache_has_duplicate_devs() || lvmcache_found_duplicate_vgnames()) {
		fprintf(fp, "# Created empty by %s pid %d %s", cmd->name, getpid(), ctime(&t));

		/* leave a comment about why it's empty in case someone is curious */
		if (lvmcache_has_duplicate_devs())
			fprintf(fp, "# info: duplicate_pvs\n");
		if (lvmcache_found_duplicate_vgnames())
			fprintf(fp, "# info: duplicate_vgnames\n");
		goto out_flush;
	}

	fprintf(fp, "# Created by %s pid %d %s", cmd->name, getpid(), ctime(&t));
	fprintf(fp, "hints_version: %d.%d\n", HINTS_VERSION_MAJOR, HINTS_VERSION_MINOR);

	_filter_to_str(cmd, devices_global_filter_CFG, &filter_str);
	fprintf(fp, "global_filter:%s\n", filter_str ?: "-");
	free(filter_str);

	_filter_to_str(cmd, devices_filter_CFG, &filter_str);
	fprintf(fp, "filter:%s\n", filter_str ?: "-");
	free(filter_str);

	fprintf(fp, "scan_lvs:%d\n", cmd->scan_lvs);

	/* 
	 * iterate through all devs and write a line for each
	 * dev flagged DEV_SCAN_FOUND_LABEL
	 */

	if (!(iter = dev_iter_create(NULL, 0))) {
		ret = 0;
		goto out_close;
	}

	/*
	 * This loop does two different things (for clarity this should be
	 * two separate dev_iter loops, but one is used for efficiency).
	 * 1. compute the hint hash from all relevant devs
	 * 2. add PVs to the hint file
	 */
	while ((dev = dev_iter_get(cmd, iter))) {
		if (!_dev_in_hint_hash(cmd, dev)) {
			if (dev->flags & DEV_SCAN_FOUND_LABEL) {
				/* should never happen */
				log_error("skip hint hash but found label %s", dev_name(dev));
			}
			continue;
		}

		/*
		 * Create a hash of all device names on the system so we can
		 * detect when the devices on the system change, which
		 * invalidates the existing hints.
		 */
		strncpy(devpath, dev_name(dev), PATH_MAX);
		hash = calc_crc(hash, (const uint8_t *)devpath, strlen(devpath));
		count++;

		if (!(dev->flags & DEV_SCAN_FOUND_LABEL))
			continue;

		if (dev->flags & DEV_IS_MD_COMPONENT) {
			log_debug("exclude md component from hints %s", dev_name(dev));
			continue;
		}

		/*
		 * No vgname will be found here for a PV with no mdas,
		 * in which case the vgname hint will be incomplete.
		 * (The label scan cannot associate nomda-pvs with the
		 * correct vg in lvmcache; that is only done by vg_read.)
		 * When using vgname hint we would always want to also
		 * scan any PVs missing a vgname hint in case they are
		 * part of the vg we are looking for.
		 */
		if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 0)))
			vgname = lvmcache_vgname_from_info(info);
		else
			vgname = NULL;

		if (vgname && is_orphan_vg(vgname))
			vgname = NULL;

		fprintf(fp, "scan:%s pvid:%s devn:%d:%d vg:%s\n",
			dev_name(dev),
			dev->pvid,
			major(dev->dev), minor(dev->dev),
			vgname ?: "-");
	}

	fprintf(fp, "devs_hash: %u %u\n", hash, count);
	dev_iter_destroy(iter);

 out_flush:
	if (fflush(fp))
		stack;

	log_debug("Wrote hint file with devs_hash %u count %u", hash, count);

	/*
	 * We are writing refreshed hints because another command told us to by
	 * touching newhints, so unlink the newhints file.
	 */
	if (newhints == NEWHINTS_FILE)
		_unlink_newhints();

 out_close:
	if (fclose(fp))
		stack;

 out_unlock:
	/* get_hints() took ex lock before returning with newhints set */
	_unlock_hints(cmd);

	return ret;
}

/*
 * Commands that do things that would change existing hints (i.e. create or
 * remove PVs) call this function before they start to get rid of the existing
 * hints.  This function clears the content of the hint file so that subsequent
 * commands will recreate it.  These commands do not try to recreate hints when
 * they are done (this keeps hint creation simple, always done in one way from
 * one place.)  While this command runs, it holds an ex lock on the hint file.
 * This causes any other command that tries to use the hints to ignore the
 * hints by failing in _lock_hints(SH).  We do not want another command to
 * be creating new hints at the same time that this command is changing things
 * that would invalidate them, so we block new hints from being created until
 * we are done with the changes.
 *
 * This is the only place that makes a blocking lock request on the hints file.
 * It does this so that it won't clear the hint file while a previous command
 * is still reading it, and to ensure we are holding the hints lock before we
 * begin changing things.  (In place of a blocking request we could add a retry
 * loop around nonblocking requests, which would allow us to better handle
 * instances where a bad/stuck lock is blocking this for a long time.)
 *
 * To handle cases of indefinite postponement (repeated commands taking sh lock
 * on the hints file, preventing us from ever getting the ex lock), we touch
 * the nohints file first.  The nohints file causes all other commands to
 * ignore hints.  This means we should only have to block waiting for
 * pre-existing commands that have locked the hints file.
 *
 * (If the command were to crash or be SIGKILLed between touch_nohints
 * and unlink_nohints, it could leave the nohints file in place.  This
 * is not a huge deal - it would be cleared by the next command like
 * this that doesn't crash, or by a reboot, or manually.  If it's still
 * an issue we could easily write the pid in the nohints file, and
 * others could check if the pid is still around before obeying it.)
 *
 * The function is meant to be called after the global ex lock has been
 * taken, which is the official lock serializing commands changing which
 * devs are PVs or not.  This means that a command should never block in
 * this function due to another command that has used this function --
 * they would be serialized by the official global lock first.
 * e.g. two pvcreates should never block each other from the hint lock,
 * but rather from the global lock.
 */

void clear_hint_file(struct cmd_context *cmd)
{
	/* No commands are using hints. */
	if (!cmd->enable_hints)
		return;

	log_debug("clear_hint_file");

	/*
	 * This function runs even when cmd->use_hints is 0,
	 * which means this command does not use hints, but
	 * others do, so we are clearing the hints for them.
	 */

	/* limit potential delay blocking on hints lock next */
	if (!_touch_nohints())
		stack;

	if (!_lock_hints(cmd, LOCK_EX, 0))
		stack;

	_unlink_nohints();

	if (!_clear_hints(cmd))
		stack;

	/*
	 * Creating a newhints file here is not necessary, since
	 * get_hints would see an empty hints file, but get_hints
	 * is more efficient if it sees a newhints file first.
	 */
	if (!_touch_newhints())
		stack;
}

/*
 * This is only used at the start of pvscan --cache [-aay] to
 * set up for recreating the hint file.
 */
void pvscan_recreate_hints_begin(struct cmd_context *cmd)
{
	/* No commands are using hints. */
	if (!cmd->enable_hints)
		return;

	log_debug("pvscan_recreate_hints_begin");

	if (!_touch_hints())
		return;

	/* limit potential delay blocking on hints lock next */
	if (!_touch_nohints())
		stack;

	if (!_lock_hints(cmd, LOCK_EX, 0))
		stack;

	_unlink_nohints();

	if (!_clear_hints(cmd))
		stack;
}

/*
 * This is used when pvscan --cache sees a new PV, which
 * means we should refresh hints.  It could catch some case
 * which the other methods of detecting stale hints may miss.
 */
void invalidate_hints(struct cmd_context *cmd)
{
	/* No commands are using hints. */
	if (!cmd->enable_hints)
		return;

	if (!_touch_newhints())
		stack;
}

/*
 * Currently, all the commands using hints (ALLOW_HINTS) take an optional or
 * required first position arg of a VG name or LV name.  If some other command
 * began using hints which took some other kind of position arg, we would
 * probably want to exclude that command from attempting this optimization,
 * because it would be difficult to know what VG that command wanted to use.
 */
static void _get_single_vgname_cmd_arg(struct cmd_context *cmd,
				       struct dm_list *hints, char **vgname)
{
	struct hint *hint;
	char namebuf[NAME_LEN];
	char *name = NULL;
	char *arg, *st, *p;
	int i = 0;
	
	memset(namebuf, 0, sizeof(namebuf));

	if (cmd->position_argc != 1)
		return;

	if (!cmd->position_argv[0])
		return;

	arg = cmd->position_argv[0];

	/* tag */
	if (arg[0] == '@')
		return;

	/* /dev/path - strip chars before vgname */
	if (arg[0] == '/') {
#if 0
		/* skip_dev_dir only available in tools layer */
		const char *strip;
		if (!(strip = skip_dev_dir(cmd, (const char *)arg, NULL)))
			return;
		arg = (char *)strip;
#endif
		return;
	}

	if (!(st = strchr(arg, '/'))) {
		/* simple vgname */
		if (!(name = strdup(arg)))
			return;
		goto check;
	}

	/* take vgname from vgname/lvname */
	for (p = arg; p < st; p++)
		namebuf[i++] = *p;

	if (!(name = strdup(namebuf)))
		return;

check:
	/*
	 * Only use this vgname hint if there are hints that contain this
	 * vgname.  This might happen if we aren't able to properly extract the
	 * vgname from the command args (could happen in some odd cases, e.g.
	 * only LV name is specified without VG name).
	 */
	dm_list_iterate_items(hint, hints) {
		if (!strcmp(hint->vgname, name)) {
			*vgname = name;
			return;
		}
	}

	free(name);
}

/*
 * Returns 0: no hints are used.
 *  . newhints is set if this command should create new hints after scan
 *    for subsequent commands to use.
 *
 * Returns 1: use hints that are returned in hints list.
 */

int get_hints(struct cmd_context *cmd, struct dm_list *hints_out, int *newhints,
	      struct dm_list *devs_in, struct dm_list *devs_out)
{
	struct dm_list hints_list;
	int needs_refresh = 0;
	char *vgname = NULL;

	dm_list_init(&hints_list);

	/* Decide below if the caller should create new hints. */
	*newhints = NEWHINTS_NONE;

	/* No commands are using hints. */
	if (!cmd->enable_hints)
		return 0;

	/*
	 * Special case for 'pvscan --cache' which removes hints,
	 * and then creates new hints.  pvscan does not use hints,
	 * so this has to be checked before the cmd->use_hints check.
	 */
	if (cmd->pvscan_recreate_hints) {
		/* pvscan_recreate_hints_begin already locked hints ex */
		/* create new hints after scan */
		log_debug("get_hints: pvscan recreate");
		*newhints = NEWHINTS_FILE;
		return 0;
	}

	/* This command does not use hints. */
	if (!cmd->use_hints)
		return 0;

	/*
	 * Check if another command created the nohints file to prevent us from
	 * using hints.
	 */
	if (_nohints_exists()) {
		log_debug("get_hints: nohints file");
		return 0;
	}

	/*
	 * Check if another command created the newhints file to cause us to
	 * ignore current hints and recreate new ones.  We'll unlink_newhints
	 * to remove newhints file after writing refreshed hints file.
	 */
	if (_newhints_exists()) {
		log_debug("get_hints: newhints file");
		if (!_hints_exists() && !_touch_hints())
			return 0;

		if (!_lock_hints(cmd, LOCK_EX, NONBLOCK))
			return 0;
		/* create new hints after scan */
		*newhints = NEWHINTS_FILE;
		return 0;
	}

	/*
	 * no hints file exists, a normal case
	 */
	if (!_hints_exists()) {
		log_debug("get_hints: no file");
		if (!_touch_hints())
			return 0;
		if (!_lock_hints(cmd, LOCK_EX, NONBLOCK))
			return 0;
		/* create new hints after scan */
		*newhints = NEWHINTS_INIT;
		return 0;
	}

	/*
	 * hints are locked by a command modifying things, just skip using
	 * hints this time since they aren't accurate while things change.
	 * We hold a sh lock on the hints file while reading it to prevent
	 * another command from clearing it while we're reading
	 */
	if (!_lock_hints(cmd, LOCK_SH, NONBLOCK)) {
		log_debug("get_hints: lock fail");
		return 0;
	}

	/*
	 * couln't read file for some reason, not normal, just skip using hints
	 */
	if (!_read_hint_file(cmd, &hints_list, &needs_refresh)) {
		log_debug("get_hints: read fail");
		_unlock_hints(cmd);
		return 0;
	}

	_unlock_hints(cmd);

	/*
	 * The content of the hint file is invalid and should be refreshed,
	 * so we'll scan everything and then recreate the hints.
	 */
	if (needs_refresh) {
		log_debug("get_hints: needs refresh");

		if (!_lock_hints(cmd, LOCK_EX, NONBLOCK))
			return 0;

		/* create new hints after scan */
		*newhints = NEWHINTS_REFRESH;
		return 0;

	}
	
	/*
	 * A command that changes global state clears the content
	 * of the hints file so it will be recreated, and we must
	 * be following that since we found no hints.
	 */
	if (dm_list_empty(&hints_list)) {
		log_debug("get_hints: no entries");

		if (!_lock_hints(cmd, LOCK_EX, NONBLOCK))
			return 0;

		/* create new hints after scan */
		*newhints = NEWHINTS_EMPTY;
		return 0;
	}

	/*
	 * If the command specifies a single VG (alone or as part of a single
	 * LV), then we can set vgname to further reduce scanning by only
	 * scanning the hints for the given vgname.
	 *
	 * (This is a further optimization beyond the basic hints that tell
	 * us which devs are PVs. We might want to enable this optimization
	 * separately.)
	 */
	_get_single_vgname_cmd_arg(cmd, &hints_list, &vgname);

	_apply_hints(cmd, &hints_list, vgname, devs_in, devs_out);

	log_debug("get_hints: applied using %d other %d",
		  dm_list_size(devs_out), dm_list_size(devs_in));

	dm_list_splice(hints_out, &hints_list);

	free(vgname);

	return 1;
}

