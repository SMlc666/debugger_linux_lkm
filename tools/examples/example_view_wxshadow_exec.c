#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_memory.h"

static ssize_t write_full(int fd, const void *buf, size_t len)
{
	const uint8_t *ptr = buf;
	size_t done = 0;

	while (done < len) {
		ssize_t nw = write(fd, ptr + done, len - done);

		if (nw < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!nw)
			return -1;
		done += (size_t)nw;
	}

	return (ssize_t)done;
}

static ssize_t read_full(int fd, void *buf, size_t len)
{
	uint8_t *ptr = buf;
	size_t done = 0;

	while (done < len) {
		ssize_t nr = read(fd, ptr + done, len - done);

		if (nr < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!nr)
			return -1;
		done += (size_t)nr;
	}

	return (ssize_t)done;
}

#if defined(__aarch64__)
static const uint32_t view_exec_original_code[] = {
	0xd2800220U,
	0xd65f03c0U,
};

static const uint32_t view_exec_shadow_code[] = {
	0xd2800440U,
	0xd65f03c0U,
};
#endif

static int build_exec_page(uint8_t *buf, size_t len, int shadow)
{
#if defined(__aarch64__)
	const void *src = shadow ? (const void *)view_exec_shadow_code :
				    (const void *)view_exec_original_code;

	if (!buf || len < 8U)
		return -1;

	memset(buf, 0, len);
	memcpy(buf, src, 8U);
	return 0;
#else
	(void)buf;
	(void)len;
	(void)shadow;
	return -1;
#endif
}

static int child_call0(int cmd_fd, int resp_fd, uintptr_t addr, uint64_t *out)
{
	uint64_t retval = 0;

	if (write_full(cmd_fd, &addr, sizeof(addr)) != (ssize_t)sizeof(addr))
		return -1;
	if (read_full(resp_fd, &retval, sizeof(retval)) != (ssize_t)sizeof(retval))
		return -1;
	if (out)
		*out = retval;
	return 0;
}

