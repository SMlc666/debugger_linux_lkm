#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_memory.h"

struct example_child_info {
	uintptr_t addr;
	uint8_t bytes[64];
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
			return -1;
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

static int run_child(int info_fd, int cmd_fd)
{
	static volatile uint8_t target_buf[64];
	struct example_child_info info;
	size_t i;
	char cmd;

	memset(&info, 0, sizeof(info));
	for (i = 0; i < sizeof(target_buf); i++)
		target_buf[i] = (uint8_t)(0xA0U + i);
	info.addr = (uintptr_t)target_buf;
	memcpy(info.bytes, (const void *)target_buf, sizeof(info.bytes));
	if (write_full(info_fd, &info, sizeof(info)) != (ssize_t)sizeof(info))
		return 1;

	for (;;) {
		if (read_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
			return 1;
		if (cmd == 'q')
			return 0;
	}
}

int main(void)
{
	int info_pipe[2];
	int cmd_pipe[2];
	struct example_child_info info;
	struct lkmdbg_phys_op op;
	struct lkmdbg_phys_request phys_req;
	uint8_t phys_data[sizeof(info.bytes)];
	uint64_t resolved_pa = 0;
	pid_t child;
	int session_fd = -1;
	char cmd = 'q';
	int status = 1;

	memset(&info, 0, sizeof(info));
	memset(&op, 0, sizeof(op));
	memset(&phys_req, 0, sizeof(phys_req));
	memset(phys_data, 0, sizeof(phys_data));

	if (pipe(info_pipe) != 0 || pipe(cmd_pipe) != 0) {
		fprintf(stderr, "example_phys_translate_read: pipe failed errno=%d\n",
			errno);
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr,
			"example_phys_translate_read: fork failed errno=%d\n",
			errno);
		return 1;
	}
	if (child == 0) {
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		_exit(run_child(info_pipe[1], cmd_pipe[0]));
	}

	close(info_pipe[1]);
	close(cmd_pipe[0]);

	if (read_full(info_pipe[0], &info, sizeof(info)) != (ssize_t)sizeof(info)) {
		fprintf(stderr,
			"example_phys_translate_read: child info read failed\n");
		goto out;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;

	op.phys_addr = info.addr;
	op.length = (uint32_t)sizeof(info.bytes);
	op.flags = LKMDBG_PHYS_OP_FLAG_TARGET_VADDR |
		   LKMDBG_PHYS_OP_FLAG_TRANSLATE_ONLY;
	if (xfer_physical_memory(session_fd, &op, 1, 0, &phys_req, 0) < 0) {
		fprintf(stderr,
			"example_phys_translate_read: translate failed errno=%d\n",
			errno);
		goto out;
	}
	if (phys_req.ops_done != 1 || !op.resolved_phys_addr) {
		fprintf(stderr,
			"example_phys_translate_read: bad translate reply ops_done=%u pa=0x%" PRIx64 "\n",
			phys_req.ops_done, (uint64_t)op.resolved_phys_addr);
		goto out;
	}
	resolved_pa = op.resolved_phys_addr;

	memset(&op, 0, sizeof(op));
	op.phys_addr = resolved_pa;
	op.local_addr = (uintptr_t)phys_data;
	op.length = (uint32_t)sizeof(phys_data);
	if (xfer_physical_memory(session_fd, &op, 1, 0, &phys_req, 0) < 0) {
		fprintf(stderr,
			"example_phys_translate_read: phys read failed errno=%d\n",
			errno);
		goto out;
	}
	if (phys_req.ops_done != 1 || op.bytes_done != sizeof(phys_data) ||
	    memcmp(phys_data, info.bytes, sizeof(phys_data)) != 0) {
		fprintf(stderr,
			"example_phys_translate_read: compare failed ops_done=%u bytes_done=%u\n",
			phys_req.ops_done, op.bytes_done);
		goto out;
	}

	status = 0;
	printf("example_phys_translate_read: ok va=0x%" PRIxPTR " pa=0x%" PRIx64
	       " bytes=%zu\n",
	       info.addr, (uint64_t)resolved_pa, sizeof(phys_data));

out:
	(void)write_full(cmd_pipe[1], &cmd, sizeof(cmd));
	if (session_fd >= 0)
		close(session_fd);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
