#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
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
		"  %s selftest\n"
		"  %s read <pid> <remote_addr_hex> <length>\n"
		"  %s write <pid> <remote_addr_hex> <ascii_data>\n",
		prog, prog, prog);
}

static int run_selftest(const char *prog)
{
	int to_parent[2];
	int to_child[2];
	pid_t child;
	int session_fd;
	uintptr_t remote_addr;
	char addr_buf[64] = { 0 };
	char read_buf[32] = { 0 };
	const char *payload = "patched-by-lkmdbg";
	ssize_t nread;
	int status;

	(void)prog;

	if (pipe(to_parent) < 0 || pipe(to_child) < 0) {
		fprintf(stderr, "pipe failed: %s\n", strerror(errno));
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "fork failed: %s\n", strerror(errno));
		return 1;
	}

	if (child == 0) {
		char child_buf[64] = "child-buffer-initial";
		char signal_buf[8] = { 0 };
		ssize_t ignored;

		close(to_parent[0]);
		close(to_child[1]);

		dprintf(to_parent[1], "%p\n", (void *)child_buf);
		ignored = read(to_child[0], signal_buf, sizeof(signal_buf));
		(void)ignored;

		printf("child-buffer-final=%s\n", child_buf);
		fflush(stdout);
		_exit(0);
	}

	close(to_parent[1]);
	close(to_child[0]);

	nread = read(to_parent[0], addr_buf, sizeof(addr_buf) - 1);
	if (nread <= 0) {
		fprintf(stderr, "failed to read child address\n");
		close(to_parent[0]);
		close(to_child[1]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	remote_addr = (uintptr_t)strtoull(addr_buf, NULL, 16);
	printf("selftest child pid=%d remote_addr=0x%llx\n", child,
	       (unsigned long long)remote_addr);

	session_fd = open_session_fd();
	if (session_fd < 0) {
		close(to_parent[0]);
		close(to_child[1]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (set_target(session_fd, child) < 0) {
		close(session_fd);
		close(to_parent[0]);
		close(to_child[1]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (read_target_memory(session_fd, remote_addr, read_buf,
			       sizeof("child-buffer-initial") - 1) < 0) {
		close(session_fd);
		close(to_parent[0]);
		close(to_child[1]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	printf("selftest read-back=%s\n", read_buf);

	if (write_target_memory(session_fd, remote_addr, payload,
				strlen(payload) + 1) < 0) {
		close(session_fd);
		close(to_parent[0]);
		close(to_child[1]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (write(to_child[1], "ok", 2) < 0)
		fprintf(stderr, "failed to signal child: %s\n", strerror(errno));

	close(session_fd);
	close(to_parent[0]);
	close(to_child[1]);

	if (waitpid(child, &status, 0) < 0) {
		fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
		return 1;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "child exited abnormally\n");
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int session_fd;
	pid_t pid;
	uintptr_t remote_addr;
	char *endp;

	if (argc == 2 && strcmp(argv[1], "selftest") == 0)
		return run_selftest(argv[0]);

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
