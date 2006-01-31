#ifndef __DMEVENTD_DOT_H__
#define __DMEVENTD_DOT_H__

#define EXIT_LOCKFILE_INUSE      2
#define EXIT_DESC_CLOSE_FAILURE  3
#define EXIT_OPEN_PID_FAILURE    4
#define EXIT_FIFO_FAILURE        5
#define EXIT_CHDIR_FAILURE       6

void dmeventd(void)
    __attribute((noreturn));

#endif /* __DMEVENTD_DOT_H__ */
