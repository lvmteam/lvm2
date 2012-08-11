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

#include <errno.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>

#include "dm-logging.h"
#include "daemon-shared.h"
#include "libdevmapper.h"

/*
 * Read a single message from a (socket) filedescriptor. Messages are delimited
 * by blank lines. This call will block until all of a message is received. The
 * memory will be allocated from heap. Upon error, all memory is freed and the
 * buffer pointer is set to NULL.
 *
 * See also write_buffer about blocking (read_buffer has identical behaviour).
 */
int read_buffer(int fd, char **buffer) {
	int bytes = 0;
	int buffersize = 32;
	char *new;
	*buffer = malloc(buffersize + 1);

	while (1) {
		int result = read(fd, (*buffer) + bytes, buffersize - bytes);
		if (result > 0) {
			bytes += result;
			if (!strncmp((*buffer) + bytes - 4, "\n##\n", 4)) {
				*(*buffer + bytes - 4) = 0;
				break; /* success, we have the full message now */
			}
			if (bytes == buffersize) {
				buffersize += 1024;
				if (!(new = realloc(*buffer, buffersize + 1)))
					goto fail;
				*buffer = new;
			}
			continue;
		}
		if (result == 0) {
			errno = ECONNRESET;
			goto fail; /* we should never encounter EOF here */
		}
		if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			goto fail;
		/* TODO call select here if we encountered EAGAIN/EWOULDBLOCK/EINTR */
	}
	return 1;
fail:
	free(*buffer);
	*buffer = NULL;
	return 0;
}

/*
 * Write a buffer to a filedescriptor. Keep trying. Blocks (even on
 * SOCK_NONBLOCK) until all of the write went through.
 *
 * TODO use select on EWOULDBLOCK/EAGAIN/EINTR to avoid useless spinning
 */
