/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifdef DEBUG_POOL
#include "pool-debug.c"
#else
#include "pool-fast.c"
#endif

char *pool_strdup(struct pool *p, const char *str)
{
	char *ret = pool_alloc(p, strlen(str) + 1);

	if (ret)
		strcpy(ret, str);

	return ret;
}

char *pool_strndup(struct pool *p, const char *str, size_t n)
{
	char *ret = pool_alloc(p, n + 1);

	if (ret) {
		strncpy(ret, str, n);
		ret[n] = '\0';
	}

	return ret;
}

void *pool_zalloc(struct pool *p, size_t s)
{
	void *ptr = pool_alloc(p, s);

	if (ptr)
		memset(ptr, 0, s);

	return ptr;
}
