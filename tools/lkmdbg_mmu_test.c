#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../include/lkmdbg_ioctl.h"

#define TARGET_PATH "/proc/version"
#define QUERY_BATCH 16U
#define STOP_TIMEOUT_MS 5000
#define NO_EVENT_TIMEOUT_MS 400

struct child_info {
	uintptr_t data_addr;
	uintptr_t mutate_addr;
	uintptr_t lost_addr;
	uintptr_t exec_addr;
	uintptr_t combo_page_addr;
	uintptr_t combo_data_addr;
	uint32_t page_size;
};

struct child_cmd {
	uint32_t op;
	uint32_t reserved;
};

struct child_reply {
	int32_t status;
	int32_t aux;
};

enum {
	CHILD_OP_READ_DATA = 1,
	CHILD_OP_WRITE_DATA = 2,
	CHILD_OP_EXEC_PAGE = 3,
	CHILD_OP_READ_EXEC_PAGE = 4,
	CHILD_OP_READ_COMBO = 5,
	CHILD_OP_WRITE_COMBO = 6,
	CHILD_OP_EXEC_COMBO = 7,
	CHILD_OP_MINCORE_DATA = 8,
	CHILD_OP_MPROTECT_MUTATE_READ = 9,
	CHILD_OP_MUNMAP_LOST = 10,
	CHILD_OP_EXIT = 11,
};

static ssize_t read_full(int fd, void *buf, size_t len)
{
	size_t done = 0;
	char *ptr = buf;

	while (done < len) {
		ssize_t nr = read(fd, ptr + done, len - done);

		if (nr == 0)
			return (ssize_t)done;
		if (nr < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)nr;
	}

	return (ssize_t)done;
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
	size_t done = 0;
	const char *ptr = buf;

	while (done < len) {
		ssize_t nw = write(fd, ptr + done, len - done);

		if (nw < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)nw;
	}

	return (ssize_t)done;
}

static int drain_session_events(int session_fd)
{
	struct lkmdbg_event_record events[QUERY_BATCH];

	for (;;) {
		struct pollfd pfd = {
			.fd = session_fd,
			.events = POLLIN,
		};
		ssize_t nread;

		if (poll(&pfd, 1, 0) < 0) {
			fprintf(stderr, "drain poll failed: %s\n",
				strerror(errno));
			return -1;
		}
		if (!(pfd.revents & POLLIN))
			return 0;

		nread = read(session_fd, events, sizeof(events));
		if (nread < 0) {
			fprintf(stderr, "drain read failed: %s\n",
				strerror(errno));
			return -1;
		}
		if (nread == 0)
			return 0;
		if (nread % (ssize_t)sizeof(events[0]) != 0) {
			fprintf(stderr, "short drain read: %zd\n", nread);
			return -1;
		}
	}
}

static int wait_for_session_event(int session_fd,
				  struct lkmdbg_event_record *event_out,
				  int timeout_ms)
{
	struct pollfd pfd = {
		.fd = session_fd,
		.events = POLLIN,
	};
	int poll_ret;
	ssize_t nread;

	poll_ret = poll(&pfd, 1, timeout_ms);
	if (poll_ret < 0) {
		fprintf(stderr, "event poll failed: %s\n", strerror(errno));
		return -1;
	}
	if (poll_ret == 0)
		return 1;
	if (!(pfd.revents & POLLIN)) {
		fprintf(stderr, "unexpected event revents=0x%x\n", pfd.revents);
		return -1;
	}

	nread = read(session_fd, event_out, sizeof(*event_out));
	if (nread < 0) {
		fprintf(stderr, "event read failed: %s\n", strerror(errno));
		return -1;
	}
	if ((size_t)nread != sizeof(*event_out)) {
		fprintf(stderr, "short event read: %zd\n", nread);
		return -1;
	}

	return 0;
}

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

static int add_hwpoint(int session_fd, uint64_t addr, uint32_t type,
		       uint32_t len, uint32_t flags,
		       struct lkmdbg_hwpoint_request *reply_out)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.type = type,
		.len = len,
		.flags = flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_ADD_HWPOINT, &req) < 0) {
		fprintf(stderr,
			"ADD_HWPOINT failed addr=0x%" PRIx64 " type=0x%x flags=0x%x errno=%d\n",
			addr, type, flags, errno);
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int add_hwpoint_expect_errno(int session_fd, uint64_t addr, uint32_t type,
				    uint32_t len, uint32_t flags,
				    int expected_errno)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.type = type,
		.len = len,
		.flags = flags,
	};

	errno = 0;
	if (ioctl(session_fd, LKMDBG_IOC_ADD_HWPOINT, &req) == 0) {
		fprintf(stderr,
			"ADD_HWPOINT unexpectedly succeeded addr=0x%" PRIx64 "\n",
			addr);
		return -1;
	}
	if (errno != expected_errno) {
		fprintf(stderr,
			"ADD_HWPOINT errno=%d expected=%d addr=0x%" PRIx64 "\n",
			errno, expected_errno, addr);
		return -1;
	}

	return 0;
}

