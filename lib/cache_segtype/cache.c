/*
 * Copyright (C) 2013-2014 Red Hat, Inc. All rights reserved.
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
#include "toolcontext.h"
#include "segtype.h"
#include "display.h"
#include "text_export.h"
#include "config.h"
#include "str_list.h"
#include "targets.h"
#include "lvm-string.h"
#include "activate.h"
#include "metadata.h"
#include "lv_alloc.h"
#include "defaults.h"

#define SEG_LOG_ERROR(t, p...) \
        log_error(t " segment %s of logical volume %s.", ## p,	\
                  dm_config_parent_name(sn), seg->lv->name), 0;


static const char *_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static int _cache_pool_text_import(struct lv_segment *seg,
				   const struct dm_config_node *sn,
				   struct dm_hash_table *pv_hash __attribute__((unused)))
{
	uint32_t chunk_size;
	struct logical_volume *data_lv, *meta_lv;
	const char *str = NULL;
	char *argv_str;
	struct dm_pool *mem = seg->lv->vg->vgmem; //FIXME: what mempool should be used?

	if (!dm_config_has_node(sn, "data"))
		return SEG_LOG_ERROR("Cache data not specified in");
	if (!(str = dm_config_find_str(sn, "data", NULL)))
		return SEG_LOG_ERROR("Cache data must be a string in");
	if (!(data_lv = find_lv(seg->lv->vg, str)))
		return SEG_LOG_ERROR("Unknown logical volume %s specified for "
			  "cache data in", str);

	if (!dm_config_has_node(sn, "metadata"))
		return SEG_LOG_ERROR("Cache metadata not specified in");
	if (!(str = dm_config_find_str(sn, "metadata", NULL)))
		return SEG_LOG_ERROR("Cache metadata must be a string in");
	if (!(meta_lv = find_lv(seg->lv->vg, str)))
		return SEG_LOG_ERROR("Unknown logical volume %s specified for "
			  "cache metadata in", str);

	if (!dm_config_get_uint32(sn, "chunk_size", &chunk_size))
		return SEG_LOG_ERROR("Couldn't read cache chunk_size in");

	/*
	 * Read in features:
	 *   cache_mode = {writethrough|writeback}
	 *
	 *   'cache_mode' does not have to be present.
	 */
	if (dm_config_has_node(sn, "cache_mode")) {
		if (!(str = dm_config_find_str(sn, "cache_mode", NULL)))
			return SEG_LOG_ERROR("cache_mode must be a string in");
		if (!get_cache_mode(str, &seg->feature_flags))
			return SEG_LOG_ERROR("Unknown cache_mode in");
	}

	/*
	 * Read in core arguments (these are key/value pairs)
	 *   core_argc = <# args>
	 *   core_argv = "[<key> <value>]..."
	 *
	 *   'core_argc' does not have to be present.  If it is not present,
	 *   any other core_* fields are ignored.  If it is present, then
	 *   'core_argv' must be present - even if they are
	 *   'core_argc = 0' and 'core_argv = ""'.
	 */
	if (dm_config_has_node(sn, "core_argc")) {
		if (!dm_config_has_node(sn, "core_argv"))
			return SEG_LOG_ERROR("not all core arguments defined in");

		if (!dm_config_get_uint32(sn, "core_argc", &seg->core_argc))
			return SEG_LOG_ERROR("Unable to read core_argc in");

		str = dm_config_find_str(sn, "core_argv", NULL);
		if ((str && !seg->core_argc) || (!str && seg->core_argc))
			return SEG_LOG_ERROR("core_argc and core_argv do"
					     " not match in");

		if (!(seg->core_argv =
		      dm_pool_alloc(mem, sizeof(char *) * seg->core_argc)))
			return_0;
		if (str &&
		    (!(argv_str = dm_pool_strdup(mem, str)) ||
		     ((int)seg->core_argc != dm_split_words(argv_str, seg->core_argc,
							    0, (char **) seg->core_argv))))
			return SEG_LOG_ERROR("core_argc and core_argv do"
					     " not match in");
	}

	/*
	 * Read in policy:
	 *   policy_name = "<policy_name>"
	 *   policy_argc = <# args>
	 *   policy_argv = "[<key> <value>]..."
	 *
	 *   'policy_name' does not have to be present.  If it is not present,
	 *   any other policy_* fields are ignored.  If it is present, then
	 *   the other policy_* fields must be present - even if they are
	 *   'policy_argc = 0' and 'policy_argv = ""'.
	 */
	if (dm_config_has_node(sn, "policy_name")) {
		if (!dm_config_has_node(sn, "policy_argc") ||
		    !dm_config_has_node(sn, "policy_argv"))
			return SEG_LOG_ERROR("not all policy arguments defined in");
		if (!(str = dm_config_find_str(sn, "policy_name", NULL)))
			return SEG_LOG_ERROR("policy_name must be a string in");
		seg->policy_name = dm_pool_strdup(mem, str);

		if (!dm_config_get_uint32(sn, "policy_argc", &seg->policy_argc))
			return SEG_LOG_ERROR("Unable to read policy_argc in");

		str = dm_config_find_str(sn, "policy_argv", NULL);
		if ((str && !seg->policy_argc) || (!str && seg->policy_argc))
			return SEG_LOG_ERROR("policy_argc and policy_argv do"
					     " not match in");

		if (!(seg->policy_argv =
		      dm_pool_alloc(mem, sizeof(char *) * seg->policy_argc)))
			return_0;
		if (str &&
		    (!(argv_str = dm_pool_strdup(mem, str)) ||
		     ((int)seg->policy_argc != dm_split_words(argv_str,
							      seg->policy_argc,
							      0, (char **) seg->policy_argv))))
			return SEG_LOG_ERROR("policy_argc and policy_argv do"
					     " not match in");
	}

	if (!attach_pool_data_lv(seg, data_lv))
		return_0;
	if (!attach_pool_metadata_lv(seg, meta_lv))
		return_0;
	seg->chunk_size = chunk_size;

	return 1;
}

