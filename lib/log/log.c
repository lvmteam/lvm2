/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "device.h"
#include "memlock.h"
#include "lvm-string.h"

#include <stdarg.h>
#include <syslog.h>

static FILE *_log_file;
static struct device _log_dev;
static struct str_list _log_dev_alias;

static int _verbose_level = VERBOSE_BASE_LEVEL;
static int _test = 0;
static int _partial = 0;
static int _pvmove = 0;
static int _debug_level = 0;
static int _syslog = 0;
static int _log_to_file = 0;
static int _log_direct = 0;
static int _log_while_suspended = 0;
static int _indent = 1;
static int _log_cmd_name = 0;
static int _log_suppress = 0;
static int _ignorelockingfailure = 0;
static char _cmd_name[30] = "";
static char _msg_prefix[30] = "  ";
static int _already_logging = 0;

static lvm2_log_fn_t _lvm2_log_fn = NULL;

void init_log_fn(lvm2_log_fn_t log_fn)
{
	if (log_fn)
		_lvm2_log_fn = log_fn;
	else
		_lvm2_log_fn = NULL;
}

void init_log_file(const char *log_file, int append)
{
	const char *open_mode = append ? "a" : "w";

	if (!(_log_file = fopen(log_file, open_mode))) {
		log_sys_error("fopen", log_file);
		return;
	}

	_log_to_file = 1;
}

void init_log_direct(const char *log_file, int append)
{
	int open_flags = append ? 0 : O_TRUNC;

	dev_create_file(log_file, &_log_dev, &_log_dev_alias);
	if (!dev_open_flags(&_log_dev, O_RDWR | O_CREAT | open_flags, 1, 0))
		return;

	_log_direct = 1;
}

void init_log_while_suspended(int log_while_suspended)
{
	_log_while_suspended = log_while_suspended;
}

void init_syslog(int facility)
{
	openlog("lvm", LOG_PID, facility);
	_syslog = 1;
}

void log_suppress(int suppress)
{
	_log_suppress = suppress;
}

void release_log_memory(void)
{
	if (!_log_direct)
		return;

	dbg_free((char *) _log_dev_alias.str);
	_log_dev_alias.str = "activate_log file";
}

void fin_log(void)
{
	if (_log_direct) {
		dev_close(&_log_dev);
		_log_direct = 0;
	}

	if (_log_to_file) {
		fclose(_log_file);
		_log_to_file = 0;
	}
}

void fin_syslog()
{
	if (_syslog)
		closelog();
	_syslog = 0;
}

void init_verbose(int level)
{
	_verbose_level = level;
}

void init_test(int level)
{
	if (!_test && level)
		log_print("Test mode: Metadata will NOT be updated.");
	_test = level;
}

void init_partial(int level)
{
	_partial = level;
}

void init_pvmove(int level)
{
	_pvmove = level;
}

void init_ignorelockingfailure(int level)
{
	_ignorelockingfailure = level;
}

void init_cmd_name(int status)
{
	_log_cmd_name = status;
}

void set_cmd_name(const char *cmd)
{
	if (!_log_cmd_name)
		return;
	strncpy(_cmd_name, cmd, sizeof(_cmd_name));
	_cmd_name[sizeof(_cmd_name) - 1] = '\0';
}

void init_msg_prefix(const char *prefix)
{
	strncpy(_msg_prefix, prefix, sizeof(_msg_prefix));
	_msg_prefix[sizeof(_msg_prefix) - 1] = '\0';
}

void init_indent(int indent)
{
	_indent = indent;
}

int test_mode()
{
	return _test;
}

int partial_mode()
{
	return _partial;
}

int pvmove_mode()
{
	return _pvmove;
}

int ignorelockingfailure()
{
	return _ignorelockingfailure;
}

void init_debug(int level)
{
	_debug_level = level;
}

int debug_level()
{
	return _debug_level;
}

