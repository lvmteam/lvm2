#include <netinet/in.h>

#define MAX_CLUSTER_MESSAGE 1600
#define MAX_CSID_LEN sizeof(struct in6_addr)
#define MAX_CLUSTER_MEMBER_NAME_LEN 128

extern int init_comms(unsigned short);
extern char *print_csid(char *);
