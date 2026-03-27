#ifndef LKMDBG_DRIVER_BRIDGE_EVENTS_H
#define LKMDBG_DRIVER_BRIDGE_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/lkmdbg_ioctl.h"

int drain_session_events(int session_fd);
int read_session_events_timeout(int session_fd,
				struct lkmdbg_event_record *events_out,
				size_t max_events, size_t *events_read_out,
				int timeout_ms);
int read_session_event_timeout(int session_fd,
			       struct lkmdbg_event_record *event_out,
			       int timeout_ms);
int wait_for_session_event_common(int session_fd, uint32_t type, uint32_t code,
				  int timeout_ms,
				  struct lkmdbg_event_record *event_out,
				  bool report_timeout);
int wait_for_session_event(int session_fd, uint32_t type, uint32_t code,
			   int timeout_ms,
			   struct lkmdbg_event_record *event_out);
int wait_for_syscall_event(int session_fd, uint32_t phase, uint32_t syscall_nr,
			   int timeout_ms,
			   struct lkmdbg_event_record *event_out);

#endif
