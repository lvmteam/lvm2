/*
 * Copyright (C) 2001-2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "format-text.h"
#include "import-export.h"
#include "device.h"
#include "lvm-file.h"
#include "pool.h"
#include "config.h"
#include "hash.h"
#include "display.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "uuid.h"
#include "layout.h"
#include "crc.h"
#include "xlate.h"
#include "label.h"
#include "memlock.h"

#include <unistd.h>
#include <sys/file.h>
#include <limits.h>
#include <dirent.h>
#include <ctype.h>

#define FMT_TEXT_NAME "lvm2"
#define FMT_TEXT_ALIAS "text"

static struct format_instance *_create_text_instance(const struct format_type
						     *fmt, const char *vgname,
						     void *context);

struct dir_list {
	struct list list;
	char dir[0];
};

struct raw_list {
	struct list list;
	struct device_area dev_area;
};

struct text_context {
	char *path_live;	/* Path to file holding live metadata */
	char *path_edit;	/* Path to file holding edited metadata */
	char *desc;		/* Description placed inside file */
};

/*
 * NOTE: Currently there can be only one vg per text file.
 */

static int _vg_setup(struct format_instance *fid, struct volume_group *vg)
{
	if (vg->extent_size & (vg->extent_size - 1)) {
		log_error("Extent size must be power of 2");
		return 0;
	}

	return 1;
}

static int _lv_setup(struct format_instance *fid, struct logical_volume *lv)
{
/******** FIXME Any LV size restriction? 
	uint64_t max_size = UINT_MAX;

	if (lv->size > max_size) {
		char *dummy = display_size(max_size / 2, SIZE_SHORT);
		log_error("logical volumes cannot be larger than %s", dummy);
		dbg_free(dummy);
		return 0;
	}
*/

	if (!*lv->lvid.s)
		lvid_create(&lv->lvid, &lv->vg->id);

	return 1;
}

static void _xlate_mdah(struct mda_header *mdah)
{
	struct raw_locn *rl;

	mdah->version = xlate32(mdah->version);
	mdah->start = xlate64(mdah->start);
	mdah->size = xlate64(mdah->size);

	rl = &mdah->raw_locns[0];
	while (rl->offset) {
		rl->checksum = xlate32(rl->checksum);
		rl->offset = xlate64(rl->offset);
		rl->size = xlate64(rl->size);
		rl++;
	}
}

static struct mda_header *_raw_read_mda_header(const struct format_type *fmt,
					       struct device_area *dev_area)
{
	struct mda_header *mdah;

	if (!(mdah = pool_alloc(fmt->cmd->mem, MDA_HEADER_SIZE))) {
		log_error("struct mda_header allocation failed");
		return NULL;
	}

	if (!dev_read(dev_area->dev, dev_area->start, MDA_HEADER_SIZE, mdah)) {
		stack;
		pool_free(fmt->cmd->mem, mdah);
		return NULL;
	}

	if (mdah->checksum_xl != xlate32(calc_crc(INITIAL_CRC, mdah->magic,
						  MDA_HEADER_SIZE -
						  sizeof(mdah->checksum_xl)))) {
		log_error("Incorrect metadata area header checksum");
		return NULL;
	}

	_xlate_mdah(mdah);

	if (strncmp(mdah->magic, FMTT_MAGIC, sizeof(mdah->magic))) {
		log_error("Wrong magic number in metadata area header");
		return NULL;
	}

	if (mdah->version != FMTT_VERSION) {
		log_error("Incompatible metadata area header version: %d",
			  mdah->version);
		return NULL;
	}

	if (mdah->start != dev_area->start) {
		log_error("Incorrect start sector in metadata area header: %"
			  PRIu64, mdah->start);
		return NULL;
	}

	return mdah;
}

static int _raw_write_mda_header(const struct format_type *fmt,
				 struct device *dev,
				 uint64_t start_byte, struct mda_header *mdah)
{
	strncpy(mdah->magic, FMTT_MAGIC, sizeof(mdah->magic));
	mdah->version = FMTT_VERSION;
	mdah->start = start_byte;

	_xlate_mdah(mdah);
	mdah->checksum_xl = xlate32(calc_crc(INITIAL_CRC, mdah->magic,
					     MDA_HEADER_SIZE -
					     sizeof(mdah->checksum_xl)));

	if (!dev_write(dev, start_byte, MDA_HEADER_SIZE, mdah)) {
		stack;
		pool_free(fmt->cmd->mem, mdah);
		return 0;
	}

	return 1;
}

static struct raw_locn *_find_vg_rlocn(struct device_area *dev_area,
				       struct mda_header *mdah,
				       const char *vgname)
{
	size_t len;
	char vgnamebuf[NAME_LEN + 2];
	struct raw_locn *rlocn;

	rlocn = mdah->raw_locns;

	/* FIXME Ignore if checksum incorrect!!! */
	while (rlocn->offset) {
		if (!dev_read(dev_area->dev, dev_area->start + rlocn->offset,
			      sizeof(vgnamebuf), vgnamebuf)) {
			stack;
			return NULL;
		}
		if (!strncmp(vgnamebuf, vgname, len = strlen(vgname)) &&
		    (isspace(vgnamebuf[len]) || vgnamebuf[len] == '{')) {
			return rlocn;
		}
		rlocn++;
	}

	return NULL;
}

static struct raw_locn *_vg_posn(struct format_instance *fid,
				 struct device_area *dev_area,
				 const char *vgname)
{

	struct mda_header *mdah;

	if (!(mdah = _raw_read_mda_header(fid->fmt, dev_area))) {
		stack;
		return NULL;
	}

	return _find_vg_rlocn(dev_area, mdah, vgname);
}

static int _raw_holds_vgname(struct format_instance *fid,
			     struct device_area *dev_area, const char *vgname)
{
	int r = 0;

	if (!dev_open(dev_area->dev)) {
		stack;
		return 0;
	}

	if (_vg_posn(fid, dev_area, vgname))
		r = 1;

	if (!dev_close(dev_area->dev))
		stack;

	return r;
}

