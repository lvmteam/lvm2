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
static int _partial = 0;
static int _debug_level = 0;
static int _syslog = 0;
static int _indent = 1;
static int _log_cmd_name = 0;
static char _cmd_name[30] = "";
static char _msg_prefix[30] = "  ";

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
	if (_syslog)
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

void init_partial(int level) {
	_partial = level;
}

void init_cmd_name(int status) {
	_log_cmd_name = status;
}

void set_cmd_name(const char *cmd) {
	if (!_log_cmd_name)
		return;
	strncpy(_cmd_name, cmd, sizeof(_cmd_name));
	_cmd_name[sizeof(_cmd_name) - 1] = '\0';
}

void init_msg_prefix(const char *prefix) {
	strncpy(_msg_prefix, prefix, sizeof(_msg_prefix));
	_msg_prefix[sizeof(_msg_prefix) - 1] = '\0';
}

void init_indent(int indent) {
	_indent = indent;
}

int test_mode() {
	return _test;
}

int partial_mode() {
	return _partial;
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
			printf("%s%s", _cmd_name, _msg_prefix);
			if (_indent)
				printf("      ");
			vprintf(format, ap);
			putchar('\n');
		}
		break;

	  case _LOG_INFO:
		if (_verbose_level > 1) {
			printf("%s%s", _cmd_name, _msg_prefix);
			if (_indent)
				printf("    ");
			vprintf(format, ap);
			putchar('\n');
		}
		break;
	  case _LOG_NOTICE:
		if (_verbose_level) {
			printf("%s%s", _cmd_name, _msg_prefix);
			if (_indent)
				printf("  ");
			vprintf(format, ap);
			putchar('\n');
		}
		break;
	  case _LOG_WARN:
		printf("%s%s", _cmd_name, _msg_prefix);
		vprintf(format, ap);
		putchar('\n');
		break;
	  case _LOG_ERR:
		fprintf(stderr, "%s%s", _cmd_name, _msg_prefix);
		vfprintf(stderr, format, ap);
		fputc('\n',stderr);
		break;
	  case _LOG_FATAL:
	  default:
		fprintf(stderr, "%s%s", _cmd_name, _msg_prefix);
		vfprintf(stderr, format, ap);
		fputc('\n',stderr);
		break;
		;
	}
	va_end(ap);

	if (level > _debug_level)
		return;

	if (_log) {
		fprintf(_log, "%s:%d %s%s", file, line, _cmd_name, 
			_msg_prefix);

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

