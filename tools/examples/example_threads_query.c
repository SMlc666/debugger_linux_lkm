#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_control.h"

static int run_child(void)
{
	for (;;)
		usleep(20000);
	return 0;
}

int main(void)
{
	struct lkmdbg_thread_entry entries[32];
	struct lkmdbg_thread_query_request reply;
	pid_t child;
	int session_fd = -1;
	uint32_t i;
	int found = 0;
	int status = 1;

	memset(entries, 0, sizeof(entries));
	memset(&reply, 0, sizeof(reply));

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_threads_query: fork failed errno=%d\n", errno);
		return 1;
	}
	if (child == 0)
		_exit(run_child());

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;
	if (query_target_threads(session_fd, 0, entries,
				 (uint32_t)(sizeof(entries) / sizeof(entries[0])),
				 &reply) < 0) {
		goto out;
	}
	if (reply.entries_filled == 0) {
		fprintf(stderr, "example_threads_query: no entries\n");
		goto out;
	}
	for (i = 0; i < reply.entries_filled; i++) {
		if (entries[i].tid == child) {
			found = 1;
			break;
		}
	}
	if (!found) {
		fprintf(stderr,
			"example_threads_query: child tid=%d not found in first batch\n",
			child);
		goto out;
	}

	status = 0;
	printf("example_threads_query: ok child=%d entries=%u\n", child,
	       reply.entries_filled);

out:
	if (session_fd >= 0)
		close(session_fd);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