static struct volume_group *_vg_read_raw_area(struct format_instance *fid,
					      const char *vgname,
					      struct device_area *area)
{
	struct volume_group *vg = NULL;
	struct raw_locn *rlocn;
	struct mda_header *mdah;
	time_t when;
	char *desc;
	uint32_t wrap = 0;

	if (!dev_open(area->dev)) {
		stack;
		return NULL;
	}

	if (!(mdah = _raw_read_mda_header(fid->fmt, area))) {
		stack;
		goto out;
	}

	if (!(rlocn = _vg_posn(fid, area, vgname))) {
		stack;
		goto out;
	}

	if (rlocn->offset + rlocn->size > mdah->size)
		wrap = (uint32_t) ((rlocn->offset + rlocn->size) - mdah->size);

	if (wrap > rlocn->offset) {
		log_error("VG %s metadata too large for circular buffer",
			  vg->name);
		goto out;
	}

	/* FIXME 64-bit */
	if (!(vg = text_vg_import_fd(fid, NULL, area->dev,
				     (off_t) (area->start + rlocn->offset),
				     (uint32_t) (rlocn->size - wrap),
				     (off_t) (area->start + MDA_HEADER_SIZE),
				     wrap, calc_crc, rlocn->checksum, &when,
				     &desc))) {
		stack;
		goto out;
	}
	log_debug("Read %s metadata (%u) from %s at %" PRIu64 " size %" PRIu64,
		  vg->name, vg->seqno, dev_name(area->dev),
		  area->start + rlocn->offset, rlocn->size);

      out:
	if (!dev_close(area->dev))
		stack;

	return vg;
}

static struct volume_group *_vg_read_raw(struct format_instance *fid,
					 const char *vgname,
					 struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;

	return _vg_read_raw_area(fid, vgname, &mdac->area);
}

static int _vg_write_raw(struct format_instance *fid, struct volume_group *vg,
			 struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct raw_locn *rlocn;
	struct mda_header *mdah;
	struct physical_volume *pv;
	struct list *pvh;
	int r = 0;
	uint32_t new_wrap = 0, old_wrap = 0;

	/* FIXME Essential fix! Make dynamic (realloc? pool?) */
	char buf[65536];
	int found = 0;

	/* Ignore any mda on a PV outside the VG. vgsplit relies on this */
	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;
		if (pv->dev == mdac->area.dev) {
			found = 1;
			break;
		}
	}

	if (!found)
		return 1;

	if (!dev_open(mdac->area.dev)) {
		stack;
		return 0;
	}

	if (!(mdah = _raw_read_mda_header(fid->fmt, &mdac->area))) {
		stack;
		goto out;
	}

	if ((rlocn = _find_vg_rlocn(&mdac->area, mdah, vg->name))) {
		/* Start of free space - round up to next sector; circular */
		mdac->rlocn.offset =
		    ((rlocn->offset + rlocn->size +
		      (SECTOR_SIZE - rlocn->size % SECTOR_SIZE) -
		      MDA_HEADER_SIZE) % (mdah->size - MDA_HEADER_SIZE))
		    + MDA_HEADER_SIZE;
	} else {
		/* Find an empty slot */
		/* FIXME Assume only one VG per mdah for now */
		mdac->rlocn.offset = MDA_HEADER_SIZE;
	}

	if (!(mdac->rlocn.size = text_vg_export_raw(vg, "", buf, sizeof(buf)))) {
		log_error("VG %s metadata writing failed", vg->name);
		goto out;
	}

	if (mdac->rlocn.offset + mdac->rlocn.size > mdah->size)
		new_wrap = (mdac->rlocn.offset + mdac->rlocn.size) - mdah->size;

	if (rlocn && (rlocn->offset + rlocn->size > mdah->size))
		old_wrap = (rlocn->offset + rlocn->size) - mdah->size;

	if ((new_wrap && old_wrap) ||
	    (rlocn && ((new_wrap > rlocn->offset) ||
		       (old_wrap && (mdac->rlocn.offset + mdac->rlocn.size >
				     rlocn->offset)))) ||
	    (mdac->rlocn.size >= mdah->size)) {
		log_error("VG %s metadata too large for circular buffer",
			  vg->name);
		goto out;
	}

	log_debug("Writing %s metadata to %s at %" PRIu64 " len %" PRIu64,
		  vg->name, dev_name(mdac->area.dev), mdac->area.start +
		  mdac->rlocn.offset, mdac->rlocn.size - new_wrap);

	/* Write text out, circularly */
	if (!dev_write(mdac->area.dev, mdac->area.start + mdac->rlocn.offset,
		       (size_t) (mdac->rlocn.size - new_wrap), buf)) {
		stack;
		goto out;
	}

	if (new_wrap) {
		log_debug("Writing metadata to %s at %" PRIu64 " len %" PRIu32,
			  dev_name(mdac->area.dev), mdac->area.start +
			  MDA_HEADER_SIZE, new_wrap);

		if (!dev_write(mdac->area.dev,
			       mdac->area.start + MDA_HEADER_SIZE,
			       (size_t) new_wrap,
			       buf + mdac->rlocn.size - new_wrap)) {
			stack;
			goto out;
		}
	}

	mdac->rlocn.checksum = calc_crc(INITIAL_CRC, buf,
					(uint32_t) (mdac->rlocn.size -
						    new_wrap));
	if (new_wrap)
		mdac->rlocn.checksum = calc_crc(mdac->rlocn.checksum,
						buf + mdac->rlocn.size -
						new_wrap, new_wrap);

	r = 1;

      out:
	if (!r && !dev_close(mdac->area.dev))
		stack;

	return r;
}

