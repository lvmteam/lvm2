/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2017 Red Hat, Inc. All rights reserved.
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
 * This file contains functions now used only by liblvm.
 * Ideally this file should be empty as liblvm and toollib should be doing identical things.
 * FIXME Merge all the code into different parts of the tree.
 */
#include "lib.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "metadata.h"
#include "label.h"
#include "lvm-signal.h"
#include "lvmcache.h"
#include "lvmetad.h"

int vg_reduce(struct volume_group *vg, const char *pv_name)
{
	struct physical_volume *pv;
	struct pv_list *pvl;

	if (!(pvl = find_pv_in_vg(vg, pv_name))) {
		log_error("Physical volume %s not in volume group %s.",
			  pv_name, vg->name);
		return 0;
	}

	pv = pvl->pv;

	if (vgreduce_single(vg->cmd, vg, pv, 0)) {
		dm_list_add(&vg->removed_pvs, &pvl->list);
		return 1;
	}

	log_error("Unable to remove physical volume '%s' from "
		  "volume group '%s'.", pv_name, vg->name);

	return 0;
}

static int _pvcreate_write(struct cmd_context *cmd, struct pv_to_write *pvw)
{
	struct physical_volume *pv = pvw->pv;
	struct device *dev = pv->dev;
	const char *pv_name = dev_name(dev);

	if (pvw->new_pv) {
		/* Wipe existing label first */
		if (!label_remove(pv_dev(pv))) {
			log_error("Failed to wipe existing label on %s", pv_name);
			return 0;
		}

		if (pvw->pp->zero) {
			log_verbose("Zeroing start of device %s", pv_name);

			if (!dev_write_zeros(dev, UINT64_C(0), (size_t) 2048)) {
				log_error("%s not wiped: aborting", pv_name);
				return 0;
			}
		}
	}

	log_verbose("Writing physical volume data to disk \"%s\"",
		    pv_name);

	if (!(pv_write(cmd, pv, 1))) {
		log_error("Failed to write physical volume \"%s\"", pv_name);
		return 0;
	}

	if (pvw->new_pv)
		log_print_unless_silent("Physical volume \"%s\" successfully created", pv_name);
	else
		log_verbose("Physical volume \"%s\" successfully written", pv_name);

	return 1;
}

static int _verify_pv_create_params(struct pvcreate_params *pp)
{
	/*
	 * FIXME: Some of these checks are duplicates in pvcreate_params_validate.
	 */
	if (pp->pva.pvmetadatacopies > 2) {
		log_error("Metadatacopies may only be 0, 1 or 2");
		return 0;
	}

	if (pp->pva.data_alignment > UINT32_MAX) {
		log_error("Physical volume data alignment is too big.");
		return 0;
	}

	if (pp->pva.data_alignment_offset > UINT32_MAX) {
		log_error("Physical volume data alignment offset is too big.");
		return 0;
	}

	return 1;
}

/*
 * See if we may pvcreate on this device.
 * 0 indicates we may not.
 */
static int _pvcreate_check(struct cmd_context *cmd, const char *name,
			   struct pvcreate_params *pp, int *wiped)
{
	static const char really_init_msg[] = "Really INITIALIZE physical volume";
	static const char not_init_msg[] = "physical volume not initialized";
	struct physical_volume *pv;
	struct device *dev;
	int r = 0;
	int scan_needed = 0;
	int filter_refresh_needed = 0;
	int used;

	/* FIXME Check partition type is LVM unless --force is given */

	*wiped = 0;

	/* Is there a pv here already? */
	pv = find_pv_by_name(cmd, name, 1, 1);

	/* Allow partial & exported VGs to be destroyed. */
	/* We must have -ff to overwrite a non orphan */
	if (pv) {
		if (!is_orphan(pv) && pp->force != DONT_PROMPT_OVERRIDE) {
			log_error("Can't initialize physical volume \"%s\" of "
				  "volume group \"%s\" without -ff.", name, pv_vg_name(pv));
			goto out;
		}

		if ((used = is_used_pv(pv)) < 0)
			goto_out;

		if (used && pp->force != DONT_PROMPT_OVERRIDE) {
			log_error("PV %s is used by a VG but its metadata is missing.", name);
			log_error("Can't initialize PV '%s' without -ff.", name);
			goto out;
		}
	}

