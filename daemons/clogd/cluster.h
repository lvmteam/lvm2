#ifndef __CLUSTER_LOG_CLUSTER_DOT_H__
#define __CLUSTER_LOG_CLUSTER_DOT_H__

#include "list.h"
#include <linux/dm-log-userspace.h>

/*
 * There is other information in addition to what can
 * be found in the dm_ulog_request structure that we
 * need for processing.  'clog_request' is the wrapping
 * structure we use to make the additional fields
 * available.
 */
struct clog_request {
	struct list_head list;

	/*
	 * 'originator' is the machine from which the requests
	 * was made.
	 */
	uint32_t originator;

	/*
	 * 'pit_server' is the "point-in-time" server for the
	 * request.  (I.e.  The machine that was the server at
	 * the time the request was issued - only important during
	 * startup.
	 */
	uint32_t pit_server;

	/*
	 * The request from the kernel that is being processed
	 */
	struct dm_ulog_request u_rq;
};

int init_cluster(void);
void cleanup_cluster(void);
void cluster_debug(void);

int create_cluster_cpg(char *str);
int destroy_cluster_cpg(char *str);

int cluster_send(struct clog_request *rq);

#endif /* __CLUSTER_LOG_CLUSTER_DOT_H__ */