static int _vg_commit_raw(struct format_instance *fid, struct volume_group *vg,
			  struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct mda_header *mdah;
	struct raw_locn *rlocn;
	struct physical_volume *pv;
	struct list *pvh;
	int r = 0;
	int found = 0;

	/* Ignore any mda on a PV outside the VG. vgsplit relies on this */
	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;
		if (pv->dev == mdac->area.dev) {
			found = 1;
			break;
		}
	}

	if (!found)
		return 1;

	if (!(mdah = _raw_read_mda_header(fid->fmt, &mdac->area))) {
		stack;
		goto out;
	}

	if (!(rlocn = _find_vg_rlocn(&mdac->area, mdah, vg->name))) {
		rlocn = &mdah->raw_locns[0];
		mdah->raw_locns[1].offset = 0;
	}

	rlocn->offset = mdac->rlocn.offset;
	rlocn->size = mdac->rlocn.size;
	rlocn->checksum = mdac->rlocn.checksum;

	log_debug("Committing %s metadata (%u) to %s header at %" PRIu64,
		  vg->name, vg->seqno, dev_name(mdac->area.dev),
		  mdac->area.start);
	if (!_raw_write_mda_header(fid->fmt, mdac->area.dev, mdac->area.start,
				   mdah)) {
		log_error("Failed to write metadata area header");
		goto out;
	}

	r = 1;

      out:
	if (!dev_close(mdac->area.dev))
		stack;

	return r;
}

/* Close metadata area devices */
static int _vg_revert_raw(struct format_instance *fid, struct volume_group *vg,
			  struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct physical_volume *pv;
	struct list *pvh;
	int found = 0;

	/* Ignore any mda on a PV outside the VG. vgsplit relies on this */
	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;
		if (pv->dev == mdac->area.dev) {
			found = 1;
			break;
		}
	}

	if (!found)
		return 1;

	if (!dev_close(mdac->area.dev))
		stack;

	return 1;
}

static int _vg_remove_raw(struct format_instance *fid, struct volume_group *vg,
			  struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct mda_header *mdah;
	struct raw_locn *rlocn;
	int r = 0;

	if (!dev_open(mdac->area.dev)) {
		stack;
		return 0;
	}

	if (!(mdah = _raw_read_mda_header(fid->fmt, &mdac->area))) {
		stack;
		goto out;
	}

	if (!(rlocn = _find_vg_rlocn(&mdac->area, mdah, vg->name))) {
		rlocn = &mdah->raw_locns[0];
		mdah->raw_locns[1].offset = 0;
	}

	rlocn->offset = 0;
	rlocn->size = 0;
	rlocn->checksum = 0;

	if (!_raw_write_mda_header(fid->fmt, mdac->area.dev, mdac->area.start,
				   mdah)) {
		log_error("Failed to write metadata area header");
		goto out;
	}

	r = 1;

      out:
	if (!dev_close(mdac->area.dev))
		stack;

	return r;
}

static struct volume_group *_vg_read_file_name(struct format_instance *fid,
					       const char *vgname,
					       const char *path_live)
{
	struct volume_group *vg;
	time_t when;
	char *desc;

	if (!(vg = text_vg_import_file(fid, path_live, &when, &desc))) {
		stack;
		return NULL;
	}

	/*
	 * Currently you can only have a single volume group per
	 * text file (this restriction may remain).  We need to
	 * check that it contains the correct volume group.
	 */
	if (vgname && strcmp(vgname, vg->name)) {
		pool_free(fid->fmt->cmd->mem, vg);
		log_err("'%s' does not contain volume group '%s'.",
			path_live, vgname);
		return NULL;
	} else
		log_debug("Read volume group %s from %s", vg->name, path_live);

	return vg;
}

static struct volume_group *_vg_read_file(struct format_instance *fid,
					  const char *vgname,
					  struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;

	return _vg_read_file_name(fid, vgname, tc->path_live);
}

static int _vg_write_file(struct format_instance *fid, struct volume_group *vg,
			  struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;

	FILE *fp;
	int fd;
	char *slash;
	char temp_file[PATH_MAX], temp_dir[PATH_MAX];

	slash = rindex(tc->path_edit, '/');

	if (slash == 0)
		strcpy(temp_dir, ".");
	else if (slash - tc->path_edit < PATH_MAX) {
		strncpy(temp_dir, tc->path_edit,
			(size_t) (slash - tc->path_edit));
		temp_dir[slash - tc->path_edit] = '\0';

	} else {
		log_error("Text format failed to determine directory.");
		return 0;
	}

	if (!create_temp_name(temp_dir, temp_file, sizeof(temp_file), &fd)) {
		log_err("Couldn't create temporary text file name.");
		return 0;
	}

	if (!(fp = fdopen(fd, "w"))) {
		log_sys_error("fdopen", temp_file);
		close(fd);
		return 0;
	}

	log_debug("Writing %s metadata to %s", vg->name, temp_file);

	if (!text_vg_export_file(vg, tc->desc, fp)) {
		log_error("Failed to write metadata to %s.", temp_file);
		fclose(fp);
		return 0;
	}

	if (fsync(fd)) {
		log_sys_error("fsync", tc->path_edit);
		fclose(fp);
		return 0;
	}

	if (fclose(fp)) {
		log_sys_error("fclose", tc->path_edit);
		return 0;
	}

	if (rename(temp_file, tc->path_edit)) {
		log_debug("Renaming %s to %s", temp_file, tc->path_edit);
		log_error("%s: rename to %s failed: %s", temp_file,
			  tc->path_edit, strerror(errno));
		return 0;
	}

	return 1;
}

static int _vg_commit_file_backup(struct format_instance *fid,
				  struct volume_group *vg,
				  struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;

	if (test_mode()) {
		log_verbose("Test mode: Skipping committing %s metadata (%u)",
			    vg->name, vg->seqno);
		if (unlink(tc->path_edit)) {
			log_debug("Unlinking %s", tc->path_edit);
			log_sys_error("unlink", tc->path_edit);
			return 0;
		}
	} else {
		log_debug("Committing %s metadata (%u)", vg->name, vg->seqno);
		log_debug("Renaming %s to %s", tc->path_edit, tc->path_live);
		if (rename(tc->path_edit, tc->path_live)) {
			log_error("%s: rename to %s failed: %s", tc->path_edit,
				  tc->path_live, strerror(errno));
			return 0;
		}
	}

	sync_dir(tc->path_edit);

	return 1;
}