	/* prompt */
	if (pv && !pp->yes) {
		if (is_orphan(pv)) {
			if (used) {
				if (yes_no_prompt("%s \"%s\" that is marked as belonging to a VG [y/n]? ",
						  really_init_msg, name) == 'n') {
					log_error("%s: %s", name, not_init_msg);
					goto out;
				}
			}
		} else {
			if (yes_no_prompt("%s \"%s\" of volume group \"%s\" [y/n]? ",
					  really_init_msg, name, pv_vg_name(pv)) == 'n') {
				log_error("%s: %s", name, not_init_msg);
				goto out;
			}
		}
	}

	if (sigint_caught())
		goto_out;

	dev = dev_cache_get(name, cmd->full_filter);

	/*
	 * Refresh+rescan at the end is needed if:
	 *   - we don't obtain device list from udev,
	 *     hence persistent cache file is used
	 *     and we need to trash it and reevaluate
	 *     for any changes done outside - adding
	 *     any new foreign signature which may affect
	 *     filtering - before we do pvcreate, we
	 *     need to be sure that we have up-to-date
	 *     view for filters
	 *
	 *   - we have wiped existing foreign signatures
	 *     from dev as this may affect what's filtered
	 *     as well
	 *
	 *
	 * Only rescan at the end is needed if:
	 *   - we've just checked whether dev is fileterd
	 *     by MD filter. We do the refresh in-situ,
	 *     so no need to require the refresh at the
	 *     end of this fn. This is to allow for
	 *     wiping MD signature during pvcreate for
	 *     the dev - the dev would normally be
	 *     filtered because of MD filter.
	 *     This is an exception.
	 */

	/* Is there an md superblock here? */
	if (!dev && md_filtering()) {
		if (!refresh_filters(cmd))
			goto_out;

		init_md_filtering(0);
		dev = dev_cache_get(name, cmd->full_filter);
		init_md_filtering(1);

		scan_needed = 1;
	} else if (!obtain_device_list_from_udev())
		filter_refresh_needed = scan_needed = 1;

	if (!dev) {
		log_error("Device %s not found (or ignored by filtering).", name);
		goto out;
	}

	/*
	 * This test will fail if the device belongs to an MD array.
	 */
	if (!label_scan_open_excl(dev)) {
		/* FIXME Detect whether device-mapper itself is still using it */
		log_error("Can't open %s exclusively.  Mounted filesystem?",
			  name);
		goto out;
	}

	if (!wipe_known_signatures(cmd, dev, name,
				   TYPE_LVM1_MEMBER | TYPE_LVM2_MEMBER,
				   0, pp->yes, pp->force, wiped)) {
		log_error("Aborting pvcreate on %s.", name);
		goto out;
	}

	if (*wiped)
		filter_refresh_needed = scan_needed = 1;

	if (sigint_caught())
		goto_out;

	if (pv && !is_orphan(pv) && pp->force)
		log_warn("WARNING: Forcing physical volume creation on "
			  "%s%s%s%s", name,
			  !is_orphan(pv) ? " of volume group \"" : "",
			  pv_vg_name(pv),
			  !is_orphan(pv) ? "\"" : "");

	r = 1;

out:
	if (filter_refresh_needed)
		if (!refresh_filters(cmd)) {
			stack;
			r = 0;
		}

	if (scan_needed) {
		if (!lvmcache_label_scan(cmd)) {
			stack;
			r = 0;
		}
	}

	free_pv_fid(pv);
	return r;
}

/*
 * pvcreate_vol() - initialize a device with PV label and metadata area
 *
 * Parameters:
 * - pv_name: device path to initialize
 * - pp: parameters to pass to pv_create; if NULL, use default values
 *
 * Returns:
 * NULL: error
 * struct physical_volume * (non-NULL): handle to physical volume created
 */
