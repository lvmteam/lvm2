/*
 * Copyright (C) 2011-2012 Red Hat, Inc.
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

#ifndef _LVM_DAEMON_CONFIG_UTIL_H
#define _LVM_DAEMON_CONFIG_UTIL_H

#include "configure.h"
#include "libdevmapper.h"

#include <stdarg.h>

struct buffer {
	int allocated;
	int used;
	char *mem;
};

int buffer_append_vf(struct buffer *buf, va_list ap);
int buffer_append_f(struct buffer *buf, ...);
int buffer_append(struct buffer *buf, const char *string);
void buffer_init(struct buffer *buf);
void buffer_destroy(struct buffer *buf);
int buffer_realloc(struct buffer *buf, int required);

int buffer_line(const char *line, void *baton);

int set_flag(struct dm_config_tree *cft, struct dm_config_node *parent,
	     const char *field, const char *flag, int want);

struct dm_config_node *make_config_node(struct dm_config_tree *cft,
					const char *key,
					struct dm_config_node *parent,
					struct dm_config_node *pre_sib);

struct dm_config_node *make_text_node(struct dm_config_tree *cft,
				      const char *key,
				      const char *value,
				      struct dm_config_node *parent,
				      struct dm_config_node *pre_sib);

struct dm_config_node *make_int_node(struct dm_config_tree *cft,
				     const char *key,
				     int64_t value,
				     struct dm_config_node *parent,
				     struct dm_config_node *pre_sib);

struct dm_config_node *config_make_nodes_v(struct dm_config_tree *cft,
					   struct dm_config_node *parent,
					   struct dm_config_node *pre_sib,
					   va_list ap);
struct dm_config_node *config_make_nodes(struct dm_config_tree *cft,
					 struct dm_config_node *parent,
					 struct dm_config_node *pre_sib,
					 ...);

#endif /* _LVM_DAEMON_SHARED_H */
