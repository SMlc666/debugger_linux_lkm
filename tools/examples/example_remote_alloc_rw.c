#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_memory.h"

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(seed + i * 7U);
}

static int query_alloc_exists(int session_fd, uint64_t alloc_id, int *exists_out)
{
	struct lkmdbg_remote_alloc_entry entries[16];
	struct lkmdbg_remote_alloc_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = (uint32_t)(sizeof(entries) / sizeof(entries[0])),
		.start_id = 0,
	};
	uint64_t cursor = 0;

	*exists_out = 0;
	for (;;) {
		uint32_t i;

		memset(entries, 0, sizeof(entries));
		req.start_id = cursor;
		if (ioctl(session_fd, LKMDBG_IOC_QUERY_REMOTE_ALLOCS, &req) < 0)
			return -1;
		for (i = 0; i < req.entries_filled; i++) {
			if (entries[i].alloc_id == alloc_id) {
				*exists_out = 1;
				return 0;
			}
		}
		if (req.done)
			return 0;
		if (req.next_id <= cursor)
			return 0;
		cursor = req.next_id;
	}
}

static int run_child(void)
{
	for (;;)
		usleep(20000);
	return 0;
}

int main(void)
{
	struct lkmdbg_remote_alloc_request alloc_req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(alloc_req),
		.length = 4096,
		.prot = LKMDBG_REMOTE_ALLOC_PROT_READ |
			LKMDBG_REMOTE_ALLOC_PROT_WRITE,
	};
	struct lkmdbg_remote_alloc_handle_request remove_req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(remove_req),
	};
	uint8_t wbuf[128];
	uint8_t rbuf[128];
	pid_t child;
	int session_fd = -1;
	uint32_t bytes_done = 0;
	int exists = 0;
	int status = 1;

	memset(wbuf, 0, sizeof(wbuf));
	memset(rbuf, 0, sizeof(rbuf));

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_remote_alloc_rw: fork failed errno=%d\n",
			errno);
		return 1;
	}
	if (child == 0)
		_exit(run_child());

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;

	if (ioctl(session_fd, LKMDBG_IOC_CREATE_REMOTE_ALLOC, &alloc_req) < 0) {
		fprintf(stderr,
			"example_remote_alloc_rw: CREATE_REMOTE_ALLOC failed errno=%d\n",
			errno);
		goto out;
	}
	if (!alloc_req.alloc_id || !alloc_req.remote_addr ||
	    alloc_req.mapped_length < sizeof(wbuf)) {
		fprintf(stderr,
			"example_remote_alloc_rw: bad alloc reply id=%" PRIu64 " addr=0x%" PRIx64 " len=%" PRIu64 "\n",
			(uint64_t)alloc_req.alloc_id, (uint64_t)alloc_req.remote_addr,
			(uint64_t)alloc_req.mapped_length);
		goto out;
	}

	fill_pattern(wbuf, sizeof(wbuf), 0x31);
	if (write_target_memory(session_fd, (uintptr_t)alloc_req.remote_addr, wbuf,
				sizeof(wbuf), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(wbuf)) {
		fprintf(stderr,
			"example_remote_alloc_rw: write failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}
	bytes_done = 0;
	if (read_target_memory(session_fd, (uintptr_t)alloc_req.remote_addr, rbuf,
			       sizeof(rbuf), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(rbuf) || memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) {
		fprintf(stderr,
			"example_remote_alloc_rw: readback mismatch bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	if (query_alloc_exists(session_fd, alloc_req.alloc_id, &exists) < 0) {
		fprintf(stderr,
			"example_remote_alloc_rw: QUERY_REMOTE_ALLOCS failed errno=%d\n",
			errno);
		goto out;
	}
	if (!exists) {
		fprintf(stderr,
			"example_remote_alloc_rw: alloc id not found id=%" PRIu64 "\n",
			(uint64_t)alloc_req.alloc_id);
		goto out;
	}

	remove_req.alloc_id = alloc_req.alloc_id;
	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_REMOTE_ALLOC, &remove_req) < 0) {
		fprintf(stderr,
			"example_remote_alloc_rw: REMOVE_REMOTE_ALLOC failed errno=%d\n",
			errno);
		goto out;
	}

	if (query_alloc_exists(session_fd, alloc_req.alloc_id, &exists) < 0) {
		fprintf(stderr,
			"example_remote_alloc_rw: post-remove query failed errno=%d\n",
			errno);
		goto out;
	}
	if (exists) {
		fprintf(stderr,
			"example_remote_alloc_rw: alloc still visible id=%" PRIu64 "\n",
			(uint64_t)alloc_req.alloc_id);
		goto out;
	}

	status = 0;
	printf("example_remote_alloc_rw: ok id=%" PRIu64 " addr=0x%" PRIx64
	       " len=%" PRIu64 "\n",
	       (uint64_t)alloc_req.alloc_id, (uint64_t)alloc_req.remote_addr,
	       (uint64_t)alloc_req.mapped_length);

out:
	if (session_fd >= 0)
		close(session_fd);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
