#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lkmdbg/lkmdbg.h"

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
	struct lkmdbg_session *session = NULL;
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

	if (lkmdbg_session_open(&session) < 0)
		goto out;
	if (lkmdbg_session_set_target(session, child, 0) < 0)
		goto out;
	if (lkmdbg_threads_query(
		    session, 0, entries,
		    (uint32_t)(sizeof(entries) / sizeof(entries[0])), &reply) < 0) {
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
	lkmdbg_session_close(session);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
