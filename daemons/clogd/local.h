#ifndef __CLUSTER_LOG_LOCAL_DOT_H__
#define __CLUSTER_LOG_LOCAL_DOT_H__

int init_local(void);
void cleanup_local(void);

int kernel_send(struct dm_ulog_request *rq);

#endif /* __CLUSTER_LOG_LOCAL_DOT_H__ */
