#ifndef __LINK_MON_DOT_H__
#define __LINK_MON_DOT_H__

int links_register(int fd, char *name, int (*callback)(void *data), void *data);
int links_unregister(int fd);
int links_monitor(void);
int links_issue_callbacks(void);

#endif /* __LINK_MON_DOT_H__ */