int write_buffer(int fd, const char *buffer, int length) {
	static const char terminate[] = "\n##\n";
	int done = 0;
	int written = 0;
write:
	while (1) {
		int result = write(fd, buffer + written, length - written);
		if (result > 0)
			written += result;
		if (result < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
			return 0; /* too bad */
		if (written == length) {
			if (done)
				return 1;
			else
				break; /* done */
		}
	}

	buffer = terminate;
	length = 4;
	written = 0;
	done = 1;
	goto write;
}

char *format_buffer_v(const char *head, va_list ap)
{
	char *buffer, *old;
	char *next;
	int keylen;

	dm_asprintf(&buffer, "%s", head);
	if (!buffer) goto fail;

	while ((next = va_arg(ap, char *))) {
		old = buffer;
		if (!strchr(next, '=')) {
			log_error(INTERNAL_ERROR "Bad format string at '%s'", next);
			goto fail;
		}
		keylen = strchr(next, '=') - next;
		if (strstr(next, "%d")) {
			int value = va_arg(ap, int);
			dm_asprintf(&buffer, "%s%.*s= %d\n", buffer, keylen, next, value);
			dm_free(old);
		} else if (strstr(next, "%s")) {
			char *value = va_arg(ap, char *);
			dm_asprintf(&buffer, "%s%.*s= \"%s\"\n", buffer, keylen, next, value);
			dm_free(old);
		} else if (strstr(next, "%b")) {
			char *block = va_arg(ap, char *);
			if (!block)
				continue;
			dm_asprintf(&buffer, "%s%.*s%s", buffer, keylen, next, block);
			dm_free(old);
		} else {
			dm_asprintf(&buffer, "%s%s", buffer, next);
			dm_free(old);
		}
		if (!buffer) goto fail;
	}

	return buffer;
fail:
	dm_free(buffer);
	return NULL;
}

char *format_buffer(const char *head, ...)
{
	va_list ap;
	va_start(ap, head);
	char *res = format_buffer_v(head, ap);
	va_end(ap);
	return res;
}

int set_flag(struct dm_config_tree *cft, struct dm_config_node *parent,
	     const char *field, const char *flag, int want)
{
	struct dm_config_value *value = NULL, *pred = NULL;
	struct dm_config_node *node = dm_config_find_node(parent->child, field);
	struct dm_config_value *new;

	if (node)
		value = node->v;

	while (value && value->type != DM_CFG_EMPTY_ARRAY && strcmp(value->v.str, flag)) {
		pred = value;
		value = value->next;
	}

	if (value && want)
		return 1;

	if (!value && !want)
		return 1;

	if (value && !want) {
		if (pred) {
			pred->next = value->next;
		} else if (value == node->v && value->next) {
			node->v = value->next;
		} else {
			node->v->type = DM_CFG_EMPTY_ARRAY;
		}
	}

	if (!value && want) {
		if (!node) {
			if (!(node = dm_config_create_node(cft, field)))
				return 0;
			node->sib = parent->child;
			if (!(node->v = dm_config_create_value(cft)))
				return 0;
			node->v->type = DM_CFG_EMPTY_ARRAY;
			node->parent = parent;
			parent->child = node;
		}
		if (!(new = dm_config_create_value(cft))) {
			/* FIXME error reporting */
			return 0;
		}
		new->type = DM_CFG_STRING;
		new->v.str = flag;
		new->next = node->v;
		node->v = new;
	}

	return 1;
}

static void chain_node(struct dm_config_node *cn,
		       struct dm_config_node *parent,
		       struct dm_config_node *pre_sib)
{
	cn->parent = parent;
	cn->sib = NULL;

	if (parent && parent->child && !pre_sib) { /* find the last one */
		pre_sib = parent->child;
		while (pre_sib && pre_sib->sib)
			pre_sib = pre_sib->sib;
	}

	if (parent && !parent->child)
		parent->child = cn;
	if (pre_sib)
		pre_sib->sib = cn;

}

struct dm_config_node *make_config_node(struct dm_config_tree *cft,
					const char *key,
					struct dm_config_node *parent,
					struct dm_config_node *pre_sib)
{
	struct dm_config_node *cn;

	if (!(cn = dm_config_create_node(cft, key)))
		return NULL;

	cn->v = NULL;
	cn->child = NULL;

	chain_node(cn, parent, pre_sib);

	return cn;
}

struct dm_config_node *make_text_node(struct dm_config_tree *cft,
				      const char *key,
				      const char *value,
				      struct dm_config_node *parent,
				      struct dm_config_node *pre_sib)
{
	struct dm_config_node *cn;

	if (!(cn = make_config_node(cft, key, parent, pre_sib)) ||
	    !(cn->v = dm_config_create_value(cft)))
		return NULL;

	cn->v->type = DM_CFG_STRING;
	cn->v->v.str = value;
	return cn;
}

struct dm_config_node *make_int_node(struct dm_config_tree *cft,
				     const char *key,
				     int64_t value,
				     struct dm_config_node *parent,
				     struct dm_config_node *pre_sib)
{
	struct dm_config_node *cn;

	if (!(cn = make_config_node(cft, key, parent, pre_sib)) ||
	    !(cn->v = dm_config_create_value(cft)))
		return NULL;

	cn->v->type = DM_CFG_INT;
	cn->v->v.i = value;
	return cn;
}

struct dm_config_node *config_make_nodes_v(struct dm_config_tree *cft,
					   struct dm_config_node *parent,
					   struct dm_config_node *pre_sib,
					   va_list ap)
{
	const char *next;
	struct dm_config_node *first = NULL;

	while ((next = va_arg(ap, char *))) {
		struct dm_config_node *cn = NULL;
		const char *fmt = strchr(next, '=');

		if (!fmt) {
			log_error(INTERNAL_ERROR "Bad format string '%s'", fmt);
			return_NULL;
		}
		fmt += 2;

		char *key = dm_pool_strdup(cft->mem, next);
		*strchr(key, '=') = 0;

		if (!strcmp(fmt, "%d")) {
			int64_t value = va_arg(ap, int64_t);
			if (!(cn = make_int_node(cft, key, value, parent, pre_sib)))
				return 0;
		} else if (!strcmp(fmt, "%s")) {
			char *value = va_arg(ap, char *);
			if (!(cn = make_text_node(cft, key, value, parent, pre_sib)))
				return 0;
		} else if (!strcmp(fmt, "%t")) {
			struct dm_config_tree *tree = va_arg(ap, struct dm_config_tree *);
			cn = dm_config_clone_node(cft, tree->root, 1);
			if (!cn)
				return 0;
			cn->key = key;
			chain_node(cn, parent, pre_sib);
		} else {
			log_error(INTERNAL_ERROR "Bad format string '%s'", fmt);
			return_NULL;
		}
		if (!first)
			first = cn;
		if (cn)
			pre_sib = cn;
	}

	return first;
}

struct dm_config_node *config_make_nodes(struct dm_config_tree *cft,
					 struct dm_config_node *parent,
					 struct dm_config_node *pre_sib,
					 ...)
{
	va_list ap;
	va_start(ap, pre_sib);
	struct dm_config_node *res = config_make_nodes_v(cft, parent, pre_sib, ap);
	va_end(ap);
	return res;
}

int buffer_rewrite(char **buf, const char *format, const char *string)
{
	char *old = *buf;
	int r = dm_asprintf(buf, format, *buf, string);

	dm_free(old);

	return (r < 0) ? 0 : 1;
}

int buffer_line(const char *line, void *baton)
{
	char **buffer = baton;

	if (*buffer) {
		if (!buffer_rewrite(buffer, "%s\n%s", line))
			return 0;
	} else if (dm_asprintf(buffer, "%s\n", line) < 0)
		return 0;

	return 1;
}

