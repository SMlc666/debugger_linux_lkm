#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_memory.h"

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
	struct lkmdbg_remote_map_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.map_fd = -1,
		.prot = LKMDBG_REMOTE_MAP_PROT_READ |
			LKMDBG_REMOTE_MAP_PROT_WRITE,
	};
	uint8_t expected[256];
	uint8_t readback[sizeof(expected)];
	uint8_t *mapped = MAP_FAILED;
	uint32_t bytes_done = 0;
	struct lkmdbg_remote_map_handle_request remove_req;
	int info_pipe[2], cmd_pipe[2];
	pid_t child = -1;
	int session_fd = -1;
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
	session_fd = open_session_fd();
	if (session_fd < 0 || set_target(session_fd, child) < 0)
		goto out;
	req.remote_addr = info.addr;
	req.length = info.length;
	if (ioctl(session_fd, LKMDBG_IOC_CREATE_REMOTE_MAP, &req) < 0 ||
	    req.map_fd < 0 || req.mapped_length < sizeof(expected))
		goto out;
	mapped = mmap(NULL, req.mapped_length, PROT_READ | PROT_WRITE,
		      MAP_SHARED, req.map_fd, 0);
	if (mapped == MAP_FAILED)
		goto out;
	if (mprotect(mapped, req.mapped_length, PROT_READ) < 0 ||
	    mprotect(mapped, req.mapped_length, PROT_READ | PROT_WRITE) < 0)
		goto out;

	for (size_t i = 0; i < sizeof(expected); i++) {
		expected[i] = (uint8_t)(i * 13U + 7U);
		if (mapped[i] != expected[i])
			goto out;
	}
	for (size_t i = 0; i < sizeof(expected); i++)
		mapped[i] ^= (uint8_t)(0x5aU + i);
	if (read_target_memory(session_fd, info.addr, readback,
			       sizeof(readback), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(readback) ||
	    memcmp(mapped, readback, sizeof(readback)) != 0)
		goto out;

	printf("example_remote_map_rw: ok addr=0x%" PRIxPTR " len=%" PRIu64 "\n",
	       info.addr, (uint64_t)req.mapped_length);
	status = 0;
out:
	if (session_fd >= 0 && req.map_id) {
		if (bridge_remove_remote_map(session_fd, req.map_id, &remove_req) < 0)
			status = 1;
	}
	if (mapped != MAP_FAILED) {
		munmap(mapped, req.mapped_length);
		mapped = MAP_FAILED;
	}
	if (req.map_fd >= 0)
		close(req.map_fd);
	if (session_fd >= 0)
		close(session_fd);
	(void)write_full(cmd_pipe[1], &cmd, sizeof(cmd));
	if (child > 0)
		waitpid(child, NULL, 0);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	return status;
}