struct physical_volume *pvcreate_vol(struct cmd_context *cmd, const char *pv_name,
				     struct pvcreate_params *pp, int write_now)
{
	struct physical_volume *pv = NULL;
	struct device *dev;
	int wiped = 0;
	struct dm_list mdas;
	struct pvcreate_params default_pp;
	char buffer[64] __attribute__((aligned(8)));
	dev_ext_t dev_ext_src;

	pvcreate_params_set_defaults(&default_pp);
	if (!pp)
		pp = &default_pp;

	if (!_verify_pv_create_params(pp)) {
		goto bad;
	}

	if (pp->pva.idp) {
		if ((dev = lvmcache_device_from_pvid(cmd, pp->pva.idp, NULL)) &&
		    (dev != dev_cache_get(pv_name, cmd->full_filter))) {
			if (!id_write_format((const struct id*)&pp->pva.idp->uuid,
			    buffer, sizeof(buffer)))
				goto_bad;
			log_error("uuid %s already in use on \"%s\"", buffer,
				  dev_name(dev));
			goto bad;
		}
	}

	if (!_pvcreate_check(cmd, pv_name, pp, &wiped))
		goto_bad;

	if (sigint_caught())
		goto_bad;

	/*
	 * wipe_known_signatures called in _pvcreate_check fires
	 * WATCH event to update udev database. But at the moment,
	 * we have no way to synchronize with such event - we may
	 * end up still seeing the old info in udev db and pvcreate
	 * can fail to proceed because of the device still being
	 * filtered (because of the stale info in udev db).
	 * Disable udev dev-ext source temporarily here for
	 * this reason and rescan with DEV_EXT_NONE dev-ext
	 * source (so filters use DEV_EXT_NONE source).
	 */
	dev_ext_src = external_device_info_source();
	if (wiped && (dev_ext_src == DEV_EXT_UDEV))
		init_external_device_info_source(DEV_EXT_NONE);

	dev = dev_cache_get(pv_name, cmd->full_filter);

	init_external_device_info_source(dev_ext_src);

	if (!dev) {
		log_error("%s: Couldn't find device.  Check your filters?",
			  pv_name);
		goto bad;
	}

	dm_list_init(&mdas);

	if (!(pv = pv_create(cmd, dev, &pp->pva))) {
		log_error("Failed to setup physical volume \"%s\"", pv_name);
		goto bad;
	}

	log_verbose("Set up physical volume for \"%s\" with %" PRIu64
		    " available sectors", pv_name, pv_size(pv));

	pv->status |= UNLABELLED_PV;
	if (write_now) {
		struct pv_to_write pvw;
		pvw.pp = pp;
		pvw.pv = pv;
		pvw.new_pv = 1;
		if (!_pvcreate_write(cmd, &pvw))
			goto bad;
	}

	return pv;

bad:
	return NULL;
}

/*
 * Extend a VG by a single PV / device path
 *
 * Parameters:
 * - vg: handle of volume group to extend by 'pv_name'
 * - pv_name: device path of PV to add to VG
 * - pp: parameters to pass to implicit pvcreate; if NULL, do not pvcreate
 * - max_phys_block_size: largest physical block size found amongst PVs in a VG
 *
 */
static int _vg_extend_single_pv(struct volume_group *vg, char *pv_name,
				struct pvcreate_params *pp,
				unsigned int *max_phys_block_size)
{
	struct physical_volume *pv;
	struct pv_to_write *pvw;
	int new_pv = 0;

	pv = find_pv_by_name(vg->cmd, pv_name, 1, 1);

	if (!pv && !pp) {
		log_error("%s not identified as an existing "
			  "physical volume", pv_name);
		return 0;
	}

	if (!pv && pp) {
		if (!(pv = pvcreate_vol(vg->cmd, pv_name, pp, 0)))
			return_0;
		new_pv = 1;
	}

	if (!(check_dev_block_size_for_vg(pv->dev, (const struct volume_group *) vg,
					  max_phys_block_size)))
		goto_bad;

	if (!add_pv_to_vg(vg, pv_name, pv, new_pv))
		goto_bad;

	if ((pv->fmt->features & FMT_PV_FLAGS) ||
	    (pv->status & UNLABELLED_PV)) {
		if (!(pvw = dm_pool_zalloc(vg->vgmem, sizeof(*pvw)))) {
			log_error("pv_to_write allocation for '%s' failed", pv_name);
			return 0;
		}
		pvw->pv = pv;
		pvw->pp = new_pv ? pp : NULL;
		pvw->new_pv = new_pv;
		dm_list_add(&vg->pvs_to_write, &pvw->list);
	}

	return 1;
bad:
	free_pv_fid(pv);
	return 0;
}