static int remove_hwpoint(int session_fd, uint64_t id)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_HWPOINT, &req) < 0) {
		fprintf(stderr, "REMOVE_HWPOINT failed id=%" PRIu64 " errno=%d\n",
			id, errno);
		return -1;
	}

	return 0;
}

static int rearm_hwpoint(int session_fd, uint64_t id)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REARM_HWPOINT, &req) < 0) {
		fprintf(stderr, "REARM_HWPOINT failed id=%" PRIu64 " errno=%d\n",
			id, errno);
		return -1;
	}

	return 0;
}

static int rearm_hwpoint_expect_errno(int session_fd, uint64_t id,
				      int expected_errno)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	errno = 0;
	if (ioctl(session_fd, LKMDBG_IOC_REARM_HWPOINT, &req) == 0) {
		fprintf(stderr, "REARM_HWPOINT unexpectedly succeeded id=%" PRIu64 "\n",
			id);
		return -1;
	}
	if (errno != expected_errno) {
		fprintf(stderr,
			"REARM_HWPOINT errno=%d expected=%d id=%" PRIu64 "\n",
			errno, expected_errno, id);
		return -1;
	}

	return 0;
}

static int query_hwpoint_entry(int session_fd, uint64_t id,
			       struct lkmdbg_hwpoint_entry *entry_out)
{
	struct lkmdbg_hwpoint_entry entries[QUERY_BATCH];
	uint64_t cursor = 0;

	for (;;) {
		struct lkmdbg_hwpoint_query_request req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(req),
			.entries_addr = (uintptr_t)entries,
			.max_entries = QUERY_BATCH,
			.start_id = cursor,
		};
		uint32_t i;

		if (ioctl(session_fd, LKMDBG_IOC_QUERY_HWPOINTS, &req) < 0) {
			fprintf(stderr, "QUERY_HWPOINTS failed: %s\n",
				strerror(errno));
			return -1;
		}

		for (i = 0; i < req.entries_filled; i++) {
			if (entries[i].id != id)
				continue;
			*entry_out = entries[i];
			return 0;
		}

		if (req.done)
			break;
		cursor = req.next_id;
	}

	fprintf(stderr, "hwpoint %" PRIu64 " not found\n", id);
	return -1;
}

static int expect_hwpoint_state_bits(int session_fd, uint64_t id, uint32_t set_bits,
				     uint32_t clear_bits, const char *label)
{
	struct lkmdbg_hwpoint_entry entry;

	if (query_hwpoint_entry(session_fd, id, &entry) < 0)
		return -1;
	if ((entry.state & set_bits) != set_bits ||
	    (entry.state & clear_bits) != 0) {
		fprintf(stderr,
			"%s state mismatch got=0x%x require=0x%x clear=0x%x\n",
			label, entry.state, set_bits, clear_bits);
		return -1;
	}

	return 0;
}

static int get_stop_state(int session_fd,
			  struct lkmdbg_stop_query_request *reply_out)
{
	struct lkmdbg_stop_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_STOP_STATE, &req) < 0) {
		fprintf(stderr, "GET_STOP_STATE failed: %s\n", strerror(errno));
		return -1;
	}

	*reply_out = req;
	return 0;
}

static int continue_target(int session_fd, uint64_t stop_cookie, uint32_t flags)
{
	struct lkmdbg_continue_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.flags = flags,
		.timeout_ms = 2000,
		.stop_cookie = stop_cookie,
	};

	if (ioctl(session_fd, LKMDBG_IOC_CONTINUE_TARGET, &req) < 0) {
		fprintf(stderr, "CONTINUE_TARGET failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int send_child_command(int cmd_fd, uint32_t op)
{
	struct child_cmd cmd = {
		.op = op,
	};

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "send child op=%u failed\n", op);
		return -1;
	}

	return 0;
}

static int read_child_reply(int reply_fd, struct child_reply *reply_out)
{
	if (read_full(reply_fd, reply_out, sizeof(*reply_out)) !=
	    (ssize_t)sizeof(*reply_out)) {
		fprintf(stderr, "read child reply failed\n");
		return -1;
	}

	return 0;
}

static int expect_child_reply_ok(int reply_fd, const char *label)
{
	struct child_reply reply;

	if (read_child_reply(reply_fd, &reply) < 0)
		return -1;
	if (reply.status != 0) {
		fprintf(stderr, "%s child status=%d aux=%d\n", label,
			reply.status, reply.aux);
		return -1;
	}

	return 0;
}

