/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_LOG_H
#define _LVM_LOG_H

/*
 * printf()-style macros to use for messages:
 *
 *   log_error   - always print to stderr.
 *   log_print   - always print to stdout.  Use this instead of printf.
 *   log_verbose - print to stdout if verbose is set (-v)
 *   log_very_verbose - print to stdout if verbose is set twice (-vv)
 *   log_debug   - print to stdout if verbose is set three times (-vvv)
 *                (suppressed if single-character string such as with 'stack')
 *
 * In addition, messages will be logged to file or syslog if they
 * are more serious than the log level specified with the log/debug_level
 * parameter in the configuration file.  These messages get the file
 * and line number prepended.  'stack' (without arguments) can be used 
 * to log this information at debug level.
 *
 * log_sys_error and log_sys_very_verbose are for errors from system calls
 * e.g. log_sys_error("stat", filename);
 *      /dev/fd/7: stat failed: No such file or directory
 *
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define _LOG_DEBUG 7
#define _LOG_INFO 6
#define _LOG_NOTICE 5
#define _LOG_WARN 4
#define _LOG_ERR 3
#define _LOG_FATAL 2

void init_log(FILE *fp);
void fin_log(void);

void init_syslog(int facility);
void fin_syslog(void);

void init_verbose(int level);
void init_test(int level);
void init_debug(int level);

int test_mode(void);
int debug_level(void);

void print_log(int level, const char *file, int line, const char *format, ...)
     __attribute__ (( format (printf, 4, 5) ));

#define plog(l, x...) print_log(l, __FILE__, __LINE__ , ## x)

#define log_debug(x...) plog(_LOG_DEBUG, x)
#define log_info(x...) plog(_LOG_INFO, x)
#define log_notice(x...) plog(_LOG_NOTICE, x)
#define log_warn(x...) plog(_LOG_WARN, x)
#define log_err(x...) plog(_LOG_ERR, x)
#define log_fatal(x...) plog(_LOG_FATAL, x)

#define stack log_debug("stack")

#define log_error(fmt, args...) log_err(fmt , ## args)
#define log_print(fmt, args...) log_warn(fmt , ## args)
#define log_verbose(fmt, args...) log_notice(fmt , ## args)
#define log_very_verbose(fmt, args...) log_info(fmt , ## args)

/* Two System call equivalents */
#define log_sys_error(x, y) \
		log_err("%s: %s failed: %s", y, x, strerror(errno))
#define log_sys_very_verbose(x, y) \
		log_info("%s: %s failed: %s", y, x, strerror(errno))

#endif

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
