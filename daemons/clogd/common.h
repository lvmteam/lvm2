#ifndef __CLUSTER_LOG_COMMON_DOT_H__
#define __CLUSTER_LOG_COMMON_DOT_H__

/*
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
*/

#define EXIT_LOCKFILE              2

#define EXIT_KERNEL_SOCKET         3 /* Failed netlink socket create */
#define EXIT_KERNEL_BIND           4
#define EXIT_KERNEL_SETSOCKOPT     5

#define EXIT_CLUSTER_CKPT_INIT     6 /* Failed to init checkpoint */

#define EXIT_QUEUE_NOMEM           7


#define DM_ULOG_REQUEST_SIZE 1024

#endif /* __CLUSTER_LOG_COMMON_DOT_H__ */