static int _vg_commit_file(struct format_instance *fid, struct volume_group *vg,
			   struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;
	char *slash;
	char newname[PATH_MAX];
	size_t len;

	if (!_vg_commit_file_backup(fid, vg, mda))
		return 0;

	/* vgrename? */
	if ((slash = rindex(tc->path_live, '/')))
		slash = slash + 1;
	else
		slash = tc->path_live;

	if (strcmp(slash, vg->name)) {
		len = slash - tc->path_live;
		strncpy(newname, tc->path_live, len);
		strcpy(newname + len, vg->name);
		log_debug("Renaming %s to %s", tc->path_live, newname);
		if (test_mode())
			log_verbose("Test mode: Skipping rename");
		else {
			if (rename(tc->path_live, newname)) {
				log_error("%s: rename to %s failed: %s",
					  tc->path_live, newname,
					  strerror(errno));
				sync_dir(newname);
				return 0;
			}
		}
	}

	return 1;
}

static int _vg_remove_file(struct format_instance *fid, struct volume_group *vg,
			   struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;

	if (path_exists(tc->path_edit) && unlink(tc->path_edit)) {
		log_sys_error("unlink", tc->path_edit);
		return 0;
	}

	if (path_exists(tc->path_live) && unlink(tc->path_live)) {
		log_sys_error("unlink", tc->path_live);
		return 0;
	}

	sync_dir(tc->path_live);

	return 1;
}

static int _scan_file(const struct format_type *fmt)
{
	struct dirent *dirent;
	struct dir_list *dl;
	struct list *dlh, *dir_list;
	char *tmp;
	DIR *d;
	struct volume_group *vg;
	struct format_instance *fid;
	char path[PATH_MAX];
	char *vgname;

	dir_list = &((struct mda_lists *) fmt->private)->dirs;

	list_iterate(dlh, dir_list) {
		dl = list_item(dlh, struct dir_list);
		if (!(d = opendir(dl->dir))) {
			log_sys_error("opendir", dl->dir);
			continue;
		}
		while ((dirent = readdir(d)))
			if (strcmp(dirent->d_name, ".") &&
			    strcmp(dirent->d_name, "..") &&
			    (!(tmp = strstr(dirent->d_name, ".tmp")) ||
			     tmp != dirent->d_name + strlen(dirent->d_name)
			     - 4)) {
				vgname = dirent->d_name;
				if (lvm_snprintf(path, PATH_MAX, "%s/%s",
						 dl->dir, vgname) < 0) {
					log_error("Name too long %s/%s",
						  dl->dir, vgname);
					break;
				}

				/* FIXME stat file to see if it's changed */
				fid = _create_text_instance(fmt, NULL, NULL);
				if ((vg = _vg_read_file_name(fid, vgname,
							     path)))
					lvmcache_update_vg(vg);
			}

		if (closedir(d))
			log_sys_error("closedir", dl->dir);
	}

	return 1;
}

int vgname_from_mda(const struct format_type *fmt, struct device_area *dev_area,
		    char *buf, uint32_t size)
{
	struct raw_locn *rlocn;
	struct mda_header *mdah;
	unsigned int len;
	int r = 0;

	if (!dev_open(dev_area->dev)) {
		stack;
		return 0;
	}

	if (!(mdah = _raw_read_mda_header(fmt, dev_area))) {
		stack;
		goto out;
	}

	rlocn = mdah->raw_locns;

	while (rlocn->offset) {
		if (!dev_read(dev_area->dev, dev_area->start + rlocn->offset,
			      size, buf)) {
			stack;
			goto out;
		}
		len = 0;
		while (buf[len] && !isspace(buf[len]) && buf[len] != '{' &&
		       len < (size - 1))
			len++;
		buf[len] = '\0';

		/* Ignore this entry if the characters aren't permissible */
		if (!validate_name(buf)) {
			stack;
			goto out;
		}

		r = 1;
		break;

		/* FIXME Cope with returning a list */
		rlocn++;
	}

      out:
	if (!dev_close(dev_area->dev))
		stack;

	return r;
}

static int _scan_raw(const struct format_type *fmt)
{
	struct raw_list *rl;
	struct list *rlh, *raw_list;
	char vgnamebuf[NAME_LEN + 2];
	struct volume_group *vg;
	struct format_instance fid;

	raw_list = &((struct mda_lists *) fmt->private)->raws;

	fid.fmt = fmt;
	list_init(&fid.metadata_areas);

	list_iterate(rlh, raw_list) {
		rl = list_item(rlh, struct raw_list);

		/* FIXME We're reading mdah twice here... */
		if (vgname_from_mda(fmt, &rl->dev_area, vgnamebuf,
				    sizeof(vgnamebuf))) {
			if ((vg = _vg_read_raw_area(&fid, vgnamebuf,
						    &rl->dev_area)))
				lvmcache_update_vg(vg);
		}
	}

	return 1;
}

static int _scan(const struct format_type *fmt)
{
	return (_scan_file(fmt) & _scan_raw(fmt));
}

/* For orphan, creates new mdas according to policy.
   Always have an mda between end-of-label and PE_ALIGN boundary */
static int _mda_setup(const struct format_type *fmt,
		      uint64_t pe_start, uint64_t pe_end,
		      int pvmetadatacopies,
		      uint64_t pvmetadatasize, struct list *mdas,
		      struct physical_volume *pv, struct volume_group *vg)
{
	uint64_t mda_adjustment, disk_size, alignment;
	uint64_t start1, mda_size1;	/* First area - start of disk */
	uint64_t start2, mda_size2;	/* Second area - end of disk */
	uint64_t wipe_size = 8 << SECTOR_SHIFT;

	if (!pvmetadatacopies) {
		/* Space available for PEs */
		pv->size -= PE_ALIGN;
		return 1;
	}

	alignment = PE_ALIGN << SECTOR_SHIFT;
	disk_size = pv->size << SECTOR_SHIFT;
	pe_start <<= SECTOR_SHIFT;
	pe_end <<= SECTOR_SHIFT;

	if (pe_end > disk_size) {
		log_error("Physical extents end beyond end of device %s!",
			  dev_name(pv->dev));
		return 0;
	}

	/* Requested metadatasize */
	mda_size1 = pvmetadatasize << SECTOR_SHIFT;

	/* Space available for PEs (before any mdas created) */
	pv->size -= LABEL_SCAN_SECTORS;

	/* Place mda straight after label area at start of disk */
	start1 = LABEL_SCAN_SIZE;