/*
 * Extend a VG by a single PV / device path
 *
 * Parameters:
 * - vg: handle of volume group to extend by 'pv_name'
 * - pv_count: count of device paths of PVs
 * - pv_names: device paths of PVs to add to VG
 * - pp: parameters to pass to implicit pvcreate; if NULL, do not pvcreate
 *
 */
int vg_extend(struct volume_group *vg, int pv_count, const char *const *pv_names,
	      struct pvcreate_params *pp)
{
	int i;
	char *pv_name;
	unsigned int max_phys_block_size = 0;

	if (vg_bad_status_bits(vg, RESIZEABLE_VG))
		return_0;

	/* attach each pv */
	for (i = 0; i < pv_count; i++) {
		if (!(pv_name = dm_strdup(pv_names[i]))) {
			log_error("Failed to duplicate pv name %s.", pv_names[i]);
			return 0;
		}
		dm_unescape_colons_and_at_signs(pv_name, NULL, NULL);
		if (!_vg_extend_single_pv(vg, pv_name, pp, &max_phys_block_size)) {
			log_error("Unable to add physical volume '%s' to "
				  "volume group '%s'.", pv_name, vg->name);
			dm_free(pv_name);
			return 0;
		}
		dm_free(pv_name);
	}

	(void) check_pv_dev_sizes(vg);

/* FIXME Decide whether to initialise and add new mdahs to format instance */

	return 1;
}

/*
 * Decide whether it is "safe" to wipe the labels on this device.
 * 0 indicates we may not.
 */
static int _pvremove_check(struct cmd_context *cmd, const char *name,
			   unsigned force_count, unsigned prompt, struct dm_list *pvslist)
{
	static const char really_wipe_msg[] = "Really WIPE LABELS from physical volume";
	struct device *dev;
	struct pv_list *pvl;
	struct physical_volume *pv = NULL;
	int used;
	int r = 0;

	/* FIXME Check partition type is LVM unless --force is given */

	if (!(dev = dev_cache_get(name, cmd->filter))) {
		log_error("Device %s not found.", name);
		return 0;
	}

	/* Is there a pv here already? */
	/* If not, this is an error unless you used -f. */
	if (!label_read(dev)) {
		if (force_count)
			return 1;
		log_error("No PV label found on %s.", name);
		return 0;
	}

	dm_list_iterate_items(pvl, pvslist)
		if (pvl->pv->dev == dev)
			pv = pvl->pv;

	if (!pv) {
		log_error(INTERNAL_ERROR "Physical Volume %s has a label, "
			  "but is neither in a VG nor orphan.", name);
		goto out; /* better safe than sorry */
	}

	if (is_orphan(pv)) {
		if ((used = is_used_pv(pv)) < 0)
			goto_out;

		if (used) {
			log_warn("WARNING: PV %s is used by a VG but its metadata is missing.", name);

			if (force_count < 2)
				goto_bad;

			if (!prompt &&
			    yes_no_prompt("%s \"%s\" that is marked as belonging to a VG [y/n]? ",
					  really_wipe_msg, name) == 'n')
				goto_bad;
		}
	} else {
		log_warn("WARNING: PV %s is used by VG %s (consider using vgreduce).", name, pv_vg_name(pv));

		if (force_count < 2)
			goto_bad;

		if (!prompt &&
		    yes_no_prompt("%s \"%s\" of volume group \"%s\" [y/n]? ",
				  really_wipe_msg, name, pv_vg_name(pv)) == 'n')
			goto_bad;
	}

	if (force_count)
		log_warn("WARNING: Wiping physical volume label from "
			 "%s%s%s%s", name,
			 !is_orphan(pv) ? " of volume group \"" : "",
			 pv_vg_name(pv),
			 !is_orphan(pv) ? "\"" : "");

	r = 1;
bad:
	if (!r) {
		log_error("%s: physical volume label not removed.", name);

		if (force_count < 2) /* Show hint as log_error() */
			log_error("(If you are certain you need pvremove, "
				  "then confirm by using --force twice.)");
	}
out:
	return r;
}

