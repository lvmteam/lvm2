/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_CONFIG_H
#define _LVM_CONFIG_H

#include <inttypes.h>
#include <sys/types.h>

enum {
	CFG_STRING,
	CFG_FLOAT,
	CFG_INT,
	CFG_EMPTY_ARRAY
};

struct config_value {
	int type;
	union {
		int i;
		float r;
		char *str;
	} v;
	struct config_value *next;	/* for arrays */
};

struct config_node {
	char *key;
	struct config_node *sib, *child;
	struct config_value *v;
};

struct config_tree {
	struct config_node *root;
};

struct config_tree *create_config_tree(void);
void destroy_config_tree(struct config_tree *cf);

typedef uint32_t (*checksum_fn_t) (uint32_t initial, void *buf, uint32_t size);

int read_config_fd(struct config_tree *cf, int fd, const char *file,
		   off_t offset, uint32_t size, off_t offset2, uint32_t size2,
		   checksum_fn_t checksum_fn, uint32_t checksum);

int read_config_file(struct config_tree *cf, const char *file);
int write_config_file(struct config_tree *cf, const char *file);
int reload_config_file(struct config_tree **cf);

struct config_node *find_config_node(struct config_node *cn,
				     const char *path, char separator);

const char *find_config_str(struct config_node *cn,
			    const char *path, const char sep, const char *fail);

int find_config_int(struct config_node *cn, const char *path,
		    char sep, int fail);

float find_config_float(struct config_node *cn, const char *path,
			char sep, float fail);

/*
 * Understands (0, ~0), (y, n), (yes, no), (on,
 * off), (true, false).
 */
int find_config_bool(struct config_node *cn, const char *path,
		     char sep, int fail);

int get_config_uint32(struct config_node *cn, const char *path,
		      char sep, uint32_t *result);

int get_config_uint64(struct config_node *cn, const char *path,
		      char sep, uint64_t *result);

int get_config_str(struct config_node *cn, const char *path,
		   char sep, char **result);

#endif
