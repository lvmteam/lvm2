/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "log.h"
#include <stdarg.h>
#include <syslog.h>

static FILE *_log = 0;

static int _verbose_level = 0;
static int _test = 0;
static int _debug_level = 0;
static int _syslog = 0;

void init_log(FILE *fp) {
	_log = fp;
}

void init_syslog(int facility) {
	openlog("lvm", LOG_PID, facility);
	_syslog = 1;
}

void fin_log() {
	_log = 0;
}

void fin_syslog() {
	closelog();
	_syslog = 0;
}

void init_verbose(int level) {
	_verbose_level = level;
}

void init_test(int level) {
	_test = level;
	if (_test)
		log_print("Test mode. Metadata will NOT be updated.");
}

int test_mode() {
	return _test;
}

void init_debug(int level) {
	_debug_level = level;
}

int debug_level() {
	return _debug_level;
}

void print_log(int level, const char *file, int line, const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	switch(level) {
	  case _LOG_DEBUG:
		if (_verbose_level > 2 && format[1]) {
			printf("        ");
			vprintf(format, ap);
			putchar('\n');
		}
		break;

	  case _LOG_INFO:
		if (_verbose_level > 1) {
			printf("      ");
			vprintf(format, ap);
			putchar('\n');
		}
		break;
	  case _LOG_NOTICE:
		if (_verbose_level) {
			printf("    ");
			vprintf(format, ap);
			putchar('\n');
		}
		break;
	  case _LOG_WARN:
		printf("  ");
		vprintf(format, ap);
		putchar('\n');
		break;
	  case _LOG_ERR:
		fprintf(stderr, "  ");
		vfprintf(stderr, format, ap);
		fputc('\n',stderr);
		break;
	  case _LOG_FATAL:
		vfprintf(stderr, format, ap);
		fputc('\n',stderr);
		break;
	  default:
		;
	}
	va_end(ap);

	if (level > _debug_level)
		return;

	if (_log) {
		fprintf(_log, "%s:%d ", file, line);

		va_start(ap, format);
		vfprintf(_log, format, ap);
		va_end(ap);

		fprintf(_log, "\n");
		fflush(_log);
	}

	if (_syslog) {
        	va_start(ap, format);
		vsyslog(level, format, ap);
		va_end(ap);
	}
}

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */

