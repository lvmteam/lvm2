/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_LOG_H
#define _LVM_LOG_H

#include <stdio.h>

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
#define log_sys_err(x)  log_debug("system call '%s' failed (%s)", \
                                  x, strerror(errno))

#define stack log_debug( "stack trace" )

/*
 * Macros to use for messages:
 *
 *   log_error - always print to stderr
 *   log_print - always print to stdout
 *   log_verbose - print to stdout if verbose is set (-v)
 *   log_very_verbose - print to stdout if verbose is set twice (-vv)
 *   log_debug - print to stdout if verbose is set three times (-vvv)
 *
 * In addition, messages will be logged to file or syslog if they
 * are more serious than the log level specified with -d.
 */

#define log_error(fmt, args...) log_err(fmt , ## args)
#define log_print(fmt, args...) log_warn(fmt , ## args)
#define log_verbose(fmt, args...) log_notice(fmt , ## args)
#define log_very_verbose(fmt, args...) log_info(fmt , ## args)

#endif

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
