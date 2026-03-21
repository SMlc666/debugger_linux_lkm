#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../include/lkmdbg_ioctl.h"

#define TARGET_PATH "/proc/version"
#define SELFTEST_THREAD_COUNT 4
#define SELFTEST_THREAD_ITERS 64
#define SELFTEST_SLOT_SIZE 64
#define SELFTEST_LARGE_MAP_LEN (192U * 1024U)
#define SELFTEST_BATCH_OPS 4

struct child_info {
	uintptr_t basic_addr;
	uintptr_t slots_addr;
	uintptr_t nofault_addr;
	uintptr_t large_addr;
	uint32_t page_size;
	uint32_t slot_size;
	uint32_t slot_count;
	uint32_t large_len;
};

struct child_cmd {
	uint32_t op;
	uint32_t reserved;
};

struct worker_ctx {
	int session_fd;
	uintptr_t remote_addr;
	unsigned int thread_index;
	int failed;
	char final_payload[SELFTEST_SLOT_SIZE];
};

enum {
	CHILD_OP_QUERY_NOFAULT = 1,
	CHILD_OP_EXIT = 2,
};

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

static unsigned char pattern_byte(size_t index, unsigned int seed)
{
	return (unsigned char)((index * 131U + seed * 17U + 29U) & 0xffU);
}

static void fill_pattern(unsigned char *buf, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = pattern_byte(i, seed);
}

static int verify_pattern_range(const unsigned char *buf, size_t len,
				unsigned int seed, size_t base_index)
{
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned char expected = pattern_byte(base_index + i, seed);

		if (buf[i] != expected)
			return -1;
	}

	return 0;
}

static int verify_pattern(const unsigned char *buf, size_t len, unsigned int seed)
{
	return verify_pattern_range(buf, len, seed, 0);
}

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

static int xfer_target_memory(int session_fd, struct lkmdbg_mem_op *ops,
			      uint32_t op_count, int write,
			      struct lkmdbg_mem_request *reply_out, int verbose)
{
	struct lkmdbg_mem_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.ops_addr = (uintptr_t)ops,
		.op_count = op_count,
	};
	unsigned long cmd = write ? LKMDBG_IOC_WRITE_MEM : LKMDBG_IOC_READ_MEM;

	if (ioctl(session_fd, cmd, &req) < 0) {
		fprintf(stderr, "%s failed: %s\n",
			write ? "WRITE_MEM" : "READ_MEM", strerror(errno));
		return -1;
	}

	if (verbose)
		printf("%s ops_done=%u bytes_done=%" PRIu64 "\n",
		       write ? "WRITE_MEM" : "READ_MEM", req.ops_done,
		       (uint64_t)req.bytes_done);

	if (reply_out)
		*reply_out = req;

	return 0;
}

static int read_target_memory(int session_fd, uintptr_t remote_addr, void *buf,
			      size_t len, uint32_t *bytes_done_out,
			      int verbose)
{
	struct lkmdbg_mem_op op = {
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
	};
	struct lkmdbg_mem_request req;

	if (xfer_target_memory(session_fd, &op, 1, 0, &req, verbose) < 0)
		return -1;

	if (bytes_done_out)
		*bytes_done_out = op.bytes_done;

	return req.ops_done == 1 || !op.bytes_done ? 0 : -1;
}

static int write_target_memory(int session_fd, uintptr_t remote_addr,
			       const void *buf, size_t len,
			       uint32_t *bytes_done_out, int verbose)
{
	struct lkmdbg_mem_op op = {
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
	};
	struct lkmdbg_mem_request req;

	if (xfer_target_memory(session_fd, &op, 1, 1, &req, verbose) < 0)
		return -1;

	if (bytes_done_out)
		*bytes_done_out = op.bytes_done;

	return req.ops_done == 1 || !op.bytes_done ? 0 : -1;
}

static int read_target_memoryv(int session_fd, struct lkmdbg_mem_op *ops,
			       uint32_t op_count, uint32_t *ops_done_out,
			       uint64_t *bytes_done_out, int verbose)
{
	struct lkmdbg_mem_request req;

	if (xfer_target_memory(session_fd, ops, op_count, 0, &req, verbose) < 0)
		return -1;

	if (ops_done_out)
		*ops_done_out = req.ops_done;
	if (bytes_done_out)
		*bytes_done_out = req.bytes_done;
	return 0;
}

