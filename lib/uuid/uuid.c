/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "uuid.h"
#include "log.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

static unsigned char _c[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static int _built_inverse;
static unsigned char _inverse_c[256];

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
		close(random);
		return 0;
	}
	close(random);

	for (i = 0; i < len; i++)
		id->uuid[i] = _c[id->uuid[i] % (sizeof(_c) - 1)];

	return 1;
}

/*
 * The only validity check we have is that
 * the uuid just contains characters from
 * '_c'.  A checksum would have been nice :(
 */
void _build_inverse(void)
{
	char *ptr;

	if (_built_inverse)
		return;

	memset(_inverse_c, 0, sizeof(_inverse_c));

	for (ptr = _c; *ptr; ptr++)
		_inverse_c[(int) *ptr] = (char) 0x1;
}

int id_valid(struct id *id)
{
	int i;

	_build_inverse();

	for (i = 0; i < ID_LEN; i++)
		if (!_inverse_c[id->uuid[i]]) {
			log_err("UUID contains invalid character");
			return 0;
		}

	return 1;
}

int id_equal(struct id *lhs, struct id *rhs)
{
	return !memcmp(lhs->uuid, rhs->uuid, sizeof(lhs->uuid));
}

#define GROUPS (ID_LEN / 4)

int id_write_format(struct id *id, char *buffer, size_t size)
{
	int i, tot;

	static int group_size[] = {6, 4, 4, 4, 4, 4, 6};

	assert(ID_LEN == 32);

	/* split into groups seperated by dashes */
	if (size < (32 + 6 + 1)) {
		log_err("Couldn't write uuid, buffer too small.");
		return 0;
	}

	for (i = 0, tot = 0; i < 7; i++) {
		memcpy(buffer, id->uuid + tot, group_size[i]);
		buffer += group_size[i];
		tot += group_size[i];
		*buffer++ = '-';
	}

	*--buffer = '\0';
	return 1;
}

int id_read_format(struct id *id, char *buffer)
{
	int out = 0;

	/* just strip out any dashes */
	while (*buffer) {

		if (*buffer == '-') {
			buffer++;
			continue;
		}

		if (out >= ID_LEN) {
			log_err("Too many characters to be uuid.");
			return 0;
		}

		id->uuid[out++] = *buffer++;
	}

	if (out != ID_LEN) {
		log_err("Couldn't read uuid, incorrect number of characters.");
		return 0;
	}

	return id_valid(id);
}
