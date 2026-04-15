#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_memory.h"

struct example_child_info {
	uintptr_t addr;
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
	static volatile char target_buf[64] = "example-vma-page-query";
	struct example_child_info info = {
		.addr = (uintptr_t)target_buf,
	};
	char cmd;

	target_buf[0] ^= 0x1;
	target_buf[0] ^= 0x1;

	if (write_full(info_fd, &info, sizeof(info)) != (ssize_t)sizeof(info))
		return 1;

	for (;;) {
		if (read_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
			return 1;
		if (cmd == 'q')
			return 0;
	}
}

static int query_vma_for_addr(int session_fd, uintptr_t addr,
			      struct lkmdbg_vma_entry *entry_out,
			      const char **name_out,
			      char *names, size_t names_size)
{
	struct lkmdbg_vma_entry entries[16];
	uint64_t cursor = 0;
	unsigned int pass;

	for (pass = 0; pass < 128; pass++) {
		struct lkmdbg_vma_query_request req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(req),
			.start_addr = cursor,
			.entries_addr = (uintptr_t)entries,
			.max_entries = (uint32_t)(sizeof(entries) / sizeof(entries[0])),
			.names_addr = (uintptr_t)names,
			.names_size = (uint32_t)names_size,
		};
		uint32_t i;

		memset(entries, 0, sizeof(entries));
		memset(names, 0, names_size);
			if (bridge_query_target_vmas(session_fd, cursor, entries,
						     (uint32_t)(sizeof(entries) /
								sizeof(entries[0])),
						     names, (uint32_t)names_size,
						     &req) < 0)
				return -1;
		for (i = 0; i < req.entries_filled; i++) {
			const struct lkmdbg_vma_entry *e = &entries[i];

			if (addr < e->start_addr || addr >= e->end_addr)
				continue;
			*entry_out = *e;
			if (name_out && e->name_size > 0 && e->name_offset < req.names_used &&
			    e->name_offset + e->name_size <= req.names_used) {
				*name_out = names + e->name_offset;
			} else if (name_out) {
				*name_out = "";
			}
			return 0;
		}
		if (req.done)
			break;
		if (req.next_addr <= cursor)
			break;
		cursor = req.next_addr;
	}

	errno = ENOENT;
	return -1;
}

int main(void)
{
	int info_pipe[2];
	int cmd_pipe[2];
	struct example_child_info info;
	struct lkmdbg_vma_entry vma;
	struct lkmdbg_page_entry pages[8];
	struct lkmdbg_page_query_request page_req;
	char names[512];
	const char *vma_name = "";
	pid_t child;
	int session_fd = -1;
	char cmd = 'q';
	uint64_t page_addr;
	uint32_t i;
	int found_page = 0;
	int status = 1;

	memset(&info, 0, sizeof(info));
	memset(&vma, 0, sizeof(vma));
	memset(pages, 0, sizeof(pages));
	memset(&page_req, 0, sizeof(page_req));
	memset(names, 0, sizeof(names));

	if (pipe(info_pipe) != 0 || pipe(cmd_pipe) != 0) {
		fprintf(stderr, "example_vma_page_query: pipe failed errno=%d\n",
			errno);
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_vma_page_query: fork failed errno=%d\n",
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
		fprintf(stderr, "example_vma_page_query: child info read failed\n");
		goto out;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;

	if (query_vma_for_addr(session_fd, info.addr, &vma, &vma_name, names,
			       sizeof(names)) < 0) {
		fprintf(stderr,
			"example_vma_page_query: QUERY_VMAS failed addr=0x%" PRIxPTR " errno=%d\n",
			info.addr, errno);
		goto out;
	}
	if (!(vma.prot & LKMDBG_VMA_PROT_READ) ||
	    !(vma.prot & LKMDBG_VMA_PROT_WRITE)) {
		fprintf(stderr,
			"example_vma_page_query: unexpected vma prot=0x%x flags=0x%x\n",
			vma.prot, vma.flags);
		goto out;
	}

	page_addr = info.addr & ~(uint64_t)(getpagesize() - 1);
	if (bridge_query_target_pages(
		    session_fd, page_addr, (uint64_t)getpagesize(), pages,
		    (uint32_t)(sizeof(pages) / sizeof(pages[0])), &page_req) < 0) {
		fprintf(stderr, "example_vma_page_query: QUERY_PAGES failed errno=%d\n",
			errno);
		goto out;
	}
	for (i = 0; i < page_req.entries_filled; i++) {
		if (pages[i].page_addr != page_addr)
			continue;
		if (!(pages[i].flags & LKMDBG_PAGE_FLAG_MAPPED)) {
			fprintf(stderr,
				"example_vma_page_query: page not mapped flags=0x%x\n",
				pages[i].flags);
			goto out;
		}
		found_page = 1;
		break;
	}
	if (!found_page) {
		fprintf(stderr,
			"example_vma_page_query: target page not found page=0x%" PRIx64 "\n",
			page_addr);
		goto out;
	}

	status = 0;
	printf("example_vma_page_query: ok addr=0x%" PRIxPTR
	       " vma=[0x%" PRIx64 ",0x%" PRIx64 ") prot=0x%x vm_flags_raw=0x%" PRIx64
	       " pgoff=0x%" PRIx64 " inode=%" PRIu64 " dev=%u:%u name=%s\n",
	       info.addr, (uint64_t)vma.start_addr, (uint64_t)vma.end_addr,
	       vma.prot, (uint64_t)vma.vm_flags_raw, (uint64_t)vma.pgoff,
	       (uint64_t)vma.inode, vma.dev_major, vma.dev_minor, vma_name);

out:
	(void)write_full(cmd_pipe[1], &cmd, sizeof(cmd));
	if (session_fd >= 0)
		close(session_fd);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
