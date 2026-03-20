#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "../lkmdbg_ioctl.h"

#define TARGET_PATH "/proc/version"

static void print_status(const struct lkmdbg_status_reply *reply)
{
	printf("version=%u\n", reply->version);
	printf("size=%u\n", reply->size);
	printf("hook_requested=%u\n", reply->hook_requested);
	printf("hook_active=%u\n", reply->hook_active);
	printf("owner_tgid=%d\n", reply->owner_tgid);
	printf("session_id=%" PRIu64 "\n", reply->session_id);
	printf("active_sessions=%" PRIu64 "\n", reply->active_sessions);
	printf("load_jiffies=%" PRIu64 "\n", reply->load_jiffies);
	printf("status_reads=%" PRIu64 "\n", reply->status_reads);
	printf("bootstrap_ioctl_calls=%" PRIu64 "\n",
	       reply->bootstrap_ioctl_calls);
	printf("session_ioctl_calls=%" PRIu64 "\n",
	       reply->session_ioctl_calls);
	printf("session_opened_total=%" PRIu64 "\n",
	       reply->session_opened_total);
	printf("open_successes=%" PRIu64 "\n", reply->open_successes);
}

int main(void)
{
	struct lkmdbg_open_session_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};
	struct lkmdbg_status_reply reply = { 0 };
	int proc_fd;
	int session_fd;

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

	close(session_fd);
	close(proc_fd);
	return 0;
}
