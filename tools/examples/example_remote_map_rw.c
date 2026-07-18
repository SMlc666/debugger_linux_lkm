#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lkmdbg/lkmdbg.h"

struct child_info {
	uintptr_t addr;
	uint32_t length;
};

static int read_full(int fd, void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = read(fd, (char *)buf + done, len - done);
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = write(fd, (const char *)buf + done, len - done);
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

static int child_main(int info_fd, int cmd_fd)
{
	struct child_info info;
	uint8_t *p;
	char cmd;

	info.length = (uint32_t)getpagesize() * 8U;
	p = mmap(NULL, info.length, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return 1;
	info.addr = (uintptr_t)p;
	for (uint32_t i = 0; i < info.length; i++)
		p[i] = (uint8_t)(i * 13U + 7U);
	if (write_full(info_fd, &info, sizeof(info)) < 0)
		return 1;
	if (read_full(cmd_fd, &cmd, sizeof(cmd)) < 0)
		return 1;
	munmap(p, info.length);
	return cmd == 'q' ? 0 : 1;
}

int main(void)
{
	struct child_info info;
	struct lkmdbg_remote_map_options options = {
		.protection = LKMDBG_REMOTE_MAP_PROT_READ |
			LKMDBG_REMOTE_MAP_PROT_WRITE,
	};
	uint8_t expected[256];
	uint8_t readback[sizeof(expected)];
	uint8_t *mapped = NULL;
	struct lkmdbg_transfer_result transfer;
	struct lkmdbg_remote_mapping *mapping = NULL;
	struct lkmdbg_session *session = NULL;
	int info_pipe[2], cmd_pipe[2];
	pid_t child = -1;
	int status = 1;
	char cmd = 'q';

	if (pipe(info_pipe) || pipe(cmd_pipe))
		return 1;
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0) {
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		_exit(child_main(info_pipe[1], cmd_pipe[0]));
	}
	close(info_pipe[1]);
	close(cmd_pipe[0]);

	if (read_full(info_pipe[0], &info, sizeof(info)) < 0 ||
	    !info.addr || info.length < (uint32_t)getpagesize())
		goto out;
	if (lkmdbg_session_open(&session) < 0 ||
	    lkmdbg_session_set_target(session, child, 0) < 0)
		goto out;
	options.remote_address = info.addr;
	options.length = info.length;
	if (lkmdbg_remote_map_create(session, &options, &mapping) < 0 ||
	    lkmdbg_remote_map_length(mapping) < sizeof(expected))
		goto out;
	mapped = lkmdbg_remote_map_data(mapping);
	if (mprotect(mapped, lkmdbg_remote_map_length(mapping), PROT_READ) < 0 ||
	    mprotect(mapped, lkmdbg_remote_map_length(mapping),
		     PROT_READ | PROT_WRITE) < 0)
		goto out;

	for (size_t i = 0; i < sizeof(expected); i++) {
		expected[i] = (uint8_t)(i * 13U + 7U);
		if (mapped[i] != expected[i])
			goto out;
	}
	for (size_t i = 0; i < sizeof(expected); i++)
		mapped[i] ^= (uint8_t)(0x5aU + i);
	if (lkmdbg_memory_read(session, info.addr, readback, sizeof(readback), 0,
			       &transfer) < 0 ||
	    transfer.bytes_done != sizeof(readback) ||
	    memcmp(mapped, readback, sizeof(readback)) != 0)
		goto out;

	printf("example_remote_map_rw: ok addr=0x%" PRIxPTR " len=%" PRIu64 "\n",
	       info.addr, lkmdbg_remote_map_length(mapping));
	status = 0;
out:
	if (mapping && lkmdbg_remote_map_destroy(mapping) < 0)
		status = 1;
	lkmdbg_session_close(session);
	(void)write_full(cmd_pipe[1], &cmd, sizeof(cmd));
	if (child > 0)
		waitpid(child, NULL, 0);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	return status;
}
