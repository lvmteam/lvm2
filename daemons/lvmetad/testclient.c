#include "lvmetad-client.h"

int main() {
	daemon_handle h = lvmetad_open();
	daemon_request rq = { .buffer= "hello worldn\n" };
	int i;
	for (i = 0; i < 5; ++i ) {
		daemon_reply reply = daemon_send(h, rq);
		fprintf(stderr, "daemon says: %s\n", reply.buffer);
	}
	daemon_close(h);
	return 0;
}
