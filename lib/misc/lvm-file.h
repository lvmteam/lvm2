/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

/*
 * Create a temporary filename, and opens a descriptor to the file.
 */
int create_temp_name(const char *dir, char *buffer, size_t len, int *fd);

/*
 * NFS-safe rename of a temporary file to a common name, designed
 * to avoid race conditions and not overwrite the destination if
 * it exists.
 */
int lvm_rename(const char *old, const char *new);

/*
 * Return 1 if path exists else return 0
 */
int path_exists(const char *path);
int dir_exists(const char *path);

/*
 * Create directory (but not recursively) if necessary
 * Return 1 if directory exists on return, else 0
 */
int create_dir(const char *dir);
