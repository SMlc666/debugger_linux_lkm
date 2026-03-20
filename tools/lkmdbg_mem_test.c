#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "../lkmdbg_ioctl.h"

#define TARGET_PATH "/proc/version"

static int open_session_fd(void)
{
	struct lkmdbg_open_session_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};
	int proc_fd;
	int session_fd;

	proc_fd = open(TARGET_PATH, O_RDONLY | O_CLOEXEC);
	if (proc_fd < 0) {
		fprintf(stderr, "open(%s) failed: %s\n", TARGET_PATH,
			strerror(errno));
		return -1;
	}

	session_fd = ioctl(proc_fd, LKMDBG_IOC_OPEN_SESSION, &req);
	if (session_fd < 0) {
		fprintf(stderr, "OPEN_SESSION failed: %s\n", strerror(errno));
		close(proc_fd);
		return -1;
	}

	close(proc_fd);
	return session_fd;
}

static int set_target(int session_fd, pid_t pid)
{
	struct lkmdbg_target_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tgid = pid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_TARGET, &req) < 0) {
		fprintf(stderr, "SET_TARGET failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int read_target_memory(int session_fd, uintptr_t remote_addr, void *buf,
			      size_t len)
{
	struct lkmdbg_mem_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
	};

	if (ioctl(session_fd, LKMDBG_IOC_READ_MEM, &req) < 0) {
		fprintf(stderr, "READ_MEM failed: %s\n", strerror(errno));
		return -1;
	}

	printf("READ_MEM bytes_done=%u\n", req.bytes_done);
	return 0;
}

static int write_target_memory(int session_fd, uintptr_t remote_addr,
			       const void *buf, size_t len)
{
	struct lkmdbg_mem_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
	};

	if (ioctl(session_fd, LKMDBG_IOC_WRITE_MEM, &req) < 0) {
		fprintf(stderr, "WRITE_MEM failed: %s\n", strerror(errno));
		return -1;
	}

	printf("WRITE_MEM bytes_done=%u\n", req.bytes_done);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s read <pid> <remote_addr_hex> <length>\n"
		"  %s write <pid> <remote_addr_hex> <ascii_data>\n",
		prog, prog);
}

int main(int argc, char **argv)
{
	int session_fd;
	pid_t pid;
	uintptr_t remote_addr;
	char *endp;

	if (argc < 5) {
		usage(argv[0]);
		return 1;
	}

	pid = (pid_t)strtol(argv[2], &endp, 10);
	if (*endp != '\0' || pid <= 0) {
		fprintf(stderr, "invalid pid: %s\n", argv[2]);
		return 1;
	}

	remote_addr = (uintptr_t)strtoull(argv[3], &endp, 16);
	if (*endp != '\0') {
		fprintf(stderr, "invalid remote address: %s\n", argv[3]);
		return 1;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		return 1;

	if (set_target(session_fd, pid) < 0) {
		close(session_fd);
		return 1;
	}

	if (strcmp(argv[1], "read") == 0) {
		size_t len;
		unsigned char *buf;
		size_t i;

		len = (size_t)strtoul(argv[4], &endp, 0);
		if (*endp != '\0' || len == 0) {
			fprintf(stderr, "invalid length: %s\n", argv[4]);
			close(session_fd);
			return 1;
		}

		buf = calloc(1, len);
		if (!buf) {
			fprintf(stderr, "calloc failed\n");
			close(session_fd);
			return 1;
		}

		if (read_target_memory(session_fd, remote_addr, buf, len) < 0) {
			free(buf);
			close(session_fd);
			return 1;
		}

		for (i = 0; i < len; i++)
			printf("%02x", buf[i]);
		printf("\n");

		free(buf);
	} else if (strcmp(argv[1], "write") == 0) {
		const char *data = argv[4];
		size_t len = strlen(data);

		if (write_target_memory(session_fd, remote_addr, data, len) < 0) {
			close(session_fd);
			return 1;
		}
	} else {
		usage(argv[0]);
		close(session_fd);
		return 1;
	}

	close(session_fd);
	return 0;
}
