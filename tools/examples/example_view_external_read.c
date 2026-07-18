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

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(seed + (uint8_t)(i * 13U));
}

int main(void)
{
	size_t page_size;
	uint8_t *page = MAP_FAILED;
	uint8_t *read_backing = NULL;
	uint8_t *kernel_read = NULL;
	uint8_t *external_read = NULL;
	struct lkmdbg_view_region *region = NULL;
	struct lkmdbg_session *session = NULL;
	struct lkmdbg_view_backing_request backing_reply;
	struct lkmdbg_view_backing_request policy_backing_reply;
	struct lkmdbg_view_backing_request reset_reply;
	struct lkmdbg_view_region_query_request query_reply;
	struct lkmdbg_transfer_result transfer;
	struct lkmdbg_view_region_entry entry;
	struct iovec local_iov;
	struct iovec remote_iov;
	pid_t child = -1;
	int status = 1;

	memset(&backing_reply, 0, sizeof(backing_reply));
	memset(&policy_backing_reply, 0, sizeof(policy_backing_reply));
	memset(&reset_reply, 0, sizeof(reset_reply));
	memset(&query_reply, 0, sizeof(query_reply));
	memset(&entry, 0, sizeof(entry));

	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (!page_size)
		return 1;

	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		fprintf(stderr, "example_view_external_read: mmap failed errno=%d\n",
			errno);
		return 1;
	}

	read_backing = malloc(page_size);
	kernel_read = malloc(page_size);
	external_read = malloc(page_size);
	if (!read_backing || !kernel_read || !external_read)
		goto out;

	fill_pattern(page, page_size, 0x31U);
	fill_pattern(read_backing, page_size, 0x91U);

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_view_external_read: fork failed errno=%d\n",
			errno);
		goto out;
	}
	if (child == 0) {
		for (;;)
			pause();
	}

	if (lkmdbg_session_open(&session) < 0)
		goto out;
	if (lkmdbg_session_set_target(session, child, 0) < 0)
		goto out;

	{
		struct lkmdbg_view_region_options options = {
			.base_address = (uintptr_t)page,
			.length = page_size,
			.access_mask = LKMDBG_VIEW_ACCESS_READ |
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
		    region, LKMDBG_VIEW_KIND_READ, read_backing, page_size,
		    LKMDBG_VIEW_BACKING_USER_BUFFER, &backing_reply) < 0)
		goto out;
	if (lkmdbg_view_region_set_backing(
		    region, LKMDBG_VIEW_KIND_WRITE, NULL, 0,
		    LKMDBG_VIEW_BACKING_ORIGINAL, &policy_backing_reply) < 0)
		goto out;
	if (lkmdbg_view_region_set_backing(
		    region, LKMDBG_VIEW_KIND_EXEC, NULL, 0,
		    LKMDBG_VIEW_BACKING_ORIGINAL, &policy_backing_reply) < 0)
		goto out;

	if (lkmdbg_view_regions_query(session, lkmdbg_view_region_id(region),
				      &entry, 1, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.region_id != lkmdbg_view_region_id(region) ||
	    entry.active_backend != LKMDBG_VIEW_BACKEND_EXTERNAL_READ ||
	    entry.read_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER ||
	    entry.write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL) {
		fprintf(stderr,
			"example_view_external_read: bad query active_backend=%u read=%u write=%u exec=%u filled=%u\n",
			entry.active_backend, entry.read_backing_type,
			entry.write_backing_type, entry.exec_backing_type,
			query_reply.entries_filled);
		goto out;
	}

	if (lkmdbg_memory_read(session, (uintptr_t)page, kernel_read, page_size, 0,
			       &transfer) < 0 ||
	    transfer.bytes_done != page_size ||
	    memcmp(kernel_read, page, page_size) != 0) {
		fprintf(stderr,
			"example_view_external_read: READ_MEM mismatch bytes_done=%" PRIu64
			"\n", transfer.bytes_done);
		goto out;
	}

	local_iov.iov_base = external_read;
	local_iov.iov_len = page_size;
	remote_iov.iov_base = page;
	remote_iov.iov_len = page_size;
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)page_size) {
		fprintf(stderr,
			"example_view_external_read: process_vm_readv failed errno=%d\n",
			errno);
		goto out;
	}
	if (memcmp(external_read, read_backing, page_size) != 0) {
		fprintf(stderr,
			"example_view_external_read: external read mismatch\n");
		goto out;
	}

	if (lkmdbg_view_region_set_backing(
		    region, LKMDBG_VIEW_KIND_READ, NULL, 0,
		    LKMDBG_VIEW_BACKING_ORIGINAL, &reset_reply) < 0)
		goto out;

	memset(&entry, 0, sizeof(entry));
	memset(&query_reply, 0, sizeof(query_reply));
	if (lkmdbg_view_regions_query(session, lkmdbg_view_region_id(region),
				      &entry, 1, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.read_source_id != 0) {
		fprintf(stderr,
			"example_view_external_read: reset query mismatch filled=%u read_backing=%u source=%" PRIu64 "\n",
			query_reply.entries_filled, entry.read_backing_type,
			(uint64_t)entry.read_source_id);
		goto out;
	}

	memset(external_read, 0, page_size);
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)page_size) {
		fprintf(stderr,
			"example_view_external_read: post-reset process_vm_readv failed errno=%d\n",
			errno);
		goto out;
	}
	if (memcmp(external_read, page, page_size) != 0) {
		fprintf(stderr,
			"example_view_external_read: reset did not restore original bytes\n");
		goto out;
	}

	status = 0;
	printf("example_view_external_read: ok region=%" PRIu64 " backend=%u bytes=%zu\n",
	       lkmdbg_view_region_id(region), entry.active_backend, page_size);

out:
	if (region && lkmdbg_view_region_destroy(region) < 0)
		status = 1;
	lkmdbg_session_close(session);
	if (child > 0) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
	}
	free(external_read);
	free(kernel_read);
	free(read_backing);
	if (page != MAP_FAILED)
		munmap(page, page_size);
	return status;
}
