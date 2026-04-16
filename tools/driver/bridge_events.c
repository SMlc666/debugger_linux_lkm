#include "bridge_events.h"
#include "common.hpp"

#include <errno.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>

#define LKMDBG_EVENT_READ_BATCH 16U

static int64_t monotonic_time_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return -1;

	return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

int drain_session_events(int session_fd)
{
	struct lkmdbg_event_record events[LKMDBG_EVENT_READ_BATCH];

	for (;;) {
		struct pollfd pfd = {
			.fd = session_fd,
			.events = POLLIN,
		};
		ssize_t nread;

		if (poll(&pfd, 1, 0) < 0) {
			lkmdbg_log_errorf("drain poll failed: %s",
					  strerror(errno));
			return -1;
		}
		if (!(pfd.revents & POLLIN))
			return 0;

		nread = read(session_fd, events, sizeof(events));
		if (nread > 0) {
			if (nread % (ssize_t)sizeof(events[0]) != 0) {
				lkmdbg_log_errorf("short event drain read: %zd",
						  nread);
				return -1;
			}
			continue;
		}
		if (nread == 0)
			return 0;
		if (errno == EINTR)
			continue;
		lkmdbg_log_errorf("drain read failed: %s", strerror(errno));
		return -1;
	}
}

int read_session_events_timeout(int session_fd,
				struct lkmdbg_event_record *events_out,
				size_t max_events, size_t *events_read_out,
				int timeout_ms)
{
	struct pollfd pfd = {
		.fd = session_fd,
		.events = POLLIN,
	};
	size_t bytes;
	ssize_t nread;

	if (!events_out || !events_read_out || !max_events)
		return -1;

	*events_read_out = 0;
	if (poll(&pfd, 1, timeout_ms) < 0) {
		lkmdbg_log_errorf("event poll failed: %s", strerror(errno));
		return -1;
	}

	if (!(pfd.revents & POLLIN))
		return 1;

	bytes = max_events * sizeof(*events_out);
	nread = read(session_fd, events_out, bytes);
	if (nread < 0) {
		lkmdbg_log_errorf("event read failed: %s", strerror(errno));
		return -1;
	}
	if (nread == 0) {
		*events_read_out = 0;
		return 1;
	}
	if (nread % (ssize_t)sizeof(*events_out) != 0) {
		lkmdbg_log_errorf("short event read: %zd", nread);
		return -1;
	}

	*events_read_out = (size_t)nread / sizeof(*events_out);
	return 0;
}

int read_session_event_timeout(int session_fd,
			       struct lkmdbg_event_record *event_out,
			       int timeout_ms)
{
	size_t events_read = 0;
	int ret;

	ret = read_session_events_timeout(session_fd, event_out, 1, &events_read,
					  timeout_ms);
	if (ret)
		return ret;
	if (events_read != 1) {
		lkmdbg_log_errorf("unexpected event batch size: %zu",
				  events_read);
		return -1;
	}

	return 0;
}

int wait_for_session_event_common(int session_fd, uint32_t type, uint32_t code,
				  int timeout_ms,
				  struct lkmdbg_event_record *event_out,
				  bool report_timeout)
{
	int64_t start_ms = monotonic_time_ms();
	int64_t deadline_ms;

	if (start_ms < 0) {
		lkmdbg_log_errorf("clock_gettime failed: %s", strerror(errno));
		return -1;
	}
	deadline_ms = start_ms + timeout_ms;

	while (1) {
		struct lkmdbg_event_record event;
		int64_t now_ms = monotonic_time_ms();
		int64_t remaining_ms;
		int slice;
		int ret;

		if (now_ms < 0) {
			lkmdbg_log_errorf("clock_gettime failed: %s", strerror(errno));
			return -1;
		}
		remaining_ms = deadline_ms - now_ms;
		if (remaining_ms <= 0)
			break;
		slice = (int)remaining_ms;
		if (slice > 1000)
			slice = 1000;
		ret = read_session_event_timeout(session_fd, &event, slice);
		if (ret < 0)
			return -1;
		if (ret > 0)
			continue;

		if (event.type != type)
			continue;
		if (code && event.code != code)
			continue;

		if (event_out)
			*event_out = event;
		return 0;
	}

	if (report_timeout)
		lkmdbg_log_errorf("event wait timed out type=%u code=%u", type,
				  code);
	return -ETIMEDOUT;
}

int wait_for_session_event(int session_fd, uint32_t type, uint32_t code,
			   int timeout_ms,
			   struct lkmdbg_event_record *event_out)
{
	return wait_for_session_event_common(session_fd, type, code, timeout_ms,
					     event_out, true);
}

int wait_for_syscall_event(int session_fd, uint32_t phase, uint32_t syscall_nr,
			   int timeout_ms,
			   struct lkmdbg_event_record *event_out)
{
	int64_t start_ms = monotonic_time_ms();
	int64_t deadline_ms;

	if (start_ms < 0) {
		lkmdbg_log_errorf("clock_gettime failed: %s", strerror(errno));
		return -1;
	}
	deadline_ms = start_ms + timeout_ms;

	while (1) {
		struct lkmdbg_event_record event;
		int64_t now_ms = monotonic_time_ms();
		int64_t remaining_ms;
		int slice;
		int ret;

		if (now_ms < 0) {
			lkmdbg_log_errorf("clock_gettime failed: %s", strerror(errno));
			return -1;
		}
		remaining_ms = deadline_ms - now_ms;
		if (remaining_ms <= 0)
			break;
		slice = (int)remaining_ms;
		if (slice > 1000)
			slice = 1000;
		ret = read_session_event_timeout(session_fd, &event, slice);
		if (ret < 0)
			return -1;
		if (ret > 0)
			continue;

		if (event.type != LKMDBG_EVENT_TARGET_SYSCALL)
			continue;
		if (event.flags != phase)
			continue;
		if (event.value0 != syscall_nr)
			continue;

		if (event_out)
			*event_out = event;
		return 0;
	}

	lkmdbg_log_errorf("syscall event wait timed out phase=0x%x nr=%u", phase,
			  syscall_nr);
	return -ETIMEDOUT;
}
