/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 */

#include "tool.h"

#include "lvmetad-client.h"

daemon_handle h;

static void print_reply(daemon_reply reply)
{
	const char *a = daemon_reply_str(reply, "response", NULL);
	const char *b = daemon_reply_str(reply, "status", NULL);
	const char *c = daemon_reply_str(reply, "reason", NULL);

	printf("response \"%s\" status \"%s\" reason \"%s\"\n",
	       a ? a : "", b ? b : "", c ? c : "");
}

int main(int argc, char **argv)
{
	daemon_reply reply;
	char *cmd;
	char *uuid;
	char *name;
	int val;
	int ver;

	if (argc < 2) {
		printf("lvmeta dump\n");
		printf("lvmeta pv_list\n");
		printf("lvmeta vg_list\n");
		printf("lvmeta vg_lookup_name <name>\n");
		printf("lvmeta vg_lookup_uuid <uuid>\n");
		printf("lvmeta pv_lookup_uuid <uuid>\n");
		printf("lvmeta set_global_invalid 0|1\n");
		printf("lvmeta get_global_invalid\n");
		printf("lvmeta set_vg_version <uuid> <version>\n");
		printf("lvmeta vg_lock_type <uuid>\n");
		return -1;
	}

	cmd = argv[1];

	h = lvmetad_open(NULL);

	if (!strcmp(cmd, "dump")) {
		reply = daemon_send_simple(h, "dump",
					   "token = %s", "skip",
					   NULL);
		printf("%s\n", reply.buffer.mem);

	} else if (!strcmp(cmd, "pv_list")) {
		reply = daemon_send_simple(h, "pv_list",
					   "token = %s", "skip",
					   NULL);
		printf("%s\n", reply.buffer.mem);

	} else if (!strcmp(cmd, "vg_list")) {
		reply = daemon_send_simple(h, "vg_list",
					   "token = %s", "skip",
					   NULL);
		printf("%s\n", reply.buffer.mem);

	} else if (!strcmp(cmd, "set_global_invalid")) {
		if (argc < 3) {
			printf("set_global_invalid 0|1\n");
			return -1;
		}
		val = atoi(argv[2]);

		reply = daemon_send_simple(h, "set_global_info",
					   "global_invalid = %d", val,
					   "token = %s", "skip",
					   NULL);
		print_reply(reply);

	} else if (!strcmp(cmd, "get_global_invalid")) {
		reply = daemon_send_simple(h, "get_global_info",
					   "token = %s", "skip",
					   NULL);
		printf("%s\n", reply.buffer.mem);

	} else if (!strcmp(cmd, "set_vg_version")) {
		if (argc < 4) {
			printf("set_vg_version <uuid> <ver>\n");
			return -1;
		}
		uuid = argv[2];
		ver = atoi(argv[3]);

		reply = daemon_send_simple(h, "set_vg_info",
					   "uuid = %s", uuid,
					   "version = %d", ver,
					   "token = %s", "skip",
					   NULL);
		print_reply(reply);

	} else if (!strcmp(cmd, "vg_lookup_name")) {
		if (argc < 3) {
			printf("vg_lookup_name <name>\n");
			return -1;
		}
		name = argv[2];

		reply = daemon_send_simple(h, "vg_lookup",
					   "name = %s", name,
					   "token = %s", "skip",
					   NULL);
		printf("%s\n", reply.buffer.mem);

	} else if (!strcmp(cmd, "vg_lookup_uuid")) {
		if (argc < 3) {
			printf("vg_lookup_uuid <uuid>\n");
			return -1;
		}
		uuid = argv[2];

		reply = daemon_send_simple(h, "vg_lookup",
					   "uuid = %s", uuid,
					   "token = %s", "skip",
					   NULL);
		printf("%s\n", reply.buffer.mem);

	} else if (!strcmp(cmd, "vg_lock_type")) {
		struct dm_config_node *metadata;
		const char *lock_type;

		if (argc < 3) {
			printf("vg_lock_type <uuid>\n");
			return -1;
		}
		uuid = argv[2];

		reply = daemon_send_simple(h, "vg_lookup",
					   "uuid = %s", uuid,
					   "token = %s", "skip",
					   NULL);
		/* printf("%s\n", reply.buffer.mem); */

		metadata = dm_config_find_node(reply.cft->root, "metadata");
		if (!metadata) {
			printf("no metadata\n");
			goto out;
		}

		lock_type = dm_config_find_str(metadata, "metadata/lock_type", NULL);
		if (!lock_type) {
			printf("no lock_type\n");
			goto out;
		}
		printf("lock_type %s\n", lock_type);

	} else if (!strcmp(cmd, "pv_lookup_uuid")) {
		if (argc < 3) {
			printf("pv_lookup_uuid <uuid>\n");
			return -1;
		}
		uuid = argv[2];

		reply = daemon_send_simple(h, "pv_lookup",
					   "uuid = %s", uuid,
					   "token = %s", "skip",
					   NULL);
		printf("%s\n", reply.buffer.mem);

	} else {
		printf("unknown command\n");
		goto out_close;
	}
out:
	daemon_reply_destroy(reply);
out_close:
	daemon_close(h);
	return 0;
}