static int _pvremove_single(struct cmd_context *cmd, const char *pv_name,
			    void *handle __attribute__((unused)), unsigned force_count,
			    unsigned prompt, struct dm_list *pvslist)
{
	struct device *dev;
	//struct lvmcache_info *info;
	int r = 0;

	if (!_pvremove_check(cmd, pv_name, force_count, prompt, pvslist))
		goto out;

	if (!(dev = dev_cache_get(pv_name, cmd->filter))) {
		log_error("%s: Couldn't find device.  Check your filters?",
			  pv_name);
		goto out;
	}

	/* Wipe existing label(s) */
	if (!label_remove(dev)) {
		log_error("Failed to wipe existing label(s) on %s", pv_name);
		goto out;
	}

	if (!lvmetad_pv_gone_by_dev(dev))
		goto_out;

	log_print_unless_silent("Labels on physical volume \"%s\" successfully wiped",
				pv_name);

	r = 1;

out:
	return r;
}

int pvremove_many(struct cmd_context *cmd, struct dm_list *pv_names,
		  unsigned force_count, unsigned prompt)
{
	int ret = 1;
	struct dm_list *pvslist = NULL;
	struct pv_list *pvl;
	const struct dm_str_list *pv_name;

	if (!lock_vol(cmd, VG_ORPHANS, LCK_VG_WRITE, NULL)) {
		log_error("Can't get lock for orphan PVs");
		return 0;
	}

	lvmcache_seed_infos_from_lvmetad(cmd);

	if (!(pvslist = get_pvs(cmd))) {
		ret = 0;
		goto_out;
	}

	dm_list_iterate_items(pv_name, pv_names) {
		if (!_pvremove_single(cmd, pv_name->str, NULL, force_count, prompt, pvslist)) {
			stack;
			ret = 0;
		}
		if (sigint_caught()) {
			ret = 0;
			goto_out;
		}
	}

out:
	unlock_vg(cmd, NULL, VG_ORPHANS);

	if (pvslist)
		dm_list_iterate_items(pvl, pvslist)
			free_pv_fid(pvl->pv);

	return ret;
}

/* FIXME: liblvm todo - make into function that returns handle */
struct physical_volume *find_pv_by_name(struct cmd_context *cmd,
					const char *pv_name,
					int allow_orphan, int allow_unformatted)
{
	struct device *dev;
	struct pv_list *pvl;
	struct dm_list *pvslist;
	struct physical_volume *pv = NULL;

	lvmcache_seed_infos_from_lvmetad(cmd);

	if (!(dev = dev_cache_get(pv_name, cmd->filter))) {
		if (!allow_unformatted)
			log_error("Physical volume %s not found", pv_name);
		return_NULL;
	}

	if (!(pvslist = get_pvs(cmd)))
		return_NULL;

	dm_list_iterate_items(pvl, pvslist)
		if (pvl->pv->dev == dev)
			pv = pvl->pv;
		else
			free_pv_fid(pvl->pv);

	if (!pv && !allow_unformatted)
		log_error("Physical volume %s not found", pv_name);

	if (pv && !allow_orphan && is_orphan_vg(pv->vg_name)) {
		log_error("Physical volume %s not in a volume group", pv_name);
		goto bad;
	}

	return pv;

bad:
	free_pv_fid(pv);
	return NULL;
}