static int write_target_memoryv(int session_fd, struct lkmdbg_mem_op *ops,
				uint32_t op_count, uint32_t *ops_done_out,
				uint64_t *bytes_done_out, int verbose)
{
	struct lkmdbg_mem_request req;

	if (xfer_target_memory(session_fd, ops, op_count, 1, &req, verbose) < 0)
		return -1;

	if (ops_done_out)
		*ops_done_out = req.ops_done;
	if (bytes_done_out)
		*bytes_done_out = req.bytes_done;
	return 0;
}

static int query_mincore_resident(void *addr, size_t len)
{
	unsigned char vec = 0;

	if (mincore(addr, len, &vec) < 0)
		return -1;

	return !!(vec & 1);
}

static int child_selftest_main(int info_fd, int cmd_fd, int resp_fd)
{
	struct child_info info;
	char child_buf[64] = "child-buffer-initial";
	char *slots_map;
	void *nofault_map;
	unsigned char *large_map;
	size_t page_size;
	unsigned int i;

	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (!page_size)
		return 2;

	slots_map = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (slots_map == MAP_FAILED)
		return 2;

	nofault_map = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (nofault_map == MAP_FAILED)
		return 2;

	large_map = mmap(NULL, SELFTEST_LARGE_MAP_LEN, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (large_map == MAP_FAILED)
		return 2;

	for (i = 0; i < SELFTEST_THREAD_COUNT; i++) {
		snprintf(slots_map + (i * SELFTEST_SLOT_SIZE), SELFTEST_SLOT_SIZE,
			 "slot-%u-initial", i);
	}
	fill_pattern(large_map, SELFTEST_LARGE_MAP_LEN, 1);

	memset(nofault_map, 0xA5, page_size);
	if (madvise(nofault_map, page_size, MADV_DONTNEED) < 0)
		return 2;

	memset(&info, 0, sizeof(info));
	info.basic_addr = (uintptr_t)child_buf;
	info.slots_addr = (uintptr_t)slots_map;
	info.nofault_addr = (uintptr_t)nofault_map;
	info.large_addr = (uintptr_t)large_map;
	info.page_size = (uint32_t)page_size;
	info.slot_size = SELFTEST_SLOT_SIZE;
	info.slot_count = SELFTEST_THREAD_COUNT;
	info.large_len = SELFTEST_LARGE_MAP_LEN;

	if (write_full(info_fd, &info, sizeof(info)) != (ssize_t)sizeof(info))
		return 2;

	for (;;) {
		struct child_cmd cmd;
		int resident;

		if (read_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
			return 2;

		switch (cmd.op) {
		case CHILD_OP_QUERY_NOFAULT:
			resident = query_mincore_resident(nofault_map, page_size);
			if (write_full(resp_fd, &resident, sizeof(resident)) !=
			    (ssize_t)sizeof(resident))
				return 2;
			break;
		case CHILD_OP_EXIT:
			return 0;
		default:
			return 2;
		}
	}
}

static int child_query_nofault_residency(int cmd_fd, int resp_fd)
{
	struct child_cmd cmd = {
		.op = CHILD_OP_QUERY_NOFAULT,
	};
	int resident;

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "failed to send child mincore query\n");
		return -1;
	}

	if (read_full(resp_fd, &resident, sizeof(resident)) !=
	    (ssize_t)sizeof(resident)) {
		fprintf(stderr, "failed to read child mincore reply\n");
		return -1;
	}

	if (resident < 0) {
		fprintf(stderr, "child mincore failed\n");
		return -1;
	}

	return resident;
}

