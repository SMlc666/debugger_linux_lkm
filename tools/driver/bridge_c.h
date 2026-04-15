#ifndef LKMDBG_DRIVER_BRIDGE_C_H
#define LKMDBG_DRIVER_BRIDGE_C_H

#include <sys/types.h>

#include "../../include/lkmdbg_ioctl.h"

int open_session_fd(void);
int set_target_ex(int session_fd, pid_t pid, pid_t tid);
int set_target(int session_fd, pid_t pid);
int get_status(int session_fd, struct lkmdbg_status_reply *reply_out);
int reset_session(int session_fd);
int get_event_config(int session_fd,
		     struct lkmdbg_event_config_request *reply_out);

/* Bridge-prefixed aliases for callers that define local helper names. */
int bridge_open_session_fd(void);
int bridge_set_target_ex(int session_fd, pid_t pid, pid_t tid);
int bridge_set_target(int session_fd, pid_t pid);
int bridge_get_status(int session_fd, struct lkmdbg_status_reply *reply_out);
int bridge_reset_session(int session_fd);
int bridge_get_event_config(int session_fd,
			    struct lkmdbg_event_config_request *reply_out);

#endif