static int _cache_pool_text_import_area_count(const struct dm_config_node *sn,
					      uint32_t *area_count)
{
	*area_count = 1;

	return 1;
}

static int _cache_pool_text_export(const struct lv_segment *seg,
				   struct formatter *f)
{
	unsigned i;
	char buf[256]; //FIXME: IS THERE AN 'outf' THAT DOESN'T DO NEWLINE?!?
	uint32_t feature_flags = seg->feature_flags;

	outf(f, "data = \"%s\"", seg_lv(seg, 0)->name);
	outf(f, "metadata = \"%s\"", seg->metadata_lv->name);
	outf(f, "chunk_size = %" PRIu32, seg->chunk_size);

	if (feature_flags) {
		if (feature_flags & DM_CACHE_FEATURE_WRITETHROUGH) {
			outf(f, "cache_mode = \"writethrough\"");
			feature_flags &= ~DM_CACHE_FEATURE_WRITETHROUGH;
		} else if (feature_flags & DM_CACHE_FEATURE_WRITEBACK) {
			outf(f, "cache_mode = \"writeback\"");
			feature_flags &= ~DM_CACHE_FEATURE_WRITEBACK;
		} else {
			log_error(INTERNAL_ERROR "Unknown feature flags "
				  "in cache_pool segment for %s", seg->lv->name);
			return 0;
		}
	}

	if (seg->core_argc) {
		outf(f, "core_argc = %u", seg->core_argc);
		outf(f, "core_argv = \"");
		for (i = 0; i < seg->core_argc; i++)
			outf(f, "%s%s", i ? " " : "", seg->core_argv[i]);
		outf(f, "\"");
	}

	if (seg->policy_name) {
		outf(f, "policy_name = \"%s\"", seg->policy_name);
		outf(f, "policy_argc = %u", seg->policy_argc);
		buf[0] = '\0';
		for (i = 0; i < seg->policy_argc; i++)
			sprintf(buf, "%s%s", i ? " " : "", seg->policy_argv[i]);
		outf(f, "policy_argv = \"%s\"", buf);
	}

	return 1;
}

static void _destroy(struct segment_type *segtype)
{
	dm_free((void *) segtype);
}

#ifdef DEVMAPPER_SUPPORT
static int _target_present(struct cmd_context *cmd,
				const struct lv_segment *seg __attribute__((unused)),
				unsigned *attributes __attribute__((unused)))
{
	uint32_t maj, min, patchlevel;
	static int _cache_checked = 0;
	static int _cache_present = 0;

	if (!_cache_checked) {
		_cache_present = target_present(cmd, "cache", 1);

		if (!target_version("cache", &maj, &min, &patchlevel)) {
			log_error("Failed to determine version of cache kernel module");
			return 0;
		}

		_cache_checked = 1;

		if ((maj < 1) ||
		    ((maj == 1) && (min < 3))) {
			log_error("The cache kernel module is version %u.%u.%u."
				  "  Version 1.3.0+ is required.",
				  maj, min, patchlevel);
			return 0;
		}
	}

	return _cache_present;
}

static int _modules_needed(struct dm_pool *mem,
			   const struct lv_segment *seg __attribute__((unused)),
			   struct dm_list *modules)
{
	if (!str_list_add(mem, modules, "cache")) {
		log_error("String list allocation failed for cache module.");
		return 0;
	}

	return 1;
}
#endif /* DEVMAPPER_SUPPORT */

static struct segtype_handler _cache_pool_ops = {
	.name = _name,
	.text_import = _cache_pool_text_import,
	.text_import_area_count = _cache_pool_text_import_area_count,
	.text_export = _cache_pool_text_export,
#ifdef DEVMAPPER_SUPPORT
	.target_present = _target_present,
	.modules_needed = _modules_needed,
#  ifdef DMEVENTD
#  endif        /* DMEVENTD */
#endif
	.destroy = _destroy,
};

static int _cache_text_import(struct lv_segment *seg,
			      const struct dm_config_node *sn,
			      struct dm_hash_table *pv_hash __attribute__((unused)))
{
	struct logical_volume *pool_lv, *origin_lv;
	const char *name = NULL;