static int expect_no_stop_event(int session_fd, const char *label)
{
	struct lkmdbg_event_record event;
	int ret;

	ret = wait_for_session_event(session_fd, &event, NO_EVENT_TIMEOUT_MS);
	if (ret == 1)
		return 0;
	if (ret < 0)
		return -1;

	fprintf(stderr,
		"%s unexpected event type=%u code=%u flags=0x%x value0=0x%" PRIx64 "\n",
		label, event.type, event.code, event.flags,
		(uint64_t)event.value0);
	return -1;
}

static int expect_stop_event(int session_fd, uint32_t expected_reason,
			     uint32_t expected_event_flags,
			     uint64_t expected_addr,
			     struct lkmdbg_stop_query_request *stop_out)
{
	struct lkmdbg_event_record event;
	struct lkmdbg_stop_query_request stop_req;
	int ret;

	ret = wait_for_session_event(session_fd, &event, STOP_TIMEOUT_MS);
	if (ret == 1) {
		fprintf(stderr, "stop event timed out reason=%u\n",
			expected_reason);
		return -1;
	}
	if (ret < 0)
		return -1;

	if (event.type != LKMDBG_EVENT_TARGET_STOP ||
	    event.code != expected_reason ||
	    event.flags != expected_event_flags ||
	    event.value0 != expected_addr) {
		fprintf(stderr,
			"stop event mismatch type=%u code=%u flags=0x%x addr=0x%" PRIx64 "\n",
			event.type, event.code, event.flags,
			(uint64_t)event.value0);
		return -1;
	}