static void *worker_thread_main(void *arg)
{
	struct worker_ctx *ctx = arg;
	unsigned int iter;

	for (iter = 0; iter < SELFTEST_THREAD_ITERS; iter++) {
		char payload[SELFTEST_SLOT_SIZE];
		char readback[SELFTEST_SLOT_SIZE];
		size_t len;
		uint32_t bytes_done = 0;

		snprintf(payload, sizeof(payload), "thread-%u-iter-%u",
			 ctx->thread_index, iter);
		len = strlen(payload) + 1;

		if (write_target_memory(ctx->session_fd, ctx->remote_addr, payload,
					len, &bytes_done, 0) < 0 ||
		    bytes_done != len) {
			ctx->failed = 1;
			return NULL;
		}

		memset(readback, 0, sizeof(readback));
		if (read_target_memory(ctx->session_fd, ctx->remote_addr, readback,
				       len, &bytes_done, 0) < 0 ||
		    bytes_done != len || strcmp(readback, payload) != 0) {
			ctx->failed = 1;
			return NULL;
		}

		memcpy(ctx->final_payload, payload, len);
	}

	return NULL;
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
	int info_pipe[2];
	int cmd_pipe[2];
	int resp_pipe[2];
	struct child_info info;
	pid_t child;
	int session_fd;
	char read_buf[32] = { 0 };
	const char *payload = "patched-by-lkmdbg";
	pthread_t threads[SELFTEST_THREAD_COUNT];
	struct worker_ctx workers[SELFTEST_THREAD_COUNT];
	unsigned char *large_buf = NULL;
	unsigned char *batch_a = NULL;
	unsigned char *batch_b = NULL;
	unsigned char *batch_c = NULL;
	unsigned char *batch_d = NULL;
	struct lkmdbg_mem_op batch_ops[SELFTEST_BATCH_OPS];
	static const uint32_t batch_lengths[SELFTEST_BATCH_OPS] = {
		32768, 16384, 65536, 16384,
	};
	uint32_t bytes_done = 0;
	uint32_t ops_done = 0;
	uint64_t batch_bytes_done = 0;
	unsigned int i;
	int status;
	int resident_before;
	int resident_after_read;
	int resident_after_write;

	(void)prog;

	if (pipe(info_pipe) < 0 || pipe(cmd_pipe) < 0 || pipe(resp_pipe) < 0) {
		fprintf(stderr, "pipe failed: %s\n", strerror(errno));
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "fork failed: %s\n", strerror(errno));
		return 1;
	}

	if (child == 0) {
		int ret;

		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		ret = child_selftest_main(info_pipe[1], cmd_pipe[0], resp_pipe[1]);
		_exit(ret);
	}

	close(info_pipe[1]);
	close(cmd_pipe[0]);
	close(resp_pipe[1]);

	if (read_full(info_pipe[0], &info, sizeof(info)) != (ssize_t)sizeof(info)) {
		fprintf(stderr, "failed to read child info\n");
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	printf("selftest child pid=%d basic=0x%llx slots=0x%llx nofault=0x%llx\n",
	       child, (unsigned long long)info.basic_addr,
	       (unsigned long long)info.slots_addr,
	       (unsigned long long)info.nofault_addr);

	session_fd = open_session_fd();
	if (session_fd < 0) {
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (set_target(session_fd, child) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (read_target_memory(session_fd, info.basic_addr, read_buf,
			       sizeof("child-buffer-initial"), &bytes_done,
			       1) < 0 ||
	    bytes_done != sizeof("child-buffer-initial")) {
		fprintf(stderr, "basic READ_MEM returned bytes_done=%u\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	printf("selftest read-back=%s\n", read_buf);

	if (write_target_memory(session_fd, info.basic_addr, payload,
				strlen(payload) + 1, &bytes_done, 1) < 0 ||
	    bytes_done != strlen(payload) + 1) {
		fprintf(stderr, "basic WRITE_MEM returned bytes_done=%u\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	for (i = 0; i < SELFTEST_THREAD_COUNT; i++) {
		memset(&workers[i], 0, sizeof(workers[i]));
		workers[i].session_fd = session_fd;
		workers[i].remote_addr =
			info.slots_addr + (uintptr_t)(i * info.slot_size);
		workers[i].thread_index = i;
		if (pthread_create(&threads[i], NULL, worker_thread_main,
				   &workers[i]) != 0) {
			fprintf(stderr, "pthread_create failed for worker %u\n", i);
			close(session_fd);
			close(info_pipe[0]);
			close(cmd_pipe[1]);
			close(resp_pipe[0]);
			kill(child, SIGKILL);
			waitpid(child, NULL, 0);
			return 1;
		}
	}

	for (i = 0; i < SELFTEST_THREAD_COUNT; i++) {
		char verify_buf[SELFTEST_SLOT_SIZE];

		pthread_join(threads[i], NULL);
		if (workers[i].failed) {
			fprintf(stderr, "worker %u failed\n", i);
			close(session_fd);
			close(info_pipe[0]);
			close(cmd_pipe[1]);
			close(resp_pipe[0]);
			kill(child, SIGKILL);
			waitpid(child, NULL, 0);
			return 1;
		}

		memset(verify_buf, 0, sizeof(verify_buf));
		if (read_target_memory(session_fd, workers[i].remote_addr,
				       verify_buf,
				       strlen(workers[i].final_payload) + 1,
				       &bytes_done, 0) < 0 ||
		    bytes_done != strlen(workers[i].final_payload) + 1 ||
		    strcmp(verify_buf, workers[i].final_payload) != 0) {
			fprintf(stderr, "worker %u final verify failed\n", i);
			close(session_fd);
			close(info_pipe[0]);
			close(cmd_pipe[1]);
			close(resp_pipe[0]);
			kill(child, SIGKILL);
			waitpid(child, NULL, 0);
			return 1;
		}
	}

	printf("selftest concurrent session access ok threads=%u iters=%u\n",
	       SELFTEST_THREAD_COUNT, SELFTEST_THREAD_ITERS);

	resident_before = child_query_nofault_residency(cmd_pipe[1], resp_pipe[0]);
	if (resident_before != 0) {
		fprintf(stderr, "nofault page unexpectedly resident before test\n");
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	memset(read_buf, 0xCC, sizeof(read_buf));
	if (read_target_memory(session_fd, info.nofault_addr, read_buf,
			       sizeof(read_buf), &bytes_done, 1) < 0 ||
	    bytes_done != 0) {
		fprintf(stderr, "nofault READ_MEM bytes_done=%u expected=0\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	resident_after_read = child_query_nofault_residency(cmd_pipe[1],
							    resp_pipe[0]);
	if (resident_after_read != 0) {
		fprintf(stderr, "nofault read faulted target page in\n");
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (write_target_memory(session_fd, info.nofault_addr, payload,
				strlen(payload) + 1, &bytes_done, 1) < 0 ||
	    bytes_done != 0) {
		fprintf(stderr, "nofault WRITE_MEM bytes_done=%u expected=0\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	resident_after_write = child_query_nofault_residency(cmd_pipe[1],
							     resp_pipe[0]);
	if (resident_after_write != 0) {
		fprintf(stderr, "nofault write faulted target page in\n");
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	printf("selftest nofault remote access ok resident_before=%d after_read=%d after_write=%d\n",
	       resident_before, resident_after_read, resident_after_write);

	large_buf = malloc(info.large_len);
	batch_a = malloc(batch_lengths[0]);
	batch_b = malloc(batch_lengths[1]);
	batch_c = malloc(batch_lengths[2]);
	batch_d = malloc(batch_lengths[3]);
	if (!large_buf || !batch_a || !batch_b || !batch_c || !batch_d) {
		fprintf(stderr, "large test allocation failed\n");
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		free(large_buf);
		free(batch_a);
		free(batch_b);
		free(batch_c);
		free(batch_d);
		return 1;
	}

	memset(large_buf, 0, info.large_len);
	if (read_target_memory(session_fd, info.large_addr, large_buf,
			       info.large_len, &bytes_done, 1) < 0 ||
	    bytes_done != info.large_len ||
	    verify_pattern(large_buf, info.large_len, 1) != 0) {
		fprintf(stderr, "large READ_MEM verify failed bytes_done=%u\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		free(large_buf);
		free(batch_a);
		free(batch_b);
		free(batch_c);
		free(batch_d);
		return 1;
	}

	fill_pattern(large_buf, info.large_len, 2);
	if (write_target_memory(session_fd, info.large_addr, large_buf,
				info.large_len, &bytes_done, 1) < 0 ||
	    bytes_done != info.large_len) {
		fprintf(stderr, "large WRITE_MEM failed bytes_done=%u\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		free(large_buf);
		free(batch_a);
		free(batch_b);
		free(batch_c);
		free(batch_d);
		return 1;
	}

	memset(batch_ops, 0, sizeof(batch_ops));
	batch_ops[0].remote_addr = info.large_addr;
	batch_ops[0].local_addr = (uintptr_t)batch_a;
	batch_ops[0].length = batch_lengths[0];
	batch_ops[1].remote_addr = info.large_addr + 49152;
	batch_ops[1].local_addr = (uintptr_t)batch_b;
	batch_ops[1].length = batch_lengths[1];
	batch_ops[2].remote_addr = info.large_addr + 81920;
	batch_ops[2].local_addr = (uintptr_t)batch_c;
	batch_ops[2].length = batch_lengths[2];
	batch_ops[3].remote_addr = info.large_addr + 163840;
	batch_ops[3].local_addr = (uintptr_t)batch_d;
	batch_ops[3].length = batch_lengths[3];

	if (read_target_memoryv(session_fd, batch_ops, SELFTEST_BATCH_OPS,
				&ops_done, &batch_bytes_done, 1) < 0 ||
	    ops_done != SELFTEST_BATCH_OPS ||
	    batch_bytes_done != (uint64_t)batch_lengths[0] +
				    batch_lengths[1] + batch_lengths[2] +
				    batch_lengths[3] ||
	    verify_pattern_range(batch_a, batch_lengths[0], 2, 0) != 0 ||
	    verify_pattern_range(batch_b, batch_lengths[1], 2, 49152) != 0 ||
	    verify_pattern_range(batch_c, batch_lengths[2], 2, 81920) != 0 ||
	    verify_pattern_range(batch_d, batch_lengths[3], 2, 163840) != 0) {
		fprintf(stderr,
			"READ_MEM batch verify failed ops_done=%u bytes_done=%" PRIu64 "\n",
			ops_done, batch_bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		free(large_buf);
		free(batch_a);
		free(batch_b);
		free(batch_c);
		free(batch_d);
		return 1;
	}

	fill_pattern(batch_a, batch_lengths[0], 11);
	fill_pattern(batch_b, batch_lengths[1], 12);
	fill_pattern(batch_c, batch_lengths[2], 13);
	fill_pattern(batch_d, batch_lengths[3], 14);
	if (write_target_memoryv(session_fd, batch_ops, SELFTEST_BATCH_OPS,
				 &ops_done, &batch_bytes_done, 1) < 0 ||
	    ops_done != SELFTEST_BATCH_OPS ||
	    batch_bytes_done != (uint64_t)batch_lengths[0] +
				    batch_lengths[1] + batch_lengths[2] +
				    batch_lengths[3]) {
		fprintf(stderr,
			"WRITE_MEM batch failed ops_done=%u bytes_done=%" PRIu64 "\n",
			ops_done, batch_bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		free(large_buf);
		free(batch_a);
		free(batch_b);
		free(batch_c);
		free(batch_d);
		return 1;
	}

	memset(large_buf, 0, info.large_len);
	if (read_target_memory(session_fd, info.large_addr, large_buf,
			       info.large_len, &bytes_done, 0) < 0 ||
	    bytes_done != info.large_len ||
	    verify_pattern(large_buf, batch_lengths[0], 11) != 0 ||
	    verify_pattern(large_buf + 49152, batch_lengths[1], 12) != 0 ||
	    verify_pattern(large_buf + 81920, batch_lengths[2], 13) != 0 ||
	    verify_pattern(large_buf + 163840, batch_lengths[3], 14) != 0) {
		fprintf(stderr, "large batch writeback verify failed bytes_done=%u\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		free(large_buf);
		free(batch_a);
		free(batch_b);
		free(batch_c);
		free(batch_d);
		return 1;
	}

	printf("selftest large and batched memory access ok len=%u batch_bytes=%" PRIu64 "\n",
	       info.large_len, batch_bytes_done);

	close(session_fd);
	close(info_pipe[0]);

	{
		struct child_cmd exit_cmd = {
			.op = CHILD_OP_EXIT,
		};

		if (write_full(cmd_pipe[1], &exit_cmd, sizeof(exit_cmd)) !=
		    (ssize_t)sizeof(exit_cmd))
			fprintf(stderr, "failed to send child exit command\n");
	}
	close(cmd_pipe[1]);
	close(resp_pipe[0]);

	if (waitpid(child, &status, 0) < 0) {
		fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
		return 1;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "child exited abnormally status=%d\n", status);
		free(large_buf);
		free(batch_a);
		free(batch_b);
		free(batch_c);
		free(batch_d);
		return 1;
	}

	free(large_buf);
	free(batch_a);
	free(batch_b);
	free(batch_c);
	free(batch_d);
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

		if (read_target_memory(session_fd, remote_addr, buf, len, NULL,
				       1) < 0) {
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

		if (write_target_memory(session_fd, remote_addr, data, len, NULL,
					1) < 0) {
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