	/* Ensure it's not going to be bigger than the disk! */
	if (mda_size1 > disk_size) {
		log_print("Warning: metadata area fills disk %s",
			  dev_name(pv->dev));
		/* Leave some free space for rounding */
		/* Avoid empty data area as could cause tools problems */
		mda_size1 = disk_size - start1 - alignment * 2;
		/* Only have 1 mda in this case */
		pvmetadatacopies = 1;
	}

	/* Round up to PE_ALIGN boundary */
	mda_adjustment = (mda_size1 + start1) % alignment;
	if (mda_adjustment)
		mda_size1 += (alignment - mda_adjustment);

	/* If we already have PEs, avoid overlap */
	if (pe_start || pe_end) {
		if (pe_start <= start1)
			mda_size1 = 0;
		else if (start1 + mda_size1 > pe_start)
			mda_size1 = pe_start - start1;
	}

	/* FIXME If creating new mdas, wipe them! */
	if (mda_size1) {
		if (!add_mda(fmt, fmt->cmd->mem, mdas, pv->dev, start1,
			     mda_size1))
			return 0;

		if (!dev_zero((struct device *) pv->dev, start1,
			      (size_t) (mda_size1 >
					wipe_size ? wipe_size : mda_size1))) {
			log_error("Failed to wipe new metadata area");
			return 0;
		}

		pv->size -= mda_size1 >> SECTOR_SHIFT;
		if (pvmetadatacopies == 1)
			return 1;
	} else
		start1 = 0;

	/* A second copy at end of disk */
	mda_size2 = pvmetadatasize << SECTOR_SHIFT;

	/* Ensure it's not going to be bigger than the disk! */
	if (mda_size2 > disk_size)
		mda_size2 = disk_size - start1 - mda_size1;

	mda_adjustment = (disk_size - mda_size2) % alignment;
	if (mda_adjustment)
		mda_size2 += mda_adjustment;

	start2 = disk_size - mda_size2;

	/* If we already have PEs, avoid overlap */
	if (pe_start || pe_end) {
		if (start2 < pe_end) {
			mda_size2 -= (pe_end - start2);
			start2 = pe_end;
		}
	}

	/* If we already have a first mda, avoid overlap */
	if (mda_size1) {
		if (start2 < start1 + mda_size1) {
			mda_size2 -= (start1 + mda_size1 - start2);
			start2 = start1 + mda_size1;
		}
		/* No room for any PEs here now! */
	}

	if (mda_size2) {
		if (!add_mda(fmt, fmt->cmd->mem, mdas, pv->dev, start2,
			     mda_size2))
			return 0;
		if (!dev_zero(pv->dev, start2,
			      (size_t) (mda_size1 >
					wipe_size ? wipe_size : mda_size1))) {
			log_error("Failed to wipe new metadata area");
			return 0;
		}
		pv->size -= mda_size2 >> SECTOR_SHIFT;
	} else
		return 0;

	return 1;
}

/* Only for orphans */
/* Set label_sector to -1 if rewriting existing label into same sector */
static int _pv_write(const struct format_type *fmt, struct physical_volume *pv,
		     struct list *mdas, int64_t label_sector)
{
	struct label *label;
	struct lvmcache_info *info;
	struct mda_context *mdac;
	struct list *mdash;
	struct metadata_area *mda;
	char buf[MDA_HEADER_SIZE];
	struct mda_header *mdah = (struct mda_header *) buf;
	uint64_t adjustment;

	/* FIXME Test mode don't update cache? */

	if (!(info = lvmcache_add(fmt->labeller, (char *) &pv->id, pv->dev,
				  ORPHAN, NULL))) {
		stack;
		return 0;
	}
	label = info->label;

	if (label_sector != -1)
		label->sector = label_sector;

	info->device_size = pv->size << SECTOR_SHIFT;
	info->fmt = fmt;

	/* If mdas supplied, use them regardless of existing ones, */
	/* otherwise retain existing ones */
	if (mdas) {
		if (info->mdas.n)
			del_mdas(&info->mdas);
		else
			list_init(&info->mdas);
		list_iterate(mdash, mdas) {
			mda = list_item(mdash, struct metadata_area);
			mdac = mda->metadata_locn;
			log_debug("Creating metadata area on %s at sector %"
				  PRIu64 " size %" PRIu64 " sectors",
				  dev_name(mdac->area.dev),
				  mdac->area.start >> SECTOR_SHIFT,
				  mdac->area.size >> SECTOR_SHIFT);
			add_mda(fmt, NULL, &info->mdas, mdac->area.dev,
				mdac->area.start, mdac->area.size);
		}
		/* FIXME Temporary until mda creation supported by tools */
	} else if (!info->mdas.n) {
		list_init(&info->mdas);
	}

	if (info->das.n)
		del_das(&info->das);
	else
		list_init(&info->das);

	/* Set pe_start to first aligned sector after any metadata 
	 * areas that begin before pe_start */
	pv->pe_start = PE_ALIGN;
	list_iterate(mdash, &info->mdas) {
		mda = list_item(mdash, struct metadata_area);
		mdac = (struct mda_context *) mda->metadata_locn;
		if (pv->dev == mdac->area.dev &&
		    (mdac->area.start < (pv->pe_start << SECTOR_SHIFT)) &&
		    (mdac->area.start + mdac->area.size >
		     (pv->pe_start << SECTOR_SHIFT))) {
			pv->pe_start = (mdac->area.start + mdac->area.size)
			    >> SECTOR_SHIFT;
			adjustment = pv->pe_start % PE_ALIGN;
			if (adjustment)
				pv->pe_start += (PE_ALIGN - adjustment);
		}
	}
	if (!add_da
	    (fmt, NULL, &info->das, pv->pe_start << SECTOR_SHIFT,
	     UINT64_C(0))) {
		stack;
		return 0;
	}

	if (!dev_open(pv->dev)) {
		stack;
		return 0;
	}

	list_iterate(mdash, &info->mdas) {
		mda = list_item(mdash, struct metadata_area);
		mdac = mda->metadata_locn;
		memset(&buf, 0, sizeof(buf));
		mdah->size = mdac->area.size;
		if (!_raw_write_mda_header(fmt, mdac->area.dev,
					   mdac->area.start, mdah)) {
			stack;
			if (!dev_close(pv->dev))
				stack;
			return 0;
		}
	}

