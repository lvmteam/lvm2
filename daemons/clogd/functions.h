#ifndef __CLOG_FUNCTIONS_DOT_H__
#define __CLOG_FUNCTIONS_DOT_H__

#include <linux/dm-log-userspace.h>
#include "cluster.h"

#define LOG_RESUMED   1
#define LOG_SUSPENDED 2

int local_resume(struct dm_ulog_request *rq);
int cluster_postsuspend(char *);

int do_request(struct clog_request *rq, int server);
int push_state(const char *uuid, const char *which,
	       char **buf, uint32_t debug_who);
int pull_state(const char *uuid, const char *which, char *buf, int size);

int log_get_state(struct dm_ulog_request *rq);
int log_status(void);
void log_debug(void);

#endif /* __CLOG_FUNCTIONS_DOT_H__ */
