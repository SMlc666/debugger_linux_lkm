#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lkmdbg/lkmdbg.h"

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(seed + i * 7U);
}

static int query_alloc_exists(struct lkmdbg_session *session, uint64_t alloc_id,
			      int *exists_out)
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
		if (lkmdbg_remote_alloc_query(
			    session, cursor, entries,
			    (uint32_t)(sizeof(entries) / sizeof(entries[0])),
			    &req) < 0)
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

struct example_child_info {
	uintptr_t map_addr;
	uint32_t page_size;
	uint32_t map_len;
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
	struct example_child_info info;
	void *map;
	char cmd;

	memset(&info, 0, sizeof(info));
	info.page_size = (uint32_t)getpagesize();
	info.map_len = info.page_size * 4U;
	map = mmap(NULL, info.map_len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		return 1;
	memset(map, 0xCC, info.map_len);
	info.map_addr = (uintptr_t)map;
	if (write_full(info_fd, &info, sizeof(info)) != (ssize_t)sizeof(info))
		return 1;

	for (;;) {
		if (read_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
			return 1;
		if (cmd == 'q')
			break;
	}
	munmap(map, info.map_len);
	return 0;
}

int main(void)
{
	struct lkmdbg_remote_alloc_options alloc_options = {
		.protection = LKMDBG_REMOTE_ALLOC_PROT_READ |
			LKMDBG_REMOTE_ALLOC_PROT_WRITE,
	};
	struct lkmdbg_remote_allocation *allocation = NULL;
	struct lkmdbg_session *session = NULL;
	struct lkmdbg_transfer_result transfer;
	uint8_t wbuf[128];
	uint8_t rbuf[128];
	struct example_child_info info;
	uintptr_t shell_addr;
	pid_t child;
	int info_pipe[2];
	int cmd_pipe[2];
	char cmd = 'q';
	int exists = 0;
	int status = 1;

	memset(wbuf, 0, sizeof(wbuf));
	memset(rbuf, 0, sizeof(rbuf));
	memset(&info, 0, sizeof(info));

	if (pipe(info_pipe) != 0 || pipe(cmd_pipe) != 0) {
		fprintf(stderr, "example_remote_alloc_rw: pipe failed errno=%d\n",
			errno);
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_remote_alloc_rw: fork failed errno=%d\n",
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
		fprintf(stderr, "example_remote_alloc_rw: child info read failed\n");
		goto out;
	}
	if (!info.map_addr || info.page_size == 0 || info.map_len < info.page_size * 2U) {
		fprintf(stderr,
			"example_remote_alloc_rw: child map invalid addr=0x%" PRIxPTR " page=%u len=%u\n",
			info.map_addr, info.page_size, info.map_len);
		goto out;
	}

	shell_addr = info.map_addr + info.page_size;
	alloc_options.remote_address = shell_addr;
	alloc_options.length = info.page_size * 2U;

	if (lkmdbg_session_open(&session) < 0)
		goto out;
	if (lkmdbg_session_set_target(session, child, 0) < 0)
		goto out;

	if (lkmdbg_remote_alloc_create(session, &alloc_options, &allocation) < 0) {
		fprintf(stderr,
			"example_remote_alloc_rw: CREATE_REMOTE_ALLOC failed errno=%d\n",
			errno);
		goto out;
	}
	if (!lkmdbg_remote_alloc_id(allocation) ||
	    !lkmdbg_remote_alloc_address(allocation) ||
	    lkmdbg_remote_alloc_length(allocation) < sizeof(wbuf)) {
		fprintf(stderr,
			"example_remote_alloc_rw: bad alloc reply id=%" PRIu64 " addr=0x%" PRIx64 " len=%" PRIu64 "\n",
			lkmdbg_remote_alloc_id(allocation),
			(uint64_t)lkmdbg_remote_alloc_address(allocation),
			lkmdbg_remote_alloc_length(allocation));
		goto out;
	}
	shell_addr = lkmdbg_remote_alloc_address(allocation);

	fill_pattern(wbuf, sizeof(wbuf), 0x31);
	if (lkmdbg_memory_write(session, shell_addr, wbuf, sizeof(wbuf), 0,
				&transfer) < 0 ||
	    transfer.bytes_done != sizeof(wbuf)) {
		fprintf(stderr,
			"example_remote_alloc_rw: write failed bytes_done=%" PRIu64 "\n",
			transfer.bytes_done);
		goto out;
	}
	if (lkmdbg_memory_read(session, shell_addr, rbuf, sizeof(rbuf), 0,
			       &transfer) < 0 ||
	    transfer.bytes_done != sizeof(rbuf) ||
	    memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) {
		fprintf(stderr,
			"example_remote_alloc_rw: readback mismatch bytes_done=%" PRIu64
			"\n", transfer.bytes_done);
		goto out;
	}

	if (query_alloc_exists(session, lkmdbg_remote_alloc_id(allocation),
			       &exists) < 0) {
		fprintf(stderr,
			"example_remote_alloc_rw: QUERY_REMOTE_ALLOCS failed errno=%d\n",
			errno);
		goto out;
	}
	if (!exists) {
		fprintf(stderr,
			"example_remote_alloc_rw: alloc id not found id=%" PRIu64 "\n",
			lkmdbg_remote_alloc_id(allocation));
		goto out;
	}

	{
		uint64_t removed_id = lkmdbg_remote_alloc_id(allocation);
		uint64_t removed_addr = lkmdbg_remote_alloc_address(allocation);
		uint64_t removed_length = lkmdbg_remote_alloc_length(allocation);

		if (lkmdbg_remote_alloc_destroy(allocation) < 0) {
			fprintf(stderr,
				"example_remote_alloc_rw: REMOVE_REMOTE_ALLOC failed errno=%d\n",
				errno);
			allocation = NULL;
			goto out;
		}
		allocation = NULL;

		if (query_alloc_exists(session, removed_id, &exists) < 0) {
			fprintf(stderr,
				"example_remote_alloc_rw: post-remove query failed errno=%d\n",
				errno);
			goto out;
		}
		if (exists) {
			fprintf(stderr,
				"example_remote_alloc_rw: alloc still visible id=%" PRIu64
				"\n", removed_id);
			goto out;
		}

		status = 0;
		printf("example_remote_alloc_rw: ok id=%" PRIu64
		       " addr=0x%" PRIx64 " len=%" PRIu64 "\n",
		       removed_id, removed_addr, removed_length);
	}

out:
	(void)write_full(cmd_pipe[1], &cmd, sizeof(cmd));
	if (allocation && lkmdbg_remote_alloc_destroy(allocation) < 0)
		status = 1;
	lkmdbg_session_close(session);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