	label_write(pv->dev, label);

	if (!dev_close(pv->dev)) {
		stack;
		return 0;
	}

	return 1;
}

static int _get_pv_from_vg(const struct format_type *fmt, const char *vg_name,
			   const char *id, struct physical_volume *pv)
{
	struct volume_group *vg;
	struct list *pvh;
	struct pv_list *pvl;
	int consistent = 0;

	if (!(vg = vg_read(fmt->cmd, vg_name, &consistent))) {
		log_error("format_text: _vg_read failed to read VG %s",
			  vg_name);
		return 0;
	}

	if (!consistent)
		log_error("Warning: Volume group %s is not consistent",
			  vg_name);

	list_iterate(pvh, &vg->pvs) {
		pvl = list_item(pvh, struct pv_list);
		if (id_equal(&pvl->pv->id, (const struct id *) id)) {
			memcpy(pv, pvl->pv, sizeof(*pv));
			return 1;
		}
	}
	return 0;
}

static int _add_raw(struct list *raw_list, struct device_area *dev_area)
{
	struct raw_list *rl;
	struct list *rlh;

	/* Already present? */
	list_iterate(rlh, raw_list) {
		rl = list_item(rlh, struct raw_list);
		/* FIXME Check size/overlap consistency too */
		if (rl->dev_area.dev == dev_area->dev &&
		    rl->dev_area.start == dev_area->start)
			return 1;
	}

	if (!(rl = dbg_malloc(sizeof(struct raw_list)))) {
		log_error("_add_raw allocation failed");
		return 0;
	}
	memcpy(&rl->dev_area, dev_area, sizeof(*dev_area));
	list_add(raw_list, &rl->list);

	return 1;
}

static int _pv_read(const struct format_type *fmt, const char *pv_name,
		    struct physical_volume *pv, struct list *mdas)
{
	struct label *label;
	struct device *dev;
	struct lvmcache_info *info;
	struct metadata_area *mda, *mda_new;
	struct mda_context *mdac, *mdac_new;
	struct list *mdah, *dah;
	struct data_area_list *da;

	if (!(dev = dev_cache_get(pv_name, fmt->cmd->filter))) {
		stack;
		return 0;
	}

	/* FIXME Optimise out repeated reading when cache lock held */
	if (!(label_read(dev, &label))) {
		stack;
		return 0;
	}
	info = (struct lvmcache_info *) label->info;

	/* Have we already cached vgname? */
	if (info->vginfo && info->vginfo->vgname && *info->vginfo->vgname &&
	    _get_pv_from_vg(info->fmt, info->vginfo->vgname, info->dev->pvid,
			    pv)) {
		return 1;
	}

	/* Perform full scan and try again */
	if (!memlock()) {
		lvmcache_label_scan(fmt->cmd, 1);

		if (info->vginfo && info->vginfo->vgname &&
		    *info->vginfo->vgname &&
		    _get_pv_from_vg(info->fmt, info->vginfo->vgname,
				    info->dev->pvid, pv)) {
			return 1;
		}
	}

	/* Orphan */
	pv->dev = info->dev;
	pv->fmt = info->fmt;
	pv->size = info->device_size >> SECTOR_SHIFT;
	pv->vg_name = ORPHAN;
	memcpy(&pv->id, &info->dev->pvid, sizeof(pv->id));

	/* Currently only support exactly one data area */
	if (list_size(&info->das) != 1) {
		log_error("Must be exactly one data area (found %d) on PV %s",
			  list_size(&info->das), dev_name(dev));
		return 0;
	}
	list_iterate(dah, &info->das) {
		da = list_item(dah, struct data_area_list);
		pv->pe_start = da->disk_locn.offset >> SECTOR_SHIFT;
	}

	if (!mdas)
		return 1;

	/* Add copy of mdas to supplied list */
	list_iterate(mdah, &info->mdas) {
		mda = list_item(mdah, struct metadata_area);
		mdac = (struct mda_context *) mda->metadata_locn;
		if (!(mda_new = pool_alloc(fmt->cmd->mem, sizeof(*mda_new)))) {
			log_error("metadata_area allocation failed");
			return 0;
		}
		if (!(mdac_new = pool_alloc(fmt->cmd->mem, sizeof(*mdac_new)))) {
			log_error("metadata_area allocation failed");
			return 0;
		}
		memcpy(mda_new, mda, sizeof(*mda));
		memcpy(mdac_new, mdac, sizeof(*mdac));
		mda_new->metadata_locn = mdac_new;
		list_add(mdas, &mda_new->list);
	}

	return 1;
}

static void _destroy_instance(struct format_instance *fid)
{
	return;
}

static void _free_dirs(struct list *dir_list)
{
	struct list *dl, *tmp;

	list_iterate_safe(dl, tmp, dir_list) {
		list_del(dl);
		dbg_free(dl);
	}
}

static void _free_raws(struct list *raw_list)
{
	struct list *rl, *tmp;

	list_iterate_safe(rl, tmp, raw_list) {
		list_del(rl);
		dbg_free(rl);
	}
}

static void _destroy(const struct format_type *fmt)
{
	if (fmt->private) {
		_free_dirs(&((struct mda_lists *) fmt->private)->dirs);
		_free_raws(&((struct mda_lists *) fmt->private)->raws);
		dbg_free(fmt->private);
	}

	dbg_free((void *) fmt);
}

static struct metadata_area_ops _metadata_text_file_ops = {
	vg_read:_vg_read_file,
	vg_write:_vg_write_file,
	vg_remove:_vg_remove_file,
	vg_commit:_vg_commit_file
};

static struct metadata_area_ops _metadata_text_file_backup_ops = {
	vg_read:_vg_read_file,
	vg_write:_vg_write_file,
	vg_remove:_vg_remove_file,
	vg_commit:_vg_commit_file_backup
};

static struct metadata_area_ops _metadata_text_raw_ops = {
	vg_read:_vg_read_raw,
	vg_write:_vg_write_raw,
	vg_remove:_vg_remove_raw,
	vg_commit:_vg_commit_raw,
	vg_revert:_vg_revert_raw
};

