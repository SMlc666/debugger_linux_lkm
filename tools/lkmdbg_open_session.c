#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/lkmdbg_ioctl.h"

#define TARGET_PATH "/proc/version"

static void print_status(const struct lkmdbg_status_reply *reply)
{
	printf("version=%u\n", reply->version);
	printf("size=%u\n", reply->size);
	printf("hook_requested=%u\n", reply->hook_requested);
	printf("hook_active=%u\n", reply->hook_active);
	printf("owner_tgid=%d\n", reply->owner_tgid);
	printf("target_tgid=%d\n", reply->target_tgid);
	printf("target_tid=%d\n", reply->target_tid);
	printf("event_queue_depth=%u\n", reply->event_queue_depth);
	printf("session_id=%" PRIu64 "\n", (uint64_t)reply->session_id);
	printf("active_sessions=%" PRIu64 "\n",
	       (uint64_t)reply->active_sessions);
	printf("load_jiffies=%" PRIu64 "\n", (uint64_t)reply->load_jiffies);
	printf("status_reads=%" PRIu64 "\n", (uint64_t)reply->status_reads);
	printf("bootstrap_ioctl_calls=%" PRIu64 "\n",
	       (uint64_t)reply->bootstrap_ioctl_calls);
	printf("session_ioctl_calls=%" PRIu64 "\n",
	       (uint64_t)reply->session_ioctl_calls);
	printf("session_opened_total=%" PRIu64 "\n",
	       (uint64_t)reply->session_opened_total);
	printf("open_successes=%" PRIu64 "\n", (uint64_t)reply->open_successes);
	printf("session_event_drops=%" PRIu64 "\n",
	       (uint64_t)reply->session_event_drops);
	printf("total_event_drops=%" PRIu64 "\n",
	       (uint64_t)reply->total_event_drops);
	printf("stop_cookie=%" PRIu64 "\n", (uint64_t)reply->stop_cookie);
	printf("stop_reason=%u\n", reply->stop_reason);
	printf("stop_flags=0x%x\n", reply->stop_flags);
	printf("stop_tgid=%d\n", reply->stop_tgid);
	printf("stop_tid=%d\n", reply->stop_tid);
	printf("stealth_flags=0x%x\n", reply->stealth_flags);
	printf("stealth_supported_flags=0x%x\n", reply->stealth_supported_flags);
}

static void print_event(const struct lkmdbg_event_record *event)
{
	printf("event.version=%u\n", event->version);
	printf("event.type=%u\n", event->type);
	printf("event.size=%u\n", event->size);
	printf("event.code=%u\n", event->code);
	printf("event.session_id=%" PRIu64 "\n", (uint64_t)event->session_id);
	printf("event.seq=%" PRIu64 "\n", (uint64_t)event->seq);
	printf("event.tgid=%d\n", event->tgid);
	printf("event.tid=%d\n", event->tid);
	printf("event.flags=0x%x\n", event->flags);
	printf("event.reserved0=%u\n", event->reserved0);
	printf("event.value0=%" PRIu64 "\n", (uint64_t)event->value0);
	printf("event.value1=%" PRIu64 "\n", (uint64_t)event->value1);
}

int main(void)
{
	struct lkmdbg_open_session_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};
	struct lkmdbg_status_reply reply = { 0 };
	struct lkmdbg_event_record event = { 0 };
	struct pollfd pfd = { 0 };
	int proc_fd;
	int session_fd;
	ssize_t nread;

	proc_fd = open(TARGET_PATH, O_RDONLY | O_CLOEXEC);
	if (proc_fd < 0) {
		fprintf(stderr, "open(%s) failed: %s\n", TARGET_PATH,
			strerror(errno));
		return 1;
	}

	session_fd = ioctl(proc_fd, LKMDBG_IOC_OPEN_SESSION, &req);
	if (session_fd < 0) {
		fprintf(stderr, "OPEN_SESSION failed: %s\n", strerror(errno));
		close(proc_fd);
		return 1;
	}

	printf("session_fd=%d\n", session_fd);

	if (ioctl(session_fd, LKMDBG_IOC_GET_STATUS, &reply) < 0) {
		fprintf(stderr, "GET_STATUS failed: %s\n", strerror(errno));
		close(session_fd);
		close(proc_fd);
		return 1;
	}

	print_status(&reply);

	pfd.fd = session_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 1000) < 0) {
		fprintf(stderr, "poll failed: %s\n", strerror(errno));
		close(session_fd);
		close(proc_fd);
		return 1;
	}

	if (!(pfd.revents & POLLIN)) {
		fprintf(stderr, "poll timed out waiting for session event\n");
		close(session_fd);
		close(proc_fd);
		return 1;
	}

	nread = read(session_fd, &event, sizeof(event));
	if (nread < 0) {
		fprintf(stderr, "read event failed: %s\n", strerror(errno));
		close(session_fd);
		close(proc_fd);
		return 1;
	}
	if ((size_t)nread != sizeof(event)) {
		fprintf(stderr, "short read: %zd\n", nread);
		close(session_fd);
		close(proc_fd);
		return 1;
	}

	print_event(&event);

	close(session_fd);
	close(proc_fd);
	return 0;
}