	if (!dm_config_has_node(sn, "cache_pool"))
		return SEG_LOG_ERROR("cache_pool not specified in");
	if (!(name = dm_config_find_str(sn, "cache_pool", NULL)))
		return SEG_LOG_ERROR("cache_pool must be a string in");
	if (!(pool_lv = find_lv(seg->lv->vg, name)))
		return SEG_LOG_ERROR("Unknown logical volume %s specified for "
			  "cache_pool in", name);

	if (!dm_config_has_node(sn, "origin"))
		return SEG_LOG_ERROR("Cache origin not specified in");
	if (!(name = dm_config_find_str(sn, "origin", NULL)))
		return SEG_LOG_ERROR("Cache origin must be a string in");
	if (!(origin_lv = find_lv(seg->lv->vg, name)))
		return SEG_LOG_ERROR("Unknown logical volume %s specified for "
			  "cache origin in", name);

	if (!set_lv_segment_area_lv(seg, 0, origin_lv, 0, 0))
		return_0;
	if (!attach_pool_lv(seg, pool_lv, NULL, NULL))
		return_0;

	return 1;
}

static int _cache_text_import_area_count(const struct dm_config_node *sn,
					 uint32_t *area_count)
{
	*area_count = 1;

	return 1;
}

static int _cache_text_export(const struct lv_segment *seg, struct formatter *f)
{
	if (!seg_lv(seg, 0))
		return_0;

	outf(f, "cache_pool = \"%s\"", seg->pool_lv->name);
	outf(f, "origin = \"%s\"", seg_lv(seg, 0)->name);

	return 1;
}

#ifdef DEVMAPPER_SUPPORT
static int _cache_add_target_line(struct dev_manager *dm,
				 struct dm_pool *mem,
				 struct cmd_context *cmd __attribute__((unused)),
				 void **target_state __attribute__((unused)),
				 struct lv_segment *seg,
				 const struct lv_activate_opts *laopts __attribute__((unused)),
				 struct dm_tree_node *node, uint64_t len,
				 uint32_t *pvmove_mirror_count __attribute__((unused)))
{
	struct lv_segment *cache_pool_seg = first_seg(seg->pool_lv);
	char *metadata_uuid, *data_uuid, *origin_uuid;

	if (!(metadata_uuid = build_dm_uuid(mem, cache_pool_seg->metadata_lv, NULL)))
		return_0;

	if (!(data_uuid = build_dm_uuid(mem, seg_lv(cache_pool_seg, 0), NULL)))
		return_0;

	if (!(origin_uuid = build_dm_uuid(mem, seg_lv(seg, 0), NULL)))
		return_0;

	if (!dm_tree_node_add_cache_target(node, len,
					   metadata_uuid,
					   data_uuid,
					   origin_uuid,
					   cache_pool_seg->chunk_size,
					   cache_pool_seg->feature_flags,
					   cache_pool_seg->core_argc,
					   cache_pool_seg->core_argv,
					   cache_pool_seg->policy_name,
					   cache_pool_seg->policy_argc,
					   cache_pool_seg->policy_argv))
		return_0;

	return add_areas_line(dm, seg, node, 0u, seg->area_count);
}
#endif /* DEVMAPPER_SUPPORT */

static struct segtype_handler _cache_ops = {
	.name = _name,
	.text_import = _cache_text_import,
	.text_import_area_count = _cache_text_import_area_count,
	.text_export = _cache_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _cache_add_target_line,
	.target_present = _target_present,
	.modules_needed = _modules_needed,
#  ifdef DMEVENTD
#  endif        /* DMEVENTD */
#endif
	.destroy = _destroy,
};

#ifdef CACHE_INTERNAL /* Shared */
int init_cache_segtypes(struct cmd_context *cmd,
			struct segtype_library *seglib)
#else
int init_cache_segtypes(struct cmd_context *cmd,
			struct segtype_library *seglib);
int init_cache_segtypes(struct cmd_context *cmd,
			struct segtype_library *seglib)
#endif
{
	struct segment_type *segtype = dm_zalloc(sizeof(*segtype));

	if (!segtype) {
		log_error("Failed to allocate memory for cache_pool segtype");
		return 0;
	}
	segtype->cmd = cmd;

	segtype->name = "cache-pool";
	segtype->flags = SEG_CACHE_POOL;
	segtype->ops = &_cache_pool_ops;
	segtype->private = NULL;

	if (!lvm_register_segtype(seglib, segtype))
		return_0;
	log_very_verbose("Initialised segtype: %s", segtype->name);

	segtype = dm_zalloc(sizeof(*segtype));
	if (!segtype) {
		log_error("Failed to allocate memory for cache segtype");
		return 0;
	}
	segtype->cmd = cmd;

	segtype->name = "cache";
	segtype->flags = SEG_CACHE;
	segtype->ops = &_cache_ops;
	segtype->private = NULL;

	if (!lvm_register_segtype(seglib, segtype))
		return_0;
	log_very_verbose("Initialised segtype: %s", segtype->name);

	return 1;
}