/* pvmetadatasize in sectors */
static int _pv_setup(const struct format_type *fmt,
		     uint64_t pe_start, uint32_t extent_count,
		     uint32_t extent_size,
		     int pvmetadatacopies,
		     uint64_t pvmetadatasize, struct list *mdas,
		     struct physical_volume *pv, struct volume_group *vg)
{
	struct metadata_area *mda, *mda_new, *mda2;
	struct mda_context *mdac, *mdac_new, *mdac2;
	struct list *pvmdas, *pvmdash, *mdash;
	struct lvmcache_info *info;
	int found;
	uint64_t pe_end = 0;

	/* FIXME if vg, adjust start/end of pe area to avoid mdas! */

	/* FIXME Cope with pvchange */
	/* FIXME Merge code with _create_text_instance */

	/* If new vg, add any further mdas on this PV to the fid's mda list */
	if (vg) {
		/* Iterate through all mdas on this PV */
		if ((info = info_from_pvid(pv->dev->pvid))) {
			pvmdas = &info->mdas;
			list_iterate(pvmdash, pvmdas) {
				mda = list_item(pvmdash, struct metadata_area);
				mdac =
				    (struct mda_context *) mda->metadata_locn;

				/* FIXME Check it isn't already in use */

				/* Ensure it isn't already on list */
				found = 0;
				list_iterate(mdash, mdas) {
					mda2 =
					    list_item(mdash,
						      struct metadata_area);
					if (mda2->ops !=
					    &_metadata_text_raw_ops)
						continue;
					mdac2 =
					    (struct mda_context *) mda2->
					    metadata_locn;
					if (!memcmp
					    (&mdac2->area, &mdac->area,
					     sizeof(mdac->area))) {
						found = 1;
						break;
					}
				}
				if (found)
					continue;

				if (!(mda_new = pool_alloc(fmt->cmd->mem,
							   sizeof(*mda_new)))) {
					stack;
					return 0;
				}

				if (!(mdac_new = pool_alloc(fmt->cmd->mem,
							    sizeof(*mdac_new))))
				{
					stack;
					return 0;
				}
				/* FIXME multiple dev_areas inside area */
				memcpy(mda_new, mda, sizeof(*mda));
				memcpy(mdac_new, mdac, sizeof(*mdac));
				mda_new->metadata_locn = mdac_new;
				list_add(mdas, &mda_new->list);
			}
		}

		/* Unlike LVM1, we don't store this outside a VG */
		/* FIXME Default from config file? vgextend cmdline flag? */
		pv->status |= ALLOCATABLE_PV;
	} else {
		if (extent_count)
			pe_end = pe_start + extent_count * extent_size - 1;
		if (!_mda_setup(fmt, pe_start, pe_end, pvmetadatacopies,
				pvmetadatasize, mdas, pv, vg)) {
			stack;
			return 0;
		}

	}

	return 1;
}

/* NULL vgname means use only the supplied context e.g. an archive file */
static struct format_instance *_create_text_instance(const struct format_type
						     *fmt, const char *vgname,
						     void *context)
{
	struct format_instance *fid;
	struct metadata_area *mda, *mda_new;
	struct mda_context *mdac, *mdac_new;
	struct dir_list *dl;
	struct raw_list *rl;
	struct list *dlh, *dir_list, *rlh, *raw_list, *mdas, *mdash, *infoh;
	char path[PATH_MAX];
	struct lvmcache_vginfo *vginfo;

	if (!(fid = pool_alloc(fmt->cmd->mem, sizeof(*fid)))) {
		log_error("Couldn't allocate format instance object.");
		return NULL;
	}

	fid->fmt = fmt;

	list_init(&fid->metadata_areas);

	if (!vgname) {
		if (!(mda = pool_alloc(fmt->cmd->mem, sizeof(*mda)))) {
			stack;
			return NULL;
		}
		mda->ops = &_metadata_text_file_backup_ops;
		mda->metadata_locn = context;
		list_add(&fid->metadata_areas, &mda->list);
	} else {
		dir_list = &((struct mda_lists *) fmt->private)->dirs;

		list_iterate(dlh, dir_list) {
			dl = list_item(dlh, struct dir_list);
			if (lvm_snprintf(path, PATH_MAX, "%s/%s",
					 dl->dir, vgname) < 0) {
				log_error("Name too long %s/%s", dl->dir,
					  vgname);
				return NULL;
			}

			context = create_text_context(fmt->cmd, path, NULL);
			if (!(mda = pool_alloc(fmt->cmd->mem, sizeof(*mda)))) {
				stack;
				return NULL;
			}
			mda->ops = &_metadata_text_file_ops;
			mda->metadata_locn = context;
			list_add(&fid->metadata_areas, &mda->list);
		}

		raw_list = &((struct mda_lists *) fmt->private)->raws;

		list_iterate(rlh, raw_list) {
			rl = list_item(rlh, struct raw_list);

			/* FIXME Cache this; rescan below if some missing */
			if (!_raw_holds_vgname(fid, &rl->dev_area, vgname))
				continue;

			if (!(mda = pool_alloc(fmt->cmd->mem, sizeof(*mda)))) {
				stack;
				return NULL;
			}

			if (!(mdac = pool_alloc(fmt->cmd->mem, sizeof(*mdac)))) {
				stack;
				return NULL;
			}
			mda->metadata_locn = mdac;
			/* FIXME Allow multiple dev_areas inside area */
			memcpy(&mdac->area, &rl->dev_area, sizeof(mdac->area));
			mda->ops = &_metadata_text_raw_ops;
			/* FIXME MISTAKE? mda->metadata_locn = context; */
			list_add(&fid->metadata_areas, &mda->list);
		}

		/* Scan PVs in VG for any further MDAs */
		lvmcache_label_scan(fmt->cmd, 0);
		if (!(vginfo = vginfo_from_vgname(vgname))) {
			stack;
			goto out;
		}
		list_iterate(infoh, &vginfo->infos) {
			mdas = &(list_item(infoh, struct lvmcache_info)->mdas);
			list_iterate(mdash, mdas) {
				mda = list_item(mdash, struct metadata_area);
				mdac =
				    (struct mda_context *) mda->metadata_locn;

				/* FIXME Check it holds this VG */
				if (!(mda_new = pool_alloc(fmt->cmd->mem,
							   sizeof(*mda_new)))) {
					stack;
					return NULL;
				}

				if (!(mdac_new = pool_alloc(fmt->cmd->mem,
							    sizeof(*mdac_new))))
				{
					stack;
					return NULL;
				}
				/* FIXME multiple dev_areas inside area */
				memcpy(mda_new, mda, sizeof(*mda));
				memcpy(mdac_new, mdac, sizeof(*mdac));
				mda_new->metadata_locn = mdac_new;
				list_add(&fid->metadata_areas, &mda_new->list);
			}
		}
		/* FIXME Check raw metadata area count - rescan if required */
	}

      out:
	return fid;

}

