/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_CONFIG_H
#define _LVM_CONFIG_H


enum {
        CFG_STRING,
        CFG_FLOAT,
        CFG_INT,
};

struct config_value {
        int type;
        union {
                int i;
                float r;
                char *str;
        } v;
        struct config_value *next; /* for arrays */
};

struct config_node {
        char *key;
        struct config_node *sib, *child;
        struct config_value *v;
};

struct config_file {
        struct config_node *root;
};

struct config_file *create_config_file(void);
void destroy_config_file(struct config_file *cf);

int read_config(struct config_file *cf, const char *file);
int write_config(struct config_file *cf, const char *file);

struct config_node *find_config_node(struct config_node *cn,
				     const char *path, char seperator);

const char *find_config_str(struct config_node *cn,
			    const char *path, char sep, const char *fail);

int find_config_int(struct config_node *cn, const char *path,
		    char sep, int fail);

float find_config_float(struct config_node *cn, const char *path,
			char sep, float fail);

#endif

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
