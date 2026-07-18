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

#include "lkmdbg/lkmdbg.h"

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

static int build_exec_page(uint8_t *buf, size_t len, uint16_t retval)
{
#if defined(__aarch64__)
	uint32_t insn[2];

	if (!buf || len < 8U)
		return -1;

	(void)view_exec_original_code;
	(void)view_exec_shadow_code;
	insn[0] = 0xd2800000U | ((uint32_t)retval << 5);
	insn[1] = 0xd65f03c0U;
	memset(buf, 0, len);
	memcpy(buf, insn, sizeof(insn));
	return 0;
#else
	(void)buf;
	(void)len;
	(void)retval;
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
	uint8_t *patch_page = NULL;
	uint8_t *kernel_read = NULL;
	uint8_t *external_read = NULL;
	struct lkmdbg_view_region *region = NULL;
	struct lkmdbg_session *session = NULL;
	struct lkmdbg_view_backing_request write_backing_reply;
	struct lkmdbg_view_backing_request exec_backing_reply;
	struct lkmdbg_view_backing_request reset_reply;
	struct lkmdbg_view_region_query_request query_reply;
	struct lkmdbg_transfer_result transfer;
	struct lkmdbg_view_region_entry entry;
	struct iovec local_iov;
	struct iovec remote_iov;
	pid_t child = -1;
	int cmd_pipe[2] = { -1, -1 };
	int resp_pipe[2] = { -1, -1 };
	int status = 1;
	uint64_t retval = 0;

	memset(&write_backing_reply, 0, sizeof(write_backing_reply));
	memset(&exec_backing_reply, 0, sizeof(exec_backing_reply));
	memset(&reset_reply, 0, sizeof(reset_reply));
	memset(&query_reply, 0, sizeof(query_reply));
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
	patch_page = malloc(page_size);
	kernel_read = malloc(page_size);
	external_read = malloc(page_size);
	if (!original_page || !shadow_page || !patch_page || !kernel_read ||
	    !external_read)
		goto out;

	if (build_exec_page(original_page, page_size, 17U) < 0 ||
	    build_exec_page(shadow_page, page_size, 34U) < 0 ||
	    build_exec_page(patch_page, page_size, 51U) < 0)
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

	if (lkmdbg_session_open(&session) < 0)
		goto out;
	if (lkmdbg_session_set_target(session, child, 0) < 0)
		goto out;

	if (child_call0(cmd_pipe[1], resp_pipe[0], (uintptr_t)page, &retval) < 0 ||
	    retval != 17U) {
		fprintf(stderr,
			"example_view_wxshadow_exec: original retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	{
		struct lkmdbg_view_region_options options = {
			.base_address = (uintptr_t)page,
			.length = page_size,
			.access_mask = LKMDBG_VIEW_ACCESS_READ |
				       LKMDBG_VIEW_ACCESS_WRITE |
				       LKMDBG_VIEW_ACCESS_EXEC,
			.backend = LKMDBG_VIEW_BACKEND_AUTO,
			.fault_policy = LKMDBG_VIEW_FAULT_POLICY_TRAP_ONLY,
			.sync_policy = LKMDBG_VIEW_SYNC_NONE,
			.writeback_policy = LKMDBG_VIEW_WRITEBACK_DISCARD,
		};
		if (lkmdbg_view_region_create(session, &options, &region) < 0)
			goto out;
	}

	if (lkmdbg_view_region_set_backing(
		    region, LKMDBG_VIEW_KIND_WRITE, shadow_page, page_size,
		    LKMDBG_VIEW_BACKING_USER_BUFFER, &write_backing_reply) < 0)
		goto out;
	if (lkmdbg_view_region_set_backing(
		    region, LKMDBG_VIEW_KIND_EXEC, shadow_page, page_size,
		    LKMDBG_VIEW_BACKING_USER_BUFFER, &exec_backing_reply) < 0)
		goto out;

	if (lkmdbg_view_regions_query(session, lkmdbg_view_region_id(region),
				      &entry, 1, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.active_backend != LKMDBG_VIEW_BACKEND_WXSHADOW ||
	    entry.read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.write_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER ||
	    entry.exec_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER) {
		fprintf(stderr,
			"example_view_wxshadow_exec: bad query backend=%u read=%u write=%u exec=%u filled=%u\n",
			entry.active_backend, entry.read_backing_type,
			entry.write_backing_type, entry.exec_backing_type,
			query_reply.entries_filled);
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

	if (lkmdbg_memory_read(session, (uintptr_t)page, kernel_read, page_size, 0,
			       &transfer) < 0 ||
	    transfer.bytes_done != page_size ||
	    memcmp(kernel_read, shadow_page, page_size) != 0) {
		fprintf(stderr,
			"example_view_wxshadow_exec: READ_MEM mismatch bytes_done=%" PRIu64
			"\n", transfer.bytes_done);
		goto out;
	}

	if (child_call0(cmd_pipe[1], resp_pipe[0], (uintptr_t)page, &retval) < 0 ||
	    retval != 34U) {
		fprintf(stderr,
			"example_view_wxshadow_exec: shadow retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	if (lkmdbg_memory_write(session, (uintptr_t)page, patch_page, page_size, 0,
				&transfer) < 0 || transfer.bytes_done != page_size ||
	    lkmdbg_memory_read(session, (uintptr_t)page, kernel_read, page_size, 0,
			       &transfer) < 0 || transfer.bytes_done != page_size ||
	    memcmp(kernel_read, patch_page, page_size) != 0) {
		fprintf(stderr,
			"example_view_wxshadow_exec: WRITE_MEM patch mismatch bytes_done=%" PRIu64
			"\n", transfer.bytes_done);
		goto out;
	}

	memset(external_read, 0, page_size);
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)page_size ||
	    memcmp(external_read, original_page, page_size) != 0) {
		fprintf(stderr,
			"example_view_wxshadow_exec: patched external read mismatch errno=%d\n",
			errno);
		goto out;
	}

	if (child_call0(cmd_pipe[1], resp_pipe[0], (uintptr_t)page, &retval) < 0 ||
	    retval != 51U) {
		fprintf(stderr,
			"example_view_wxshadow_exec: patched retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	if (lkmdbg_view_region_set_backing(region, LKMDBG_VIEW_KIND_WRITE, NULL, 0,
					   LKMDBG_VIEW_BACKING_ORIGINAL,
					   &reset_reply) < 0)
		goto out;
	if (lkmdbg_view_region_set_backing(region, LKMDBG_VIEW_KIND_EXEC, NULL, 0,
					   LKMDBG_VIEW_BACKING_ORIGINAL,
					   &reset_reply) < 0)
		goto out;

	memset(&entry, 0, sizeof(entry));
	memset(&query_reply, 0, sizeof(query_reply));
	if (lkmdbg_view_regions_query(session, lkmdbg_view_region_id(region),
				      &entry, 1, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.write_source_id != 0 ||
	    entry.exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.exec_source_id != 0) {
		fprintf(stderr,
			"example_view_wxshadow_exec: reset query mismatch filled=%u write=%u write_source=%" PRIu64 " exec=%u exec_source=%" PRIu64 "\n",
			query_reply.entries_filled, entry.write_backing_type,
			(uint64_t)entry.write_source_id, entry.exec_backing_type,
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
	printf("example_view_wxshadow_exec: ok region=%" PRIu64 " write_source=%" PRIu64 " exec_source=%" PRIu64 "\n",
	       lkmdbg_view_region_id(region),
	       (uint64_t)write_backing_reply.source_id,
	       (uint64_t)exec_backing_reply.source_id);

out:
	if (cmd_pipe[1] >= 0) {
		uintptr_t zero = 0;
		(void)write_full(cmd_pipe[1], &zero, sizeof(zero));
	}
	if (region && lkmdbg_view_region_destroy(region) < 0)
		status = 1;
	lkmdbg_session_close(session);
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
	free(patch_page);
	free(shadow_page);
	free(original_page);
	if (page != MAP_FAILED)
		munmap(page, page_size);
	return status;
#endif
}
