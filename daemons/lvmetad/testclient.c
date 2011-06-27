#include "lvmetad-client.h"

int main() {
	daemon_handle h = lvmetad_open();
	int i;
	for (i = 0; i < 5; ++i ) {
		daemon_reply reply = daemon_send_simple(h, "hello world", "param = %d", 3, NULL);
		fprintf(stderr, "[C] REPLY: %s, param = %d\n", daemon_reply_str(reply, "request", "NONE"),
			                                       daemon_reply_int(reply, "param", -1));
		daemon_reply_destroy(reply);
	}
	daemon_close(h);
	return 0;
}
