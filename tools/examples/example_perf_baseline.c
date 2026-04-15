#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_memory.h"

struct example_child_info {
	uintptr_t map_addr;
	uint32_t page_size;
	uint32_t map_len;
};

static int run_mem_xfer_loops(int session_fd, uintptr_t remote_addr, uint8_t *buf,
			      size_t total_len, size_t chunk_len, int loops,
			      int write, const char *tag)
{
	int loop;

	for (loop = 0; loop < loops; loop++) {
		size_t done = 0;

		while (done < total_len) {
			size_t this_len = total_len - done;
			uint32_t bytes_done = 0;
			int ret;

			if (this_len > chunk_len)
				this_len = chunk_len;

			if (write) {
				ret = write_target_memory(session_fd,
							  remote_addr + done,
							  buf + done, this_len,
							  &bytes_done, 0);
			} else {
				ret = read_target_memory(session_fd,
							 remote_addr + done,
							 buf + done, this_len,
							 &bytes_done, 0);
			}

			if (ret < 0 || bytes_done != this_len) {
				fprintf(stderr,
					"example_perf_baseline: %s failed loop=%d off=%zu req=%zu done=%u\n",
					tag, loop, done, this_len, bytes_done);
				return -1;
			}

			done += this_len;
		}
	}

	return 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
	struct example_child_info info;
	void *map;
	size_t i;
	char cmd = 0;

	memset(&info, 0, sizeof(info));
	info.page_size = (uint32_t)getpagesize();
	/*
	 * Keep shell mapping shape aligned with remote_alloc_rw example.
	 * Large mappings can exercise different page-table forms and make
	 * perf-baseline smoke non-deterministic across kernels.
	 */
	info.map_len = info.page_size * 4U;
	map = mmap(NULL, info.map_len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		return 1;

	for (i = 0; i < info.map_len; i++)
		((uint8_t *)map)[i] = (uint8_t)(0x41U + (i & 0x3fU));

	info.map_addr = (uintptr_t)map;
	if (write_full(info_fd, &info, sizeof(info)) != (ssize_t)sizeof(info))
		return 1;
	if (read_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
		return 1;

	munmap(map, info.map_len);
	return cmd == 'q' ? 0 : 1;
}

int main(void)
{
	enum {
		LOOPS_RW = 16,
		LOOPS_XLATE = 256,
		LOOPS_ALLOC = 16,
		MAX_MEM_OP_LEN = 256 * 1024,
	};
	int info_pipe[2];
	int cmd_pipe[2];
	struct example_child_info info;
	struct lkmdbg_phys_op translate_op;
	struct lkmdbg_phys_request phys_req;
	uint8_t *buf = NULL;
	uint64_t t0;
	uint64_t t1;
	uint64_t ns_read = 0;
	uint64_t ns_write = 0;
	uint64_t ns_xlate = 0;
	uint64_t ns_alloc = 0;
	double read_mb_s;
	double write_mb_s;
	double xlate_kops;
	double alloc_us;
	uintptr_t shell_addr;
	uintptr_t bench_base;
	size_t bench_len;
	size_t chunk_len;
	size_t rw_alloc_len;
	pid_t child;
	int session_fd = -1;
	char cmd = 'q';
	uint64_t rw_alloc_id = 0;
	int i;
	int status = 1;

	memset(&info, 0, sizeof(info));
	memset(&translate_op, 0, sizeof(translate_op));
	memset(&phys_req, 0, sizeof(phys_req));

	if (pipe(info_pipe) != 0 || pipe(cmd_pipe) != 0) {
		fprintf(stderr, "example_perf_baseline: pipe failed errno=%d\n",
			errno);
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_perf_baseline: fork failed errno=%d\n",
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
		fprintf(stderr, "example_perf_baseline: child info read failed\n");
		goto out;
	}
	if (!info.map_addr || info.page_size == 0 ||
	    info.map_len < info.page_size * 4U) {
		fprintf(stderr,
			"example_perf_baseline: invalid child map addr=0x%" PRIxPTR
			" page=%u len=%u\n",
			info.map_addr, info.page_size, info.map_len);
		goto out;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;

	bench_len = info.page_size * 128U;
	if (bench_len > info.map_len / 2U)
		bench_len = info.map_len / 2U;
	/* Keep each mem ioctl op within kernel-side single-op transfer cap. */
	if (bench_len > MAX_MEM_OP_LEN)
		bench_len = MAX_MEM_OP_LEN;
	shell_addr = info.map_addr + info.page_size;
	bench_base = shell_addr;
	rw_alloc_len = info.page_size * 2U;
	{
		struct lkmdbg_remote_alloc_request rw_alloc_req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(rw_alloc_req),
			.remote_addr = shell_addr,
			.length = rw_alloc_len,
			.prot = LKMDBG_REMOTE_ALLOC_PROT_READ |
				LKMDBG_REMOTE_ALLOC_PROT_WRITE,
		};
		int alloc_ret;
		int alloc_errno = 0;

			errno = 0;
			alloc_ret = bridge_create_remote_alloc(
				session_fd, shell_addr, rw_alloc_len,
				LKMDBG_REMOTE_ALLOC_PROT_READ |
					LKMDBG_REMOTE_ALLOC_PROT_WRITE,
				0, &rw_alloc_req);
			if (alloc_ret < 0)
				alloc_errno = errno;
		if (alloc_ret < 0 || !rw_alloc_req.alloc_id ||
		    rw_alloc_req.mapped_length < rw_alloc_len) {
			fprintf(stderr,
				"example_perf_baseline: CREATE_REMOTE_ALLOC(rw) failed ret=%d errno=%d id=%" PRIu64
				" len=%" PRIu64 " target=%d shell=0x%" PRIxPTR " req_len=%zu page=%u map=0x%"
				PRIxPTR "/%u\n",
				alloc_ret, alloc_errno, (uint64_t)rw_alloc_req.alloc_id,
				(uint64_t)rw_alloc_req.mapped_length, child, shell_addr,
				rw_alloc_len, info.page_size, info.map_addr, info.map_len);
			goto out;
		}
		rw_alloc_id = rw_alloc_req.alloc_id;
		bench_base = (uintptr_t)rw_alloc_req.remote_addr;
	}
	if (bench_len > rw_alloc_len / 2U)
		bench_len = rw_alloc_len / 2U;
	chunk_len = (size_t)info.page_size * 16U;
	if (chunk_len > bench_len)
		chunk_len = bench_len;
	buf = malloc(bench_len);
	if (!buf) {
		fprintf(stderr, "example_perf_baseline: alloc failed len=%zu\n",
			bench_len);
		goto out;
	}
	memset(buf, 0x5A, bench_len);

	/* Warmup */
	if (run_mem_xfer_loops(session_fd, bench_base, buf, bench_len,
			       chunk_len, 1, 0, "READ_MEM warmup") < 0)
		goto out;
	if (run_mem_xfer_loops(session_fd, bench_base + bench_len, buf,
			       bench_len, chunk_len, 1, 1,
			       "WRITE_MEM warmup") < 0)
		goto out;

	t0 = now_ns();
	if (run_mem_xfer_loops(session_fd, bench_base, buf, bench_len,
			       chunk_len, LOOPS_RW, 0, "READ_MEM") < 0)
		goto out;
	t1 = now_ns();
	ns_read = t1 - t0;

	t0 = now_ns();
	if (run_mem_xfer_loops(session_fd, bench_base + bench_len, buf,
			       bench_len, chunk_len, LOOPS_RW, 1,
			       "WRITE_MEM") < 0)
		goto out;
	t1 = now_ns();
	ns_write = t1 - t0;

	t0 = now_ns();
	for (i = 0; i < LOOPS_XLATE; i++) {
		memset(&translate_op, 0, sizeof(translate_op));
		memset(&phys_req, 0, sizeof(phys_req));
		translate_op.phys_addr = bench_base + (uintptr_t)((i % 32) * 64);
		translate_op.length = 8;
		translate_op.flags = LKMDBG_PHYS_OP_FLAG_TARGET_VADDR |
				     LKMDBG_PHYS_OP_FLAG_TRANSLATE_ONLY;
		if (xfer_physical_memory(session_fd, &translate_op, 1, 0, &phys_req,
					 0) < 0 ||
		    phys_req.ops_done != 1 || !translate_op.resolved_phys_addr) {
			fprintf(stderr,
				"example_perf_baseline: translate failed loop=%d ops=%u pa=0x%" PRIx64 "\n",
				i, phys_req.ops_done,
				(uint64_t)translate_op.resolved_phys_addr);
			goto out;
		}
	}
	t1 = now_ns();
	ns_xlate = t1 - t0;

	if (rw_alloc_id) {
		struct lkmdbg_remote_alloc_handle_request rw_free_req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(rw_free_req),
			.alloc_id = rw_alloc_id,
		};

			if (bridge_remove_remote_alloc(session_fd, rw_alloc_id,
						       &rw_free_req) < 0) {
				fprintf(stderr,
					"example_perf_baseline: REMOVE_REMOTE_ALLOC(rw) failed errno=%d\n",
					errno);
			goto out;
		}
		rw_alloc_id = 0;
	}

	t0 = now_ns();
	for (i = 0; i < LOOPS_ALLOC; i++) {
		struct lkmdbg_remote_alloc_request alloc_req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(alloc_req),
			.remote_addr = shell_addr,
			.length = info.page_size * 2U,
			.prot = LKMDBG_REMOTE_ALLOC_PROT_READ |
				LKMDBG_REMOTE_ALLOC_PROT_WRITE,
		};
		struct lkmdbg_remote_alloc_handle_request free_req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(free_req),
		};

			if (bridge_create_remote_alloc(
				    session_fd, shell_addr, info.page_size * 2U,
				    LKMDBG_REMOTE_ALLOC_PROT_READ |
					    LKMDBG_REMOTE_ALLOC_PROT_WRITE,
				    0, &alloc_req) < 0 ||
			    !alloc_req.alloc_id) {
				fprintf(stderr,
					"example_perf_baseline: CREATE_REMOTE_ALLOC failed loop=%d errno=%d id=%" PRIu64
				" len=%" PRIu64 " shell=0x%" PRIxPTR "\n",
				i, errno, (uint64_t)alloc_req.alloc_id,
				(uint64_t)alloc_req.mapped_length, shell_addr);
			goto out;
		}

		free_req.alloc_id = alloc_req.alloc_id;
			if (bridge_remove_remote_alloc(session_fd,
						       free_req.alloc_id,
						       &free_req) < 0) {
				fprintf(stderr,
					"example_perf_baseline: REMOVE_REMOTE_ALLOC failed loop=%d errno=%d\n",
					i, errno);
			goto out;
		}
	}
	t1 = now_ns();
	ns_alloc = t1 - t0;

	read_mb_s = ((double)bench_len * (double)LOOPS_RW) /
		    ((double)ns_read / 1e9) / (1024.0 * 1024.0);
	write_mb_s = ((double)bench_len * (double)LOOPS_RW) /
		     ((double)ns_write / 1e9) / (1024.0 * 1024.0);
	xlate_kops = ((double)LOOPS_XLATE / ((double)ns_xlate / 1e9)) / 1000.0;
	alloc_us = ((double)ns_alloc / 1000.0) / (double)LOOPS_ALLOC;

	status = 0;
	printf("example_perf_baseline: ok read_mb_s=%.2f write_mb_s=%.2f xlate_kops=%.2f alloc_us=%.2f len=%zu loops_rw=%d loops_xlate=%d loops_alloc=%d\n",
	       read_mb_s, write_mb_s, xlate_kops, alloc_us, bench_len, LOOPS_RW,
	       LOOPS_XLATE, LOOPS_ALLOC);

out:
	if (rw_alloc_id && session_fd >= 0) {
		struct lkmdbg_remote_alloc_handle_request rw_free_req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(rw_free_req),
			.alloc_id = rw_alloc_id,
		};
		(void)bridge_remove_remote_alloc(session_fd, rw_alloc_id,
						 &rw_free_req);
	}
	(void)write_full(cmd_pipe[1], &cmd, sizeof(cmd));
	free(buf);
	if (session_fd >= 0)
		close(session_fd);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