	if (get_stop_state(session_fd, &stop_req) < 0)
		return -1;
	if (stop_req.stop.reason != expected_reason ||
	    !(stop_req.stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
	    !(stop_req.stop.flags & LKMDBG_STOP_FLAG_REARM_REQUIRED) ||
	    stop_req.stop.event_flags != expected_event_flags ||
	    stop_req.stop.value0 != expected_addr) {
		fprintf(stderr,
			"stop state mismatch reason=%u flags=0x%x event_flags=0x%x value0=0x%" PRIx64 "\n",
			stop_req.stop.reason, stop_req.stop.flags,
			stop_req.stop.event_flags, (uint64_t)stop_req.stop.value0);
		return -1;
	}

	if (stop_out)
		*stop_out = stop_req;
	return 0;
}

static int expect_command_no_stop(int session_fd, int cmd_fd, int reply_fd,
				  uint32_t op, const char *label)
{
	if (send_child_command(cmd_fd, op) < 0)
		return -1;
	if (expect_no_stop_event(session_fd, label) < 0)
		return -1;
	if (expect_child_reply_ok(reply_fd, label) < 0)
		return -1;
	return 0;
}

static int expect_command_stop(int session_fd, int cmd_fd, int reply_fd,
			       uint32_t op, uint32_t reason,
			       uint32_t event_flags, uint64_t event_addr,
			       const char *label)
{
	struct lkmdbg_stop_query_request stop_req;

	if (send_child_command(cmd_fd, op) < 0)
		return -1;
	if (expect_stop_event(session_fd, reason, event_flags, event_addr,
			      &stop_req) < 0) {
		fprintf(stderr, "%s stop validation failed\n", label);
		return -1;
	}
	if (continue_target(session_fd, stop_req.stop.cookie, 0) < 0)
		return -1;
	if (expect_child_reply_ok(reply_fd, label) < 0)
		return -1;
	return 0;
}

static int run_mincore_command(int session_fd, int cmd_fd, int reply_fd,
			       int *resident_out)
{
	struct child_reply reply;

	if (send_child_command(cmd_fd, CHILD_OP_MINCORE_DATA) < 0)
		return -1;
	if (expect_no_stop_event(session_fd, "mincore") < 0)
		return -1;
	if (read_child_reply(reply_fd, &reply) < 0)
		return -1;
	if (reply.status != 0) {
		fprintf(stderr, "mincore child status=%d aux=%d\n", reply.status,
			reply.aux);
		return -1;
	}

	*resident_out = reply.aux;
	return 0;
}

static void arm64_write_ret_stub(void *page)
{
	static const uint32_t insns[] = {
		0xd503201fU,
		0xd65f03c0U,
	};

	memcpy(page, insns, sizeof(insns));
	__builtin___clear_cache((char *)page, (char *)page + sizeof(insns));
}

static int map_exec_page(void **page_out, bool writable_after_exec)
{
	void *page;
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	int prot = PROT_READ | PROT_WRITE;

	if (writable_after_exec)
		prot |= PROT_EXEC;

	page = mmap(NULL, page_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED)
		return -1;

	arm64_write_ret_stub(page);
	if (!writable_after_exec &&
	    mprotect(page, page_size, PROT_READ | PROT_EXEC) < 0) {
		munmap(page, page_size);
		return -1;
	}

	*page_out = page;
	return 0;
}

static int child_reply_status(int reply_fd, int status, int aux)
{
	struct child_reply reply = {
		.status = status,
		.aux = aux,
	};

	return write_full(reply_fd, &reply, sizeof(reply)) ==
		       (ssize_t)sizeof(reply) ?
		       0 :
		       -1;
}

static int child_run(int info_fd, int cmd_fd, int reply_fd)
{
	struct child_info info;
	size_t page_size;
	uint8_t *data_page;
	uint8_t *mutate_page;
	uint8_t *lost_page;
	uint8_t *exec_page;
	uint8_t *combo_page;

	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (!page_size)
		return 2;

	data_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mutate_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	lost_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (data_page == MAP_FAILED || mutate_page == MAP_FAILED ||
	    lost_page == MAP_FAILED)
		return 2;

	if (map_exec_page((void **)&exec_page, false) < 0)
		return 2;
	if (map_exec_page((void **)&combo_page, true) < 0)
		return 2;

	*(volatile uint64_t *)(data_page + 0x80) = 0x1111111111111111ULL;
	*(volatile uint64_t *)(mutate_page + 0x80) = 0x2222222222222222ULL;
	*(volatile uint64_t *)(lost_page + 0x80) = 0x3333333333333333ULL;
	*(volatile uint64_t *)(combo_page + 0x100) = 0x4444444444444444ULL;

	memset(&info, 0, sizeof(info));
	info.data_addr = (uintptr_t)(data_page + 0x80);
	info.mutate_addr = (uintptr_t)(mutate_page + 0x80);
	info.lost_addr = (uintptr_t)(lost_page + 0x80);
	info.exec_addr = (uintptr_t)exec_page;
	info.combo_page_addr = (uintptr_t)combo_page;
	info.combo_data_addr = (uintptr_t)(combo_page + 0x100);
	info.page_size = (uint32_t)page_size;

	if (write_full(info_fd, &info, sizeof(info)) != (ssize_t)sizeof(info))
		return 2;

	for (;;) {
		struct child_cmd cmd;
		int resident;

		if (read_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
			return 2;

		switch (cmd.op) {
		case CHILD_OP_READ_DATA:
			(void)*(volatile uint64_t *)info.data_addr;
			if (child_reply_status(reply_fd, 0, 0) < 0)
				return 2;
			break;
		case CHILD_OP_WRITE_DATA:
			(*(volatile uint64_t *)info.data_addr)++;
			if (child_reply_status(reply_fd, 0, 0) < 0)
				return 2;
			break;
		case CHILD_OP_EXEC_PAGE:
			((void (*)(void))info.exec_addr)();
			if (child_reply_status(reply_fd, 0, 0) < 0)
				return 2;
			break;
		case CHILD_OP_READ_EXEC_PAGE:
			(void)*(volatile uint32_t *)info.exec_addr;
			if (child_reply_status(reply_fd, 0, 0) < 0)
				return 2;
			break;
		case CHILD_OP_READ_COMBO:
			(void)*(volatile uint64_t *)info.combo_data_addr;
			if (child_reply_status(reply_fd, 0, 0) < 0)
				return 2;
			break;
		case CHILD_OP_WRITE_COMBO:
			(*(volatile uint64_t *)info.combo_data_addr)++;
			if (child_reply_status(reply_fd, 0, 0) < 0)
				return 2;
			break;
		case CHILD_OP_EXEC_COMBO:
			((void (*)(void))info.combo_page_addr)();
			if (child_reply_status(reply_fd, 0, 0) < 0)
				return 2;
			break;
		case CHILD_OP_MINCORE_DATA:
		{
			unsigned char vec = 0;

			if (mincore((void *)(info.data_addr & ~(uintptr_t)(page_size - 1)),
				    page_size, &vec) < 0)
				resident = -errno;
			else
				resident = !!(vec & 1);
			if (child_reply_status(reply_fd, resident < 0 ? resident : 0,
					       resident < 0 ? 0 : resident) < 0)
				return 2;
			break;
		}
		case CHILD_OP_MPROTECT_MUTATE_READ:
			if (mprotect((void *)(info.mutate_addr &
					     ~(uintptr_t)(page_size - 1)),
				     page_size, PROT_READ) < 0)
				resident = -errno;
			else
				resident = 0;
			if (child_reply_status(reply_fd, resident, 0) < 0)
				return 2;
			break;
		case CHILD_OP_MUNMAP_LOST:
			if (munmap((void *)(info.lost_addr &
					   ~(uintptr_t)(page_size - 1)),
				   page_size) < 0)
				resident = -errno;
			else
				resident = 0;
			if (child_reply_status(reply_fd, resident, 0) < 0)
				return 2;
			break;
		case CHILD_OP_EXIT:
			if (child_reply_status(reply_fd, 0, 0) < 0)
				return 2;
			return 0;
		default:
			return 2;
		}
	}
}

static int read_pagemap_entry(pid_t pid, uintptr_t addr, uint64_t *entry_out)
{
	char path[64];
	uint64_t entry = 0;
	off_t offset;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
		return -1;
	}

	offset = (off_t)((addr / getpagesize()) * sizeof(entry));
	if (pread(fd, &entry, sizeof(entry), offset) != (ssize_t)sizeof(entry)) {
		fprintf(stderr, "pread(%s) failed: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	*entry_out = entry;
	return 0;
}

static int read_maps_perms(pid_t pid, uintptr_t addr, char perms_out[5])
{
	char path[64];
	FILE *fp;
	char line[512];

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	fp = fopen(path, "re");
	if (!fp) {
		fprintf(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned long long start;
		unsigned long long end;
		char perms[5];

		if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3)
			continue;
		if (addr < (uintptr_t)start || addr >= (uintptr_t)end)
			continue;
		snprintf(perms_out, 5, "%s", perms);
		fclose(fp);
		return 0;
	}

	fclose(fp);
	fprintf(stderr, "maps entry missing for 0x%" PRIxPTR "\n", addr);
	return -1;
}

static int check_process_vm_read(pid_t pid, uintptr_t remote_addr)
{
	uint64_t local = 0;
	struct iovec local_iov = {
		.iov_base = &local,
		.iov_len = sizeof(local),
	};
	struct iovec remote_iov = {
		.iov_base = (void *)remote_addr,
		.iov_len = sizeof(local),
	};
	ssize_t nr;

	errno = 0;
	nr = process_vm_readv(pid, &local_iov, 1, &remote_iov, 1, 0);
	if (nr == (ssize_t)sizeof(local))
		return 0;
	if (nr < 0 && errno == EFAULT)
		return 0;

	fprintf(stderr, "process_vm_readv unexpected nr=%zd errno=%d\n", nr,
		errno);
	return -1;
}

static int check_process_vm_write(pid_t pid, uintptr_t remote_addr)
{
	uint64_t local = 0xA5A55A5AA5A55A5AULL;
	struct iovec local_iov = {
		.iov_base = &local,
		.iov_len = sizeof(local),
	};
	struct iovec remote_iov = {
		.iov_base = (void *)remote_addr,
		.iov_len = sizeof(local),
	};
	ssize_t nr;

	errno = 0;
	nr = process_vm_writev(pid, &local_iov, 1, &remote_iov, 1, 0);
	if (nr == (ssize_t)sizeof(local))
		return 0;
	if (nr < 0 && errno == EFAULT)
		return 0;

	fprintf(stderr, "process_vm_writev unexpected nr=%zd errno=%d\n", nr,
		errno);
	return -1;
}

static int test_read_filter(int session_fd, int cmd_fd, int reply_fd,
			    const struct child_info *info)
{
	struct lkmdbg_hwpoint_request req;

	if (add_hwpoint(session_fd, info->data_addr, LKMDBG_HWPOINT_TYPE_READ, 8,
			LKMDBG_HWPOINT_FLAG_MMU, &req) < 0)
		return -1;
	if (expect_hwpoint_state_bits(session_fd, req.id,
				      LKMDBG_HWPOINT_STATE_ACTIVE,
				      LKMDBG_HWPOINT_STATE_LATCHED |
					      LKMDBG_HWPOINT_STATE_LOST |
					      LKMDBG_HWPOINT_STATE_MUTATED,
				      "read_filter_active") < 0)
		goto fail;
	if (expect_command_no_stop(session_fd, cmd_fd, reply_fd,
				   CHILD_OP_WRITE_DATA,
				   "read_filter_write_passthrough") < 0)
		goto fail;
	if (expect_hwpoint_state_bits(session_fd, req.id,
				      LKMDBG_HWPOINT_STATE_ACTIVE,
				      LKMDBG_HWPOINT_STATE_LATCHED |
					      LKMDBG_HWPOINT_STATE_LOST |
					      LKMDBG_HWPOINT_STATE_MUTATED,
				      "read_filter_rearmed") < 0)
		goto fail;
	if (expect_command_stop(session_fd, cmd_fd, reply_fd, CHILD_OP_READ_DATA,
				LKMDBG_STOP_REASON_WATCHPOINT,
				LKMDBG_HWPOINT_TYPE_READ |
					LKMDBG_HWPOINT_FLAG_MMU,
				info->data_addr, "read_filter_stop") < 0)
		goto fail;
	if (expect_hwpoint_state_bits(session_fd, req.id,
				      LKMDBG_HWPOINT_STATE_LATCHED,
				      LKMDBG_HWPOINT_STATE_ACTIVE |
					      LKMDBG_HWPOINT_STATE_LOST |
					      LKMDBG_HWPOINT_STATE_MUTATED,
				      "read_filter_latched") < 0)
		goto fail;
	if (expect_command_no_stop(session_fd, cmd_fd, reply_fd,
				   CHILD_OP_READ_DATA,
				   "read_filter_oneshot") < 0)
		goto fail;
	if (rearm_hwpoint(session_fd, req.id) < 0)
		goto fail;
	if (expect_command_stop(session_fd, cmd_fd, reply_fd, CHILD_OP_READ_DATA,
				LKMDBG_STOP_REASON_WATCHPOINT,
				LKMDBG_HWPOINT_TYPE_READ |
					LKMDBG_HWPOINT_FLAG_MMU,
				info->data_addr, "read_filter_rearm_stop") < 0)
		goto fail;
	if (remove_hwpoint(session_fd, req.id) < 0)
		return -1;

	printf("mmu test: read filter ok\n");
	return 0;

fail:
	remove_hwpoint(session_fd, req.id);
	return -1;
}

static int test_write_filter(int session_fd, int cmd_fd, int reply_fd,
			     const struct child_info *info)
{
	struct lkmdbg_hwpoint_request req;

	if (add_hwpoint(session_fd, info->data_addr, LKMDBG_HWPOINT_TYPE_WRITE, 8,
			LKMDBG_HWPOINT_FLAG_MMU, &req) < 0)
		return -1;
	if (expect_command_no_stop(session_fd, cmd_fd, reply_fd,
				   CHILD_OP_READ_DATA,
				   "write_filter_read_passthrough") < 0)
		goto fail;
	if (expect_command_stop(session_fd, cmd_fd, reply_fd,
				CHILD_OP_WRITE_DATA,
				LKMDBG_STOP_REASON_WATCHPOINT,
				LKMDBG_HWPOINT_TYPE_WRITE |
					LKMDBG_HWPOINT_FLAG_MMU,
				info->data_addr, "write_filter_stop") < 0)
		goto fail;
	if (remove_hwpoint(session_fd, req.id) < 0)
		return -1;

	printf("mmu test: write filter ok\n");
	return 0;

fail:
	remove_hwpoint(session_fd, req.id);
	return -1;
}

static int test_combo_rx(int session_fd, int cmd_fd, int reply_fd,
			 const struct child_info *info)
{
	struct lkmdbg_hwpoint_request req;

	if (add_hwpoint(session_fd, info->exec_addr,
			LKMDBG_HWPOINT_TYPE_READ | LKMDBG_HWPOINT_TYPE_EXEC, 4,
			LKMDBG_HWPOINT_FLAG_MMU, &req) < 0)
		return -1;
	if (expect_command_stop(session_fd, cmd_fd, reply_fd,
				CHILD_OP_READ_EXEC_PAGE,
				LKMDBG_STOP_REASON_WATCHPOINT,
				LKMDBG_HWPOINT_TYPE_READ |
					LKMDBG_HWPOINT_FLAG_MMU,
				info->exec_addr, "combo_rx_read") < 0)
		goto fail;
	if (rearm_hwpoint(session_fd, req.id) < 0)
		goto fail;
	if (expect_command_stop(session_fd, cmd_fd, reply_fd, CHILD_OP_EXEC_PAGE,
				LKMDBG_STOP_REASON_BREAKPOINT,
				LKMDBG_HWPOINT_TYPE_EXEC |
					LKMDBG_HWPOINT_FLAG_MMU,
				info->exec_addr, "combo_rx_exec") < 0)
		goto fail;
	if (remove_hwpoint(session_fd, req.id) < 0)
		return -1;

	printf("mmu test: rx combo ok\n");
	return 0;

fail:
	remove_hwpoint(session_fd, req.id);
	return -1;
}

static int test_combo_rwx(int session_fd, int cmd_fd, int reply_fd,
			  const struct child_info *info)
{
	struct lkmdbg_hwpoint_request req;

	if (add_hwpoint(session_fd, info->combo_page_addr,
			LKMDBG_HWPOINT_TYPE_READ | LKMDBG_HWPOINT_TYPE_WRITE |
				LKMDBG_HWPOINT_TYPE_EXEC,
			4, LKMDBG_HWPOINT_FLAG_MMU, &req) < 0)
		return -1;
	if (add_hwpoint_expect_errno(session_fd, info->combo_data_addr,
				     LKMDBG_HWPOINT_TYPE_WRITE, 8,
				     LKMDBG_HWPOINT_FLAG_MMU, EEXIST) < 0)
		goto fail;
	if (expect_command_stop(session_fd, cmd_fd, reply_fd,
				CHILD_OP_WRITE_COMBO,
				LKMDBG_STOP_REASON_WATCHPOINT,
				LKMDBG_HWPOINT_TYPE_WRITE |
					LKMDBG_HWPOINT_FLAG_MMU,
				info->combo_page_addr, "combo_rwx_write") < 0)
		goto fail;
	if (rearm_hwpoint(session_fd, req.id) < 0)
		goto fail;
	if (expect_command_stop(session_fd, cmd_fd, reply_fd,
				CHILD_OP_READ_COMBO,
				LKMDBG_STOP_REASON_WATCHPOINT,
				LKMDBG_HWPOINT_TYPE_READ |
					LKMDBG_HWPOINT_FLAG_MMU,
				info->combo_page_addr, "combo_rwx_read") < 0)
		goto fail;
	if (rearm_hwpoint(session_fd, req.id) < 0)
		goto fail;
	if (expect_command_stop(session_fd, cmd_fd, reply_fd,
				CHILD_OP_EXEC_COMBO,
				LKMDBG_STOP_REASON_BREAKPOINT,
				LKMDBG_HWPOINT_TYPE_EXEC |
					LKMDBG_HWPOINT_FLAG_MMU,
				info->combo_page_addr, "combo_rwx_exec") < 0)
		goto fail;
	if (remove_hwpoint(session_fd, req.id) < 0)
		return -1;

	printf("mmu test: rwx combo ok\n");
	return 0;

fail:
	remove_hwpoint(session_fd, req.id);
	return -1;
}

static int test_external_ops(int session_fd, int cmd_fd, int reply_fd, pid_t pid,
			     const struct child_info *info)
{
	struct lkmdbg_hwpoint_request req;
	uint64_t pagemap_before = 0;
	uint64_t pagemap_after = 0;
	char perms_before[5];
	char perms_after[5];
	int resident = 0;

	if (read_pagemap_entry(pid, info->data_addr, &pagemap_before) < 0)
		return -1;
	if (read_maps_perms(pid, info->data_addr, perms_before) < 0)
		return -1;

	if (add_hwpoint(session_fd, info->data_addr, LKMDBG_HWPOINT_TYPE_READ, 8,
			LKMDBG_HWPOINT_FLAG_MMU, &req) < 0)
		return -1;
	if (check_process_vm_read(pid, info->data_addr) < 0)
		goto fail;
	if (check_process_vm_write(pid, info->data_addr) < 0)
		goto fail;
	if (run_mincore_command(session_fd, cmd_fd, reply_fd, &resident) < 0)
		goto fail;
	if (resident != 0 && resident != 1) {
		fprintf(stderr, "mincore resident=%d\n", resident);
		goto fail;
	}
	if (expect_no_stop_event(session_fd, "external_ops_after_mincore") < 0)
		goto fail;
	if (read_pagemap_entry(pid, info->data_addr, &pagemap_after) < 0)
		goto fail;
	if (read_maps_perms(pid, info->data_addr, perms_after) < 0)
		goto fail;
	if (strcmp(perms_before, perms_after) != 0) {
		fprintf(stderr, "maps perms changed %s -> %s\n", perms_before,
			perms_after);
		goto fail;
	}
	if (expect_hwpoint_state_bits(session_fd, req.id,
				      LKMDBG_HWPOINT_STATE_ACTIVE,
				      LKMDBG_HWPOINT_STATE_LATCHED |
					      LKMDBG_HWPOINT_STATE_LOST |
					      LKMDBG_HWPOINT_STATE_MUTATED,
				      "external_ops_state") < 0)
		goto fail;
	if (remove_hwpoint(session_fd, req.id) < 0)
		return -1;

	printf("mmu test: external ops ok resident=%d perms=%s pagemap_before=0x%016" PRIx64 " pagemap_after=0x%016" PRIx64 "\n",
	       resident, perms_after, pagemap_before, pagemap_after);
	return 0;

fail:
	remove_hwpoint(session_fd, req.id);
	return -1;
}

static int test_mutated_mapping(int session_fd, int cmd_fd, int reply_fd,
				const struct child_info *info)
{
	struct lkmdbg_hwpoint_request req;

	if (add_hwpoint(session_fd, info->mutate_addr, LKMDBG_HWPOINT_TYPE_WRITE,
			8, LKMDBG_HWPOINT_FLAG_MMU, &req) < 0)
		return -1;
	if (expect_command_no_stop(session_fd, cmd_fd, reply_fd,
				   CHILD_OP_MPROTECT_MUTATE_READ,
				   "mutate_mprotect") < 0)
		goto fail;
	if (expect_hwpoint_state_bits(session_fd, req.id,
				      LKMDBG_HWPOINT_STATE_MUTATED,
				      LKMDBG_HWPOINT_STATE_ACTIVE |
					      LKMDBG_HWPOINT_STATE_LOST,
				      "mutate_state") < 0)
		goto fail;
	if (rearm_hwpoint_expect_errno(session_fd, req.id, ESTALE) < 0)
		goto fail;
	if (remove_hwpoint(session_fd, req.id) < 0)
		return -1;

	printf("mmu test: mprotect mutation ok\n");
	return 0;

fail:
	remove_hwpoint(session_fd, req.id);
	return -1;
}

static int test_lost_mapping(int session_fd, int cmd_fd, int reply_fd,
			     const struct child_info *info)
{
	struct lkmdbg_hwpoint_request req;

	if (add_hwpoint(session_fd, info->lost_addr, LKMDBG_HWPOINT_TYPE_READ, 8,
			LKMDBG_HWPOINT_FLAG_MMU, &req) < 0)
		return -1;
	if (expect_command_no_stop(session_fd, cmd_fd, reply_fd, CHILD_OP_MUNMAP_LOST,
				   "lost_munmap") < 0)
		goto fail;
	if (expect_hwpoint_state_bits(session_fd, req.id,
				      LKMDBG_HWPOINT_STATE_LOST,
				      LKMDBG_HWPOINT_STATE_ACTIVE |
					      LKMDBG_HWPOINT_STATE_MUTATED,
				      "lost_state") < 0)
		goto fail;
	if (rearm_hwpoint_expect_errno(session_fd, req.id, ENOENT) < 0)
		goto fail;
	if (remove_hwpoint(session_fd, req.id) < 0)
		return -1;

	printf("mmu test: munmap loss ok\n");
	return 0;

fail:
	remove_hwpoint(session_fd, req.id);
	return -1;
}

static int run_selftest(void)
{
	int info_pipe[2];
	int cmd_pipe[2];
	int reply_pipe[2];
	struct child_info info;
	pid_t child;
	int session_fd = -1;
	int status;
	int ret = 1;

	if (pipe(info_pipe) < 0 || pipe(cmd_pipe) < 0 || pipe(reply_pipe) < 0) {
		fprintf(stderr, "pipe failed: %s\n", strerror(errno));
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "fork failed: %s\n", strerror(errno));
		return 1;
	}
	if (child == 0) {
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(reply_pipe[0]);
		_exit(child_run(info_pipe[1], cmd_pipe[0], reply_pipe[1]));
	}

	close(info_pipe[1]);
	close(cmd_pipe[0]);
	close(reply_pipe[1]);

	if (read_full(info_pipe[0], &info, sizeof(info)) != (ssize_t)sizeof(info)) {
		fprintf(stderr, "read child info failed\n");
		goto out;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;
	if (drain_session_events(session_fd) < 0)
		goto out;

	if (test_read_filter(session_fd, cmd_pipe[1], reply_pipe[0], &info) < 0)
		goto out;
	if (test_write_filter(session_fd, cmd_pipe[1], reply_pipe[0], &info) < 0)
		goto out;
	if (test_combo_rx(session_fd, cmd_pipe[1], reply_pipe[0], &info) < 0)
		goto out;
	if (test_combo_rwx(session_fd, cmd_pipe[1], reply_pipe[0], &info) < 0)
		goto out;
	if (test_external_ops(session_fd, cmd_pipe[1], reply_pipe[0], child, &info) <
	    0)
		goto out;
	if (test_mutated_mapping(session_fd, cmd_pipe[1], reply_pipe[0], &info) < 0)
		goto out;
	if (test_lost_mapping(session_fd, cmd_pipe[1], reply_pipe[0], &info) < 0)
		goto out;

	ret = 0;
	printf("mmu test: all checks passed\n");

out:
	if (ret == 0) {
		send_child_command(cmd_pipe[1], CHILD_OP_EXIT);
		expect_child_reply_ok(reply_pipe[0], "child_exit");
	} else {
		kill(child, SIGKILL);
	}
	if (session_fd >= 0)
		close(session_fd);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	close(reply_pipe[0]);
	if (waitpid(child, &status, 0) < 0) {
		fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
		return 1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "child exit status=%d\n", status);
		return 1;
	}

	return ret;
}

int main(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "selftest") != 0) {
		fprintf(stderr, "usage: %s selftest\n", argv[0]);
		return 1;
	}

	return run_selftest();
}
