/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "uuid.h"
#include "log.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static unsigned char _c[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

int id_create(struct id *id)
{
	int random, i, len = sizeof(id->uuid);

	memset(id->uuid, 0, len);
	if ((random = open("/dev/urandom", O_RDONLY)) < 0) {
		log_sys_error("open", "id_create");
		return 0;
	}

	if (read(random, id->uuid, len) != len) {
		log_sys_error("read", "id_create");
		return 0;
	}
	close(random);

	for (i = 0; i < len; i++)
		id->uuid[i] = _c[id->uuid[i] % (sizeof(_c) - 1)];

	return 1;
}

int id_valid(struct id *id)
{
	log_err("Joe hasn't written id_valid yet");
	return 1;
}

int id_cmp(struct id *lhs, struct id *rhs)
{
	return memcmp(lhs->uuid, rhs->uuid, sizeof(lhs->uuid));
}