void print_log(int level, const char *file, int line, const char *format, ...)
{
	va_list ap;
	char buf[1024], buf2[4096];
	int bufused, n;
	const char *message;
	const char *trformat;		/* Translated format string */

	trformat = _(format);

	if (_lvm2_log_fn) {
		va_start(ap, format);
		n = vsnprintf(buf2, sizeof(buf2) - 1, trformat, ap);
		va_end(ap);

		if (n < 0) {
			fprintf(stderr, _("vsnprintf failed: skipping external "
					"logging function"));
			goto log_it;
		}

		buf2[sizeof(buf2) - 1] = '\0';
		message = &buf2[0];

		_lvm2_log_fn(level, file, line, message);

		return;
	}

      log_it:
	if (!_log_suppress) {
		va_start(ap, format);
		switch (level) {
		case _LOG_DEBUG:
			if (!strcmp("<backtrace>", format))
				break;
			if (_verbose_level >= _LOG_DEBUG) {
				printf("%s%s", _cmd_name, _msg_prefix);
				if (_indent)
					printf("      ");
				vprintf(trformat, ap);
				putchar('\n');
			}
			break;

		case _LOG_INFO:
			if (_verbose_level >= _LOG_INFO) {
				printf("%s%s", _cmd_name, _msg_prefix);
				if (_indent)
					printf("    ");
				vprintf(trformat, ap);
				putchar('\n');
			}
			break;
		case _LOG_NOTICE:
			if (_verbose_level >= _LOG_NOTICE) {
				printf("%s%s", _cmd_name, _msg_prefix);
				if (_indent)
					printf("  ");
				vprintf(trformat, ap);
				putchar('\n');
			}
			break;
		case _LOG_WARN:
			if (_verbose_level >= _LOG_WARN) {
				printf("%s%s", _cmd_name, _msg_prefix);
				vprintf(trformat, ap);
				putchar('\n');
			}
			break;
		case _LOG_ERR:
			if (_verbose_level >= _LOG_ERR) {
				fprintf(stderr, "%s%s", _cmd_name, _msg_prefix);
				vfprintf(stderr, trformat, ap);
				fputc('\n', stderr);
			}
			break;
		case _LOG_FATAL:
		default:
			if (_verbose_level >= _LOG_FATAL) {
				fprintf(stderr, "%s%s", _cmd_name, _msg_prefix);
				vfprintf(stderr, trformat, ap);
				fputc('\n', stderr);
			}
			break;
		}
		va_end(ap);
	}

	if (level > _debug_level)
		return;

	if (_log_to_file && (_log_while_suspended || !memlock())) {
		fprintf(_log_file, "%s:%d %s%s", file, line, _cmd_name,
			_msg_prefix);

		va_start(ap, format);
		vfprintf(_log_file, trformat, ap);
		va_end(ap);

		fprintf(_log_file, "\n");
		fflush(_log_file);
	}

	if (_syslog && (_log_while_suspended || !memlock())) {
		va_start(ap, format);
		vsyslog(level, trformat, ap);
		va_end(ap);
	}

	/* FIXME This code is unfinished - pre-extend & condense. */
	if (!_already_logging && _log_direct && memlock()) {
		_already_logging = 1;
		memset(&buf, ' ', sizeof(buf));
		bufused = 0;
		if ((n = lvm_snprintf(buf, sizeof(buf) - bufused - 1,
				      "%s:%d %s%s", file, line, _cmd_name,
				      _msg_prefix)) == -1)
			goto done;

		bufused += n;

		va_start(ap, format);
		n = vsnprintf(buf + bufused - 1, sizeof(buf) - bufused - 1,
			      trformat, ap);
		va_end(ap);
		bufused += n;

	      done:
		buf[bufused - 1] = '\n';
		buf[bufused] = '\n';
		buf[sizeof(buf) - 1] = '\n';
		/* FIXME real size bufused */
		dev_append(&_log_dev, sizeof(buf), buf);
		_already_logging = 0;
	}
}