void *create_text_context(struct cmd_context *cmd, const char *path,
			  const char *desc)
{
	struct text_context *tc;
	char *tmp;

	if ((tmp = strstr(path, ".tmp")) && (tmp == path + strlen(path) - 4)) {
		log_error("%s: Volume group filename may not end in .tmp",
			  path);
		return NULL;
	}

	if (!(tc = pool_alloc(cmd->mem, sizeof(*tc)))) {
		stack;
		return NULL;
	}

	if (!(tc->path_live = pool_strdup(cmd->mem, path))) {
		stack;
		goto no_mem;
	}

	if (!(tc->path_edit = pool_alloc(cmd->mem, strlen(path) + 5))) {
		stack;
		goto no_mem;
	}
	sprintf(tc->path_edit, "%s.tmp", path);

	if (!desc)
		desc = "";

	if (!(tc->desc = pool_strdup(cmd->mem, desc))) {
		stack;
		goto no_mem;
	}

	return (void *) tc;

      no_mem:
	pool_free(cmd->mem, tc);

	log_err("Couldn't allocate text format context object.");
	return NULL;
}

static struct format_handler _text_handler = {
	scan:_scan,
	pv_read:_pv_read,
	pv_setup:_pv_setup,
	pv_write:_pv_write,
	vg_setup:_vg_setup,
	lv_setup:_lv_setup,
	create_instance:_create_text_instance,
	destroy_instance:_destroy_instance,
	destroy:_destroy
};

static int _add_dir(const char *dir, struct list *dir_list)
{
	struct dir_list *dl;

	if (create_dir(dir)) {
		if (!(dl = dbg_malloc(sizeof(struct list) + strlen(dir) + 1))) {
			log_error("_add_dir allocation failed");
			return 0;
		}
		log_very_verbose("Adding text format metadata dir: %s", dir);
		strcpy(dl->dir, dir);
		list_add(dir_list, &dl->list);
		return 1;
	}

	return 0;
}

static int _get_config_disk_area(struct cmd_context *cmd,
				 struct config_node *cn, struct list *raw_list)
{
	struct device_area dev_area;
	char *id_str;
	struct id id;

	if (!(cn = cn->child)) {
		log_error("Empty metadata disk_area section of config file");
		return 0;
	}

	if (!get_config_uint64(cn, "start_sector", '/', &dev_area.start)) {
		log_error("Missing start_sector in metadata disk_area section "
			  "of config file");
		return 0;
	}
	dev_area.start <<= SECTOR_SHIFT;

	if (!get_config_uint64(cn, "size", '/', &dev_area.size)) {
		log_error("Missing size in metadata disk_area section "
			  "of config file");
		return 0;
	}
	dev_area.size <<= SECTOR_SHIFT;

	if (!get_config_str(cn, "id", '/', &id_str)) {
		log_error("Missing uuid in metadata disk_area section "
			  "of config file");
		return 0;
	}

	if (!id_read_format(&id, id_str)) {
		log_error("Invalid uuid in metadata disk_area section "
			  "of config file: %s", id_str);
		return 0;
	}

	if (!(dev_area.dev = device_from_pvid(cmd, &id))) {
		char buffer[64];

		if (!id_write_format(&id, buffer, sizeof(buffer)))
			log_err("Couldn't find device.");
		else
			log_err("Couldn't find device with uuid '%s'.", buffer);

		return 0;
	}

	return _add_raw(raw_list, &dev_area);
}

struct format_type *create_text_format(struct cmd_context *cmd)
{
	struct format_type *fmt;
	struct config_node *cn;
	struct config_value *cv;
	struct mda_lists *mda_lists;

	if (!(fmt = dbg_malloc(sizeof(*fmt)))) {
		stack;
		return NULL;
	}

	fmt->cmd = cmd;
	fmt->ops = &_text_handler;
	fmt->name = FMT_TEXT_NAME;
	fmt->alias = FMT_TEXT_ALIAS;
	fmt->features = FMT_SEGMENTS | FMT_MDAS;

	if (!(mda_lists = dbg_malloc(sizeof(struct mda_lists)))) {
		log_error("Failed to allocate dir_list");
		return NULL;
	}

	list_init(&mda_lists->dirs);
	list_init(&mda_lists->raws);
	mda_lists->file_ops = &_metadata_text_file_ops;
	mda_lists->raw_ops = &_metadata_text_raw_ops;
	fmt->private = (void *) mda_lists;

	if (!(fmt->labeller = text_labeller_create(fmt))) {
		log_error("Couldn't create text label handler.");
		return NULL;
	}

	if (!(label_register_handler(FMT_TEXT_NAME, fmt->labeller))) {
		log_error("Couldn't register text label handler.");
		return NULL;
	}

	if ((cn = find_config_node(cmd->cf->root, "metadata/dirs", '/'))) {
		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != CFG_STRING) {
				log_error("Invalid string in config file: "
					  "metadata/dirs");
				goto err;
			}

			if (!_add_dir(cv->v.str, &mda_lists->dirs)) {
				log_error("Failed to add %s to internal device "
					  "cache", cv->v.str);
				goto err;
			}
		}
	}

	if (!(cn = find_config_node(cmd->cf->root, "metadata/disk_areas", '/')))
		return fmt;

	for (cn = cn->child; cn; cn = cn->sib) {
		if (!_get_config_disk_area(cmd, cn, &mda_lists->raws))
			goto err;
	}

	return fmt;

      err:
	_free_dirs(&mda_lists->dirs);

	dbg_free(fmt);
	return NULL;
}
