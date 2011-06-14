#include "metadata-exported.h"
#include "../common/daemon-server.h"

typedef struct {
} lvmetad_state;

static response handler(daemon_state s, client_handle h, request r)
{
	response res;
	fprintf(stderr, "handling client request: %s\n", r.buffer);
	res.error = 1;
	res.buffer = strdup("hey hey.\n\n");
	return res;
}

static int setup_post(daemon_state *s)
{
	lvmetad_state *ls = s->private;

	/* if (ls->initial_registrations)
	   _process_initial_registrations(ds->initial_registrations); */

	return 1;
}

static void usage(char *prog, FILE *file)
{
	fprintf(file, "Usage:\n"
		"%s [-V] [-h] [-d] [-d] [-d] [-f]\n\n"
		"   -V       Show version of lvmetad\n"
		"   -h       Show this help information\n"
		"   -d       Log debug messages to syslog (-d, -dd, -ddd)\n"
		"   -R       Replace a running lvmetad instance, loading its data\n"
		"   -f       Don't fork, run in the foreground\n\n", prog);
}

int main(int argc, char *argv[])
{
	signed char opt;
	daemon_state s;
	lvmetad_state ls;
	int _restart = 0;

	s.private = &ls;
	s.setup_post = setup_post;
	s.handler = handler;
	s.socket_path = "/var/run/lvm/lvmetad.socket";
	s.pidfile = "/var/run/lvm/lvmetad.pid";

	while ((opt = getopt(argc, argv, "?fhVdR")) != EOF) {
		switch (opt) {
		case 'h':
			usage(argv[0], stdout);
			exit(0);
		case '?':
			usage(argv[0], stderr);
			exit(0);
		case 'R':
			_restart++;
			break;
		case 'f':
			s.foreground = 1;
			break;
		case 'd':
			s.log_level++;
			break;
		case 'V':
			printf("lvmetad version 0\n");
			exit(1);
			break;
		}
	}

	daemon_start(s);
	return 0;
}
