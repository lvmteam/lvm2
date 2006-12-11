
/* DLM constant that clvmd uses as a generic NONBLOCK lock flag */
#define LKF_NOQUEUE 1

extern int get_next_node_csid(void **context, char *csid);
extern void add_down_node(char *csid);
extern int gulm_fd(void);
extern int get_ip_address(char *node, char *addr);
extern void tcp_remove_client(char *csid);
extern int alloc_client(int fd, char *csid, struct local_client **new_client);

void gulm_add_up_node(char *csid);
int gulm_name_from_csid(char *csid, char *name);
