#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/lkmdbg_ioctl.h"
#include "driver/common.hpp"
#include "driver/bridge_c.h"

#define fprintf lkmdbg_fprintf

static void append_flag_name(char *buf, size_t buf_size, const char *name)
{
	size_t len;

	if (!buf_size)
		return;

	len = strlen(buf);
	if (len >= buf_size - 1)
		return;

	if (len) {
		snprintf(buf + len, buf_size - len, ",%s", name);
		return;
	}

	snprintf(buf, buf_size, "%s", name);
}

static const char *describe_stealth_flags(uint32_t flags, char *buf,
					  size_t buf_size)
{
	if (!buf_size)
		return "";

	buf[0] = '\0';
	if (flags & LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE)
		append_flag_name(buf, buf_size, "debugfs");
	if (flags & LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN)
		append_flag_name(buf, buf_size, "modulehide");
	if (flags & LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN)
		append_flag_name(buf, buf_size, "sysfshide");
	if (flags & LKMDBG_STEALTH_FLAG_OWNER_PROC_HIDDEN)
		append_flag_name(buf, buf_size, "ownerprochide");
	if (!buf[0])
		snprintf(buf, buf_size, "none");
	return buf;
}

static void print_status(const struct lkmdbg_status_reply *reply)
{
	char stealth_flags_buf[48];
	char stealth_supported_buf[48];

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
	printf("stealth_flags=0x%x(%s)\n", reply->stealth_flags,
	       describe_stealth_flags(reply->stealth_flags, stealth_flags_buf,
				      sizeof(stealth_flags_buf)));
	printf("stealth_supported_flags=0x%x(%s)\n",
	       reply->stealth_supported_flags,
	       describe_stealth_flags(reply->stealth_supported_flags,
				      stealth_supported_buf,
				      sizeof(stealth_supported_buf)));
}

static void print_event_config(const struct lkmdbg_event_config_request *cfg)
{
	printf("event_config.version=%u\n", cfg->version);
	printf("event_config.size=%u\n", cfg->size);
	printf("event_config.flags=0x%x\n", cfg->flags);
	printf("event_config.mask[0]=0x%016" PRIx64 "\n",
	       (uint64_t)cfg->mask_words[0]);
	printf("event_config.mask[1]=0x%016" PRIx64 "\n",
	       (uint64_t)cfg->mask_words[1]);
	printf("event_config.supported[0]=0x%016" PRIx64 "\n",
	       (uint64_t)cfg->supported_mask_words[0]);
	printf("event_config.supported[1]=0x%016" PRIx64 "\n",
	       (uint64_t)cfg->supported_mask_words[1]);
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
	struct lkmdbg_status_reply reply = { 0 };
	struct lkmdbg_event_config_request event_cfg = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(event_cfg),
	};
	struct lkmdbg_event_record event = { 0 };
	struct pollfd pfd = { 0 };
	int session_fd;
	ssize_t nread;

	session_fd = open_session_fd();
	if (session_fd < 0) {
		return 1;
	}

	printf("session_fd=%d\n", session_fd);

	if (get_status(session_fd, &reply) < 0) {
		close(session_fd);
		return 1;
	}

	print_status(&reply);

	if (ioctl(session_fd, LKMDBG_IOC_GET_EVENT_CONFIG, &event_cfg) < 0) {
		fprintf(stderr, "GET_EVENT_CONFIG failed: %s\n",
			strerror(errno));
		close(session_fd);
		return 1;
	}

	print_event_config(&event_cfg);

	pfd.fd = session_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 1000) < 0) {
		fprintf(stderr, "poll failed: %s\n", strerror(errno));
		close(session_fd);
		return 1;
	}

	if (!(pfd.revents & POLLIN)) {
		fprintf(stderr, "poll timed out waiting for session event\n");
		close(session_fd);
		return 1;
	}

	nread = read(session_fd, &event, sizeof(event));
	if (nread < 0) {
		fprintf(stderr, "read event failed: %s\n", strerror(errno));
		close(session_fd);
		return 1;
	}
	if ((size_t)nread != sizeof(event)) {
		fprintf(stderr, "short read: %zd\n", nread);
		close(session_fd);
		return 1;
	}

	print_event(&event);

	close(session_fd);
	return 0;
}
