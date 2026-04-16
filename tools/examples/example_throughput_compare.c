#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../driver/bridge_c.h"
#include "../driver/bridge_memory.h"

struct example_child_info {
	uintptr_t map_addr;
	uint32_t page_size;
	uint32_t map_len;
};

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

static int run_lkm_xfer_loops(int session_fd, uintptr_t remote_addr, uint8_t *buf,
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
					"example_throughput_compare: %s failed loop=%d off=%zu req=%zu done=%u\n",
					tag, loop, done, this_len, bytes_done);
				return -1;
			}

			done += this_len;
		}
	}

	return 0;
}

static int run_vm_xfer_loops(pid_t pid, uintptr_t remote_addr, uint8_t *buf,
			     size_t total_len, size_t chunk_len, int loops,
			     int write, const char *tag)
{
	int loop;

	for (loop = 0; loop < loops; loop++) {
		size_t done = 0;

		while (done < total_len) {
			size_t this_len = total_len - done;
			struct iovec local_iov;
			struct iovec remote_iov;
			ssize_t ret;

			if (this_len > chunk_len)
				this_len = chunk_len;

			local_iov.iov_base = buf + done;
			local_iov.iov_len = this_len;
			remote_iov.iov_base = (void *)(remote_addr + done);
			remote_iov.iov_len = this_len;

			do {
				if (write) {
					ret = process_vm_writev(
						pid, &local_iov, 1, &remote_iov,
						1, 0);
				} else {
					ret = process_vm_readv(
						pid, &local_iov, 1, &remote_iov,
						1, 0);
				}
			} while (ret < 0 && errno == EINTR);

			if (ret < 0 || (size_t)ret != this_len) {
				fprintf(stderr,
					"example_throughput_compare: %s failed loop=%d off=%zu req=%zu done=%zd errno=%d\n",
					tag, loop, done, this_len, ret, errno);
				return -1;
			}
			done += this_len;
		}
	}

	return 0;
}

