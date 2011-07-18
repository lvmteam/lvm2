#include "lvmetad-client.h"

const char *uuid1 = "abcd-efgh";
const char *uuid2 = "bbcd-efgh";
const char *vgid = "yada-yada";

const char *metadata2 = "{\n"
	"id = \"yada-yada\"\n"
	"seqno = 15\n"
	"status = [\"READ\", \"WRITE\"]\n"
	"flags = []\n"
	"extent_size = 8192\n"
	"physical_volumes {\n"
	"    pv0 {\n"
	"        id = \"abcd-efgh\"\n"
	"    }\n"
	"    pv1 {\n"
	"        id = \"bbcd-efgh\"\n"
	"    }\n"
	"}\n"
	"}\n";

void pv_add(daemon_handle h, const char *uuid, const char *metadata)
{
	daemon_reply reply = daemon_send_simple(h, "pv_add", "uuid = %s", uuid,
						             "metadata = %b", metadata,
						             NULL);
	const char *repl = daemon_reply_str(reply, "response", NULL);
	fprintf(stderr, "[C] REPLY: %s\n", repl);
	if (!strcmp(repl, "failed"))
		fprintf(stderr, "[C] REASON: %s\n", daemon_reply_str(reply, "reason", "unknown"));
	daemon_reply_destroy(reply);
}

int main() {
	daemon_handle h = lvmetad_open();

	pv_add(h, uuid1, NULL);
	pv_add(h, uuid2, metadata2);

	daemon_reply reply = daemon_send_simple(h, "vg_by_uuid", "uuid = %s", vgid, NULL);
	fprintf(stderr, "[C] reply buffer: %s\n", reply.buffer);
	daemon_reply_destroy(reply);

	daemon_close(h);
	return 0;
}
