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