int main(void)
{
#if !defined(__aarch64__)
	printf("example_view_wxshadow_exec: skipped non-aarch64 userspace\n");
	return 0;
#else
	size_t page_size;
	uint8_t *page = MAP_FAILED;
	uint8_t *original_page = NULL;
	uint8_t *shadow_page = NULL;
	uint8_t *kernel_read = NULL;
	uint8_t *external_read = NULL;
	struct lkmdbg_view_region_request region_reply;
	struct lkmdbg_view_backing_request backing_reply;
	struct lkmdbg_view_backing_request reset_reply;
	struct lkmdbg_view_region_query_request query_reply;
	struct lkmdbg_view_region_handle_request remove_reply;
	struct lkmdbg_view_region_entry entry;
	struct iovec local_iov;
	struct iovec remote_iov;
	pid_t child = -1;
	int cmd_pipe[2] = { -1, -1 };
	int resp_pipe[2] = { -1, -1 };
	int session_fd = -1;
	int status = 1;
	uint32_t bytes_done = 0;
	uint64_t retval = 0;

	memset(&region_reply, 0, sizeof(region_reply));
	memset(&backing_reply, 0, sizeof(backing_reply));
	memset(&reset_reply, 0, sizeof(reset_reply));
	memset(&query_reply, 0, sizeof(query_reply));
	memset(&remove_reply, 0, sizeof(remove_reply));
	memset(&entry, 0, sizeof(entry));

	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (!page_size)
		return 1;

	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		fprintf(stderr, "example_view_wxshadow_exec: mmap failed errno=%d\n",
			errno);
		return 1;
	}

	original_page = malloc(page_size);
	shadow_page = malloc(page_size);
	kernel_read = malloc(page_size);
	external_read = malloc(page_size);
	if (!original_page || !shadow_page || !kernel_read || !external_read)
		goto out;

	if (build_exec_page(original_page, page_size, 0) < 0 ||
	    build_exec_page(shadow_page, page_size, 1) < 0)
		goto out;
	memcpy(page, original_page, page_size);

	if (pipe(cmd_pipe) != 0 || pipe(resp_pipe) != 0) {
		fprintf(stderr, "example_view_wxshadow_exec: pipe failed errno=%d\n",
			errno);
		goto out;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_view_wxshadow_exec: fork failed errno=%d\n",
			errno);
		goto out;
	}
	if (child == 0) {
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		for (;;) {
			uintptr_t addr = 0;
			uint64_t (*fn)(void);
			uint64_t rv;

			if (read_full(cmd_pipe[0], &addr, sizeof(addr)) !=
			    (ssize_t)sizeof(addr))
				_exit(2);
			if (!addr)
				_exit(0);
			__builtin___clear_cache((char *)(uintptr_t)addr,
						(char *)(uintptr_t)addr + 64);
			fn = (uint64_t (*)(void))addr;
			rv = fn();
			if (write_full(resp_pipe[1], &rv, sizeof(rv)) !=
			    (ssize_t)sizeof(rv))
				_exit(3);
		}
	}

	close(cmd_pipe[0]);
	cmd_pipe[0] = -1;
	close(resp_pipe[1]);
	resp_pipe[1] = -1;

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;

	if (child_call0(cmd_pipe[1], resp_pipe[0], (uintptr_t)page, &retval) < 0 ||
	    retval != 17U) {
		fprintf(stderr,
			"example_view_wxshadow_exec: original retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	if (create_view_region(session_fd, (uintptr_t)page, page_size,
			       LKMDBG_VIEW_ACCESS_READ | LKMDBG_VIEW_ACCESS_EXEC,
			       LKMDBG_VIEW_BACKEND_AUTO,
			       LKMDBG_VIEW_FAULT_POLICY_TRAP_ONLY,
			       LKMDBG_VIEW_SYNC_NONE,
			       LKMDBG_VIEW_WRITEBACK_DISCARD,
			       &region_reply) < 0)
		goto out;

	if (set_view_region_exec_backing(session_fd, region_reply.region_id,
					 shadow_page, page_size,
					 LKMDBG_VIEW_BACKING_USER_BUFFER,
					 &backing_reply) < 0)
		goto out;

	if (query_view_regions(session_fd, region_reply.region_id, &entry, 1,
			       &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.active_backend != LKMDBG_VIEW_BACKEND_WXSHADOW ||
	    entry.read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.exec_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER) {
		fprintf(stderr,
			"example_view_wxshadow_exec: bad query backend=%u read=%u exec=%u filled=%u\n",
			entry.active_backend, entry.read_backing_type,
			entry.exec_backing_type, query_reply.entries_filled);
		goto out;
	}

	local_iov.iov_base = external_read;
	local_iov.iov_len = page_size;
	remote_iov.iov_base = page;
	remote_iov.iov_len = page_size;
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)page_size) {
		fprintf(stderr,
			"example_view_wxshadow_exec: process_vm_readv failed errno=%d\n",
			errno);
		goto out;
	}
	if (memcmp(external_read, original_page, page_size) != 0) {
		fprintf(stderr,
			"example_view_wxshadow_exec: external read mismatch\n");
		goto out;
	}

	if (read_target_memory(session_fd, (uintptr_t)page, kernel_read, page_size,
			       &bytes_done, 0) < 0 ||
	    bytes_done != page_size ||
	    memcmp(kernel_read, shadow_page, page_size) != 0) {
		fprintf(stderr,
			"example_view_wxshadow_exec: READ_MEM mismatch bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	if (child_call0(cmd_pipe[1], resp_pipe[0], (uintptr_t)page, &retval) < 0 ||
	    retval != 34U) {
		fprintf(stderr,
			"example_view_wxshadow_exec: shadow retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	if (set_view_region_exec_backing(session_fd, region_reply.region_id, NULL,
					 0, LKMDBG_VIEW_BACKING_ORIGINAL,
					 &reset_reply) < 0)
		goto out;

	memset(&entry, 0, sizeof(entry));
	memset(&query_reply, 0, sizeof(query_reply));
	if (query_view_regions(session_fd, region_reply.region_id, &entry, 1,
			       &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.exec_source_id != 0) {
		fprintf(stderr,
			"example_view_wxshadow_exec: reset query mismatch filled=%u exec=%u source=%" PRIu64 "\n",
			query_reply.entries_filled, entry.exec_backing_type,
			(uint64_t)entry.exec_source_id);
		goto out;
	}

	memset(external_read, 0, page_size);
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)page_size ||
	    memcmp(external_read, original_page, page_size) != 0) {
		fprintf(stderr,
			"example_view_wxshadow_exec: reset external read mismatch errno=%d\n",
			errno);
		goto out;
	}

	if (child_call0(cmd_pipe[1], resp_pipe[0], (uintptr_t)page, &retval) < 0 ||
	    retval != 17U) {
		fprintf(stderr,
			"example_view_wxshadow_exec: reset retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	status = 0;
	printf("example_view_wxshadow_exec: ok region=%" PRIu64 " source=%" PRIu64 "\n",
	       (uint64_t)region_reply.region_id,
	       (uint64_t)backing_reply.source_id);

out:
	if (cmd_pipe[1] >= 0) {
		uintptr_t zero = 0;
		(void)write_full(cmd_pipe[1], &zero, sizeof(zero));
	}
	if (session_fd >= 0 && region_reply.region_id)
		(void)remove_view_region(session_fd, region_reply.region_id,
					 &remove_reply);
	if (session_fd >= 0)
		close(session_fd);
	if (cmd_pipe[0] >= 0)
		close(cmd_pipe[0]);
	if (cmd_pipe[1] >= 0)
		close(cmd_pipe[1]);
	if (resp_pipe[0] >= 0)
		close(resp_pipe[0]);
	if (resp_pipe[1] >= 0)
		close(resp_pipe[1]);
	if (child > 0)
		waitpid(child, NULL, 0);
	free(external_read);
	free(kernel_read);
	free(shadow_page);
	free(original_page);
	if (page != MAP_FAILED)
		munmap(page, page_size);
	return status;
#endif
}
