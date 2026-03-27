#ifndef LKMDBG_DRIVER_BRIDGE_C_H
#define LKMDBG_DRIVER_BRIDGE_C_H

#include <sys/types.h>

#include "../../include/lkmdbg_ioctl.h"

int open_session_fd(void);
int set_target_ex(int session_fd, pid_t pid, pid_t tid);
int set_target(int session_fd, pid_t pid);
int get_status(int session_fd, struct lkmdbg_status_reply *reply_out);

#endif