static int run_child(int info_fd, int cmd_fd)
{
	enum {
		CHILD_MAP_LEN = 16 * 1024 * 1024,
	};
	struct example_child_info info;
	uint8_t *map;
	size_t i;
	char cmd = 0;

	memset(&info, 0, sizeof(info));
	info.page_size = (uint32_t)getpagesize();
	info.map_len = CHILD_MAP_LEN;

	map = mmap(NULL, info.map_len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		return 1;

	for (i = 0; i < info.map_len; i++)
		map[i] = (uint8_t)(0x30U + (i & 0x4fU));

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
		LOOPS_RW = 8,
		DEFAULT_BENCH_LEN = 4 * 1024 * 1024,
		DEFAULT_CHUNK_LEN = 256 * 1024,
		MAX_MEM_OP_LEN = 256 * 1024,
	};
	int info_pipe[2];
	int cmd_pipe[2];
	struct example_child_info info;
	uint8_t *read_buf = NULL;
	uint8_t *write_buf = NULL;
	uint8_t *verify_buf = NULL;
	uint64_t t0;
	uint64_t t1;
	uint64_t ns_lkm_read = 0;
	uint64_t ns_lkm_write = 0;
	uint64_t ns_vm_read = 0;
	uint64_t ns_vm_write = 0;
	double lkm_read_mb_s;
	double lkm_write_mb_s;
	double vm_read_mb_s;
	double vm_write_mb_s;
	double read_ratio;
	double write_ratio;
	double bytes_per_path_mb;
	uintptr_t read_base;
	uintptr_t write_base;
	size_t bench_len;
	size_t chunk_len;
	size_t i;
	pid_t child;
	int session_fd = -1;
	char cmd = 'q';
	int status = 1;

	memset(&info, 0, sizeof(info));

	if (pipe(info_pipe) != 0 || pipe(cmd_pipe) != 0) {
		fprintf(stderr,
			"example_throughput_compare: pipe failed errno=%d\n",
			errno);
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr,
			"example_throughput_compare: fork failed errno=%d\n",
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
			"example_throughput_compare: child info read failed\n");
		goto out;
	}
	if (!info.map_addr || info.map_len < DEFAULT_BENCH_LEN * 2U) {
		fprintf(stderr,
			"example_throughput_compare: invalid child map addr=0x%" PRIxPTR
			" len=%u\n",
			info.map_addr, info.map_len);
		goto out;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;

	bench_len = DEFAULT_BENCH_LEN;
	if (bench_len > (size_t)info.map_len / 2U)
		bench_len = (size_t)info.map_len / 2U;
	chunk_len = DEFAULT_CHUNK_LEN;
	if (chunk_len > MAX_MEM_OP_LEN)
		chunk_len = MAX_MEM_OP_LEN;
	if (chunk_len > bench_len)
		chunk_len = bench_len;
	if (!bench_len || !chunk_len) {
		fprintf(stderr,
			"example_throughput_compare: invalid bench params len=%zu chunk=%zu\n",
			bench_len, chunk_len);
		goto out;
	}

	read_base = info.map_addr;
	write_base = info.map_addr + bench_len;

	read_buf = malloc(bench_len);
	write_buf = malloc(bench_len);
	verify_buf = malloc(bench_len);
	if (!read_buf || !write_buf || !verify_buf) {
		fprintf(stderr,
			"example_throughput_compare: alloc failed len=%zu\n",
			bench_len);
		goto out;
	}
	memset(read_buf, 0, bench_len);
	for (i = 0; i < bench_len; i++)
		write_buf[i] = (uint8_t)(0xa0U + (i & 0x1fU));
	memset(verify_buf, 0, bench_len);

	if (run_lkm_xfer_loops(session_fd, read_base, read_buf, bench_len,
			       chunk_len, 1, 0, "READ_MEM warmup") < 0)
		goto out;
	if (run_lkm_xfer_loops(session_fd, write_base, write_buf, bench_len,
			       chunk_len, 1, 1, "WRITE_MEM warmup") < 0)
		goto out;
	if (run_vm_xfer_loops(child, read_base, read_buf, bench_len, chunk_len, 1,
			      0, "process_vm_readv warmup") < 0)
		goto out;
	if (run_vm_xfer_loops(child, write_base, write_buf, bench_len, chunk_len,
			      1, 1, "process_vm_writev warmup") < 0)
		goto out;

	t0 = now_ns();
	if (run_lkm_xfer_loops(session_fd, read_base, read_buf, bench_len,
			       chunk_len, LOOPS_RW, 0, "READ_MEM") < 0)
		goto out;
	t1 = now_ns();
	ns_lkm_read = t1 - t0;

	t0 = now_ns();
	if (run_lkm_xfer_loops(session_fd, write_base, write_buf, bench_len,
			       chunk_len, LOOPS_RW, 1, "WRITE_MEM") < 0)
		goto out;
	t1 = now_ns();
	ns_lkm_write = t1 - t0;

	t0 = now_ns();
	if (run_vm_xfer_loops(child, read_base, read_buf, bench_len, chunk_len,
			      LOOPS_RW, 0, "process_vm_readv") < 0)
		goto out;
	t1 = now_ns();
	ns_vm_read = t1 - t0;

	t0 = now_ns();
	if (run_vm_xfer_loops(child, write_base, write_buf, bench_len, chunk_len,
			      LOOPS_RW, 1, "process_vm_writev") < 0)
		goto out;
	t1 = now_ns();
	ns_vm_write = t1 - t0;

	if (run_vm_xfer_loops(child, write_base, verify_buf, bench_len, chunk_len,
			      1, 0, "process_vm_readv verify") < 0)
		goto out;
	if (memcmp(write_buf, verify_buf, bench_len) != 0) {
		fprintf(stderr,
			"example_throughput_compare: write verify mismatch\n");
		goto out;
	}

	lkm_read_mb_s = ((double)bench_len * (double)LOOPS_RW) /
			((double)ns_lkm_read / 1e9) / (1024.0 * 1024.0);
	lkm_write_mb_s = ((double)bench_len * (double)LOOPS_RW) /
			 ((double)ns_lkm_write / 1e9) / (1024.0 * 1024.0);
	vm_read_mb_s = ((double)bench_len * (double)LOOPS_RW) /
		       ((double)ns_vm_read / 1e9) / (1024.0 * 1024.0);
	vm_write_mb_s = ((double)bench_len * (double)LOOPS_RW) /
			((double)ns_vm_write / 1e9) / (1024.0 * 1024.0);
	read_ratio = vm_read_mb_s > 0.0 ? lkm_read_mb_s / vm_read_mb_s : 0.0;
	write_ratio = vm_write_mb_s > 0.0 ? lkm_write_mb_s / vm_write_mb_s : 0.0;
	bytes_per_path_mb =
		((double)bench_len * (double)LOOPS_RW) / (1024.0 * 1024.0);

	status = 0;
	printf("example_throughput_compare: ok len=%zu chunk=%zu loops=%d transferred_mb=%.2f lkmdbg_read_mb_s=%.2f lkmdbg_write_mb_s=%.2f vm_readv_mb_s=%.2f vm_writev_mb_s=%.2f read_ratio=%.3f write_ratio=%.3f\n",
	       bench_len, chunk_len, LOOPS_RW, bytes_per_path_mb, lkm_read_mb_s,
	       lkm_write_mb_s, vm_read_mb_s, vm_write_mb_s, read_ratio,
	       write_ratio);

out:
	(void)write_full(cmd_pipe[1], &cmd, sizeof(cmd));
	free(read_buf);
	free(write_buf);
	free(verify_buf);
	if (session_fd >= 0)
		close(session_fd);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
