#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../driver/bridge_c.h"
#include "../driver/bridge_memory.h"

struct example_child_info {
	uintptr_t addr;
	pid_t tid;
};

static ssize_t read_full(int fd, void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t nr = read(fd, (char *)buf + done, len - done);
		if (nr < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (nr == 0)
			break;
		done += (size_t)nr;
	}

	return (ssize_t)done;
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t nw = write(fd, (const char *)buf + done, len - done);
		if (nw < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)nw;
	}

	return (ssize_t)done;
}

static int run_child(int info_fd, int cmd_fd, int reply_fd)
{
	static volatile char target_buf[64] = "example-mem-before";
	static const char expected[] = "example-mem-after";
	struct example_child_info info = {
		.addr = (uintptr_t)target_buf,
		.tid = getpid(),
	};
	char cmd;
	int ok;

	if (write_full(info_fd, &info, sizeof(info)) != (ssize_t)sizeof(info))
		return 1;

	for (;;) {
		if (read_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
			return 1;
		if (cmd == 'v') {
			ok = (strcmp((const char *)target_buf, expected) == 0) ? 0 : 1;
			if (write_full(reply_fd, &ok, sizeof(ok)) != (ssize_t)sizeof(ok))
				return 1;
			continue;
		}
		if (cmd == 'q')
			return 0;
		return 1;
	}
}

int main(void)
{
	static const char expected_before[] = "example-mem-before";
	static const char expected_after[] = "example-mem-after";
	int info_pipe[2];
	int cmd_pipe[2];
	int reply_pipe[2];
	struct example_child_info info;
	char readback[sizeof(expected_before)] = { 0 };
	uint32_t bytes_done = 0;
	pid_t child;
	int session_fd = -1;
	char cmd;
	int child_ok = 1;
	int status = 1;

	if (pipe(info_pipe) != 0 || pipe(cmd_pipe) != 0 || pipe(reply_pipe) != 0) {
		fprintf(stderr, "example_mem_rw: pipe failed errno=%d\n", errno);
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_mem_rw: fork failed errno=%d\n", errno);
		return 1;
	}
	if (child == 0) {
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(reply_pipe[0]);
		_exit(run_child(info_pipe[1], cmd_pipe[0], reply_pipe[1]));
	}

	close(info_pipe[1]);
	close(cmd_pipe[0]);
	close(reply_pipe[1]);

	if (read_full(info_pipe[0], &info, sizeof(info)) != (ssize_t)sizeof(info)) {
		fprintf(stderr, "example_mem_rw: failed to read child info\n");
		goto out;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;

	if (read_target_memory(session_fd, info.addr, readback, sizeof(readback),
			       &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(readback) ||
	    memcmp(readback, expected_before, sizeof(readback)) != 0) {
		fprintf(stderr, "example_mem_rw: readback mismatch bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	bytes_done = 0;
	if (write_target_memory(session_fd, info.addr, expected_after,
				sizeof(expected_after), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(expected_after)) {
		fprintf(stderr, "example_mem_rw: write failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	cmd = 'v';
	if (write_full(cmd_pipe[1], &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
		goto out;
	if (read_full(reply_pipe[0], &child_ok, sizeof(child_ok)) !=
	    (ssize_t)sizeof(child_ok))
		goto out;
	if (child_ok != 0) {
		fprintf(stderr, "example_mem_rw: child verification failed\n");
		goto out;
	}

	status = 0;
	printf("example_mem_rw: ok addr=0x%" PRIxPTR "\n", info.addr);

out:
	cmd = 'q';
	(void)write_full(cmd_pipe[1], &cmd, sizeof(cmd));
	if (session_fd >= 0)
		close(session_fd);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	close(reply_pipe[0]);
	waitpid(child, NULL, 0);
	return status;
}

