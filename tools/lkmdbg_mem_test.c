#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/memfd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
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
#define SELFTEST_FREEZE_THREADS 2
#define SELFTEST_REG_SENTINEL 0x5A17C0DEULL
#define VMA_QUERY_BATCH 64
#define VMA_QUERY_NAMES_SIZE 65536
#define IMAGE_QUERY_BATCH 64
#define IMAGE_QUERY_NAMES_SIZE 65536
#define THREAD_QUERY_BATCH 64
#define PAGE_QUERY_BATCH 64
#define PTE_PATCH_QUERY_BATCH 64
#define EVENT_READ_BATCH 16

struct child_info {
	uintptr_t basic_addr;
	uintptr_t slots_addr;
	uintptr_t nofault_addr;
	uintptr_t force_read_addr;
	uintptr_t force_write_addr;
	uintptr_t large_addr;
	uintptr_t file_addr;
	uintptr_t exec_target_addr;
	uintptr_t watch_addr;
	uintptr_t watch_mmu_addr;
	uintptr_t freeze_counters_addr;
	uintptr_t freeze_tids_addr;
	uint64_t file_inode;
	uint32_t file_dev_major;
	uint32_t file_dev_minor;
	uint32_t freeze_thread_count;
	uint32_t page_size;
	uint32_t slot_size;
	uint32_t slot_count;
	uint32_t large_len;
};

struct child_cmd {
	uint32_t op;
	uint32_t reserved;
	uint64_t addr;
	uint32_t length;
	uint32_t value;
};

struct worker_ctx {
	int session_fd;
	uintptr_t remote_addr;
	unsigned int thread_index;
	int failed;
	char final_payload[SELFTEST_SLOT_SIZE];
};

struct freeze_child_ctx {
	volatile int *stop;
	uint64_t *counter;
	pid_t *tid_slot;
	unsigned int thread_index;
};

struct remote_map_wake_ctx {
	int cmd_fd;
	int resp_fd;
	int ret;
};

struct vma_query_buffer {
	struct lkmdbg_vma_entry *entries;
	char *names;
};

struct image_query_buffer {
	struct lkmdbg_image_entry *entries;
	char *names;
};

struct page_query_buffer {
	struct lkmdbg_page_entry *entries;
};

struct pte_patch_query_buffer {
	struct lkmdbg_pte_patch_entry *entries;
};

enum {
	CHILD_OP_QUERY_NOFAULT = 1,
	CHILD_OP_EXIT = 2,
	CHILD_OP_TRIGGER_EXEC = 3,
	CHILD_OP_TRIGGER_WATCH = 4,
	CHILD_OP_TRIGGER_SIGNAL = 5,
	CHILD_OP_SPAWN_THREAD = 6,
	CHILD_OP_TRIGGER_WATCH_MMU = 7,
	CHILD_OP_READ_REMOTE = 8,
	CHILD_OP_FILL_REMOTE = 9,
};

static int get_stop_state(int session_fd,
			  struct lkmdbg_stop_query_request *reply_out);
static int child_query_nofault_residency(int cmd_fd, int resp_fd);
static int child_read_remote_range(int cmd_fd, int resp_fd, uintptr_t addr,
				   void *buf, size_t len);
static int child_fill_remote_range(int cmd_fd, int resp_fd, uintptr_t addr,
				   size_t len, uint8_t value);
static const char *describe_pte_mode(uint32_t mode, uint32_t flags, char *buf,
				     size_t buf_size);
static const char *describe_pte_patch_state(uint32_t state, char *buf,
					    size_t buf_size);

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

static int set_target_ex(int session_fd, pid_t pid, pid_t tid)
{
	struct lkmdbg_target_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tgid = pid,
		.tid = tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_TARGET, &req) < 0) {
		fprintf(stderr, "SET_TARGET failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int set_target(int session_fd, pid_t pid)
{
	return set_target_ex(session_fd, pid, 0);
}

static int drain_session_events(int session_fd)
{
	struct lkmdbg_event_record events[EVENT_READ_BATCH];

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
		if (nread > 0) {
			if (nread % (ssize_t)sizeof(events[0]) != 0) {
				fprintf(stderr, "short event drain read: %zd\n",
					nread);
				return -1;
			}
			continue;
		}
		if (nread < 0) {
			fprintf(stderr, "drain event failed: %s\n",
				strerror(errno));
			return -1;
		}
		if (nread == 0)
			return 0;
		fprintf(stderr, "short event drain read: %zd\n", nread);
		return -1;
	}
}

static int read_session_events_timeout(int session_fd,
				       struct lkmdbg_event_record *events_out,
				       size_t max_events,
				       size_t *events_read_out,
				       int timeout_ms)
{
	struct pollfd pfd = {
		.fd = session_fd,
		.events = POLLIN,
	};
	size_t bytes;
	ssize_t nread;

	if (!events_out || !max_events || !events_read_out)
		return -1;

	if (poll(&pfd, 1, timeout_ms) < 0) {
		fprintf(stderr, "event poll failed: %s\n", strerror(errno));
		return -1;
	}

	if (!(pfd.revents & POLLIN))
		return 1;

	bytes = max_events * sizeof(*events_out);
	nread = read(session_fd, events_out, bytes);
	if (nread < 0) {
		fprintf(stderr, "event read failed: %s\n", strerror(errno));
		return -1;
	}
	if (nread == 0) {
		*events_read_out = 0;
		return 1;
	}
	if (nread % (ssize_t)sizeof(*events_out) != 0) {
		fprintf(stderr, "short event read: %zd\n", nread);
		return -1;
	}

	*events_read_out = (size_t)nread / sizeof(*events_out);
	return 0;
}

static int read_session_event_timeout(int session_fd,
				      struct lkmdbg_event_record *event_out,
				      int timeout_ms)
{
	size_t events_read = 0;
	int ret;

	ret = read_session_events_timeout(session_fd, event_out, 1,
					  &events_read, timeout_ms);
	if (ret)
		return ret;
	if (events_read != 1) {
		fprintf(stderr, "unexpected event batch size: %zu\n",
			events_read);
		return -1;
	}

	return 0;
}

static int wait_for_session_event(int session_fd, uint32_t type, uint32_t code,
				  int timeout_ms,
				  struct lkmdbg_event_record *event_out)
{
	int waited = 0;

	while (waited < timeout_ms) {
		struct lkmdbg_event_record event;
		int slice = timeout_ms - waited;
		int ret;

		if (slice > 1000)
			slice = 1000;
		ret = read_session_event_timeout(session_fd, &event, slice);
		if (ret < 0)
			return -1;
		if (ret > 0) {
			waited += slice;
			continue;
		}
		waited += slice;

		if (event.type != type)
			continue;
		if (code && event.code != code)
			continue;

		if (event_out)
			*event_out = event;
		return 0;
	}

	fprintf(stderr, "event wait timed out type=%u code=%u\n", type, code);
	return -ETIMEDOUT;
}

static void print_event_record(const struct lkmdbg_event_record *event,
			       unsigned int index)
{
	printf("event[%u].version=%u\n", index, event->version);
	printf("event[%u].type=%u\n", index, event->type);
	printf("event[%u].size=%u\n", index, event->size);
	printf("event[%u].code=%u\n", index, event->code);
	printf("event[%u].session_id=%" PRIu64 "\n", index,
	       (uint64_t)event->session_id);
	printf("event[%u].seq=%" PRIu64 "\n", index, (uint64_t)event->seq);
	printf("event[%u].tgid=%d\n", index, event->tgid);
	printf("event[%u].tid=%d\n", index, event->tid);
	printf("event[%u].flags=0x%x\n", index, event->flags);
	printf("event[%u].reserved0=%u\n", index, event->reserved0);
	printf("event[%u].value0=%" PRIu64 "\n", index, (uint64_t)event->value0);
	printf("event[%u].value1=%" PRIu64 "\n", index, (uint64_t)event->value1);
}

static int verify_batch_event_read(int session_fd)
{
	struct lkmdbg_event_record events[2];
	size_t events_read = 0;

	if (ioctl(session_fd, LKMDBG_IOC_RESET_SESSION) < 0) {
		fprintf(stderr, "RESET_SESSION failed: %s\n", strerror(errno));
		return -1;
	}

	if (read_session_events_timeout(session_fd, events, 2, &events_read,
					1000) < 0)
		return -1;
	if (events_read != 2) {
		fprintf(stderr, "expected 2 queued session events, got %zu\n",
			events_read);
		return -1;
	}
	if (events[0].type != LKMDBG_EVENT_SESSION_OPENED ||
	    events[1].type != LKMDBG_EVENT_SESSION_RESET) {
		fprintf(stderr,
			"unexpected batch event order types=%u,%u\n",
			events[0].type, events[1].type);
		return -1;
	}
	if (events[1].reserved0 != 0) {
		fprintf(stderr, "unexpected reset reserved0=%u\n",
			events[1].reserved0);
		return -1;
	}

	printf("selftest batched event read ok seq=%" PRIu64 "->%" PRIu64 "\n",
	       (uint64_t)events[0].seq, (uint64_t)events[1].seq);
	return 0;
}

static int expect_stop_state(int session_fd, uint32_t reason,
			     struct lkmdbg_stop_query_request *reply_out)
{
	struct lkmdbg_stop_query_request stop_req;

	memset(&stop_req, 0, sizeof(stop_req));
	if (get_stop_state(session_fd, &stop_req) < 0)
		return -1;

	if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_ACTIVE)) {
		fprintf(stderr, "stop state inactive\n");
		return -1;
	}
	if (reason && stop_req.stop.reason != reason) {
		fprintf(stderr, "stop reason mismatch got=%u expected=%u\n",
			stop_req.stop.reason, reason);
		return -1;
	}

	if (reply_out)
		*reply_out = stop_req;
	return 0;
}

static int query_target_threads(int session_fd, int32_t start_tid,
				struct lkmdbg_thread_entry *entries,
				uint32_t max_entries,
				struct lkmdbg_thread_query_request *reply_out)
{
	struct lkmdbg_thread_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_tid = start_tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_THREADS, &req) < 0) {
		fprintf(stderr, "QUERY_THREADS failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int get_target_regs(int session_fd, pid_t tid,
			   struct lkmdbg_thread_regs_request *reply_out)
{
	struct lkmdbg_thread_regs_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_REGS, &req) < 0) {
		fprintf(stderr, "GET_REGS failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int set_target_regs(int session_fd,
			   const struct lkmdbg_thread_regs_request *req_in)
{
	struct lkmdbg_thread_regs_request req = *req_in;

	if (ioctl(session_fd, LKMDBG_IOC_SET_REGS, &req) < 0) {
		fprintf(stderr, "SET_REGS failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int add_hwpoint_ex(int session_fd, pid_t tid, uint64_t addr,
			  uint32_t type, uint32_t len, uint32_t flags,
			  uint64_t trigger_hit_count, uint32_t action_flags,
			  struct lkmdbg_hwpoint_request *reply_out)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.tid = tid,
		.type = type,
		.len = len,
		.flags = flags,
		.trigger_hit_count = trigger_hit_count,
		.action_flags = action_flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_ADD_HWPOINT, &req) < 0) {
		fprintf(stderr, "ADD_HWPOINT failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int add_hwpoint(int session_fd, pid_t tid, uint64_t addr, uint32_t type,
		       uint32_t len, uint32_t flags,
		       struct lkmdbg_hwpoint_request *reply_out)
{
	return add_hwpoint_ex(session_fd, tid, addr, type, len, flags, 1, 0,
			      reply_out);
}

static int add_hwpoint_expect_errno_ex(int session_fd, pid_t tid, uint64_t addr,
				       uint32_t type, uint32_t len,
				       uint32_t flags, uint64_t trigger_hit_count,
				       uint32_t action_flags,
				       int expected_errno)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.tid = tid,
		.type = type,
		.len = len,
		.flags = flags,
		.trigger_hit_count = trigger_hit_count,
		.action_flags = action_flags,
	};

	errno = 0;
	if (ioctl(session_fd, LKMDBG_IOC_ADD_HWPOINT, &req) == 0) {
		fprintf(stderr,
			"ADD_HWPOINT unexpectedly succeeded addr=0x%" PRIx64 " flags=0x%x\n",
			(uint64_t)addr, flags);
		return -1;
	}

	if (errno != expected_errno) {
		fprintf(stderr,
			"ADD_HWPOINT errno=%d expected=%d addr=0x%" PRIx64 " flags=0x%x\n",
			errno, expected_errno, (uint64_t)addr, flags);
		return -1;
	}

	return 0;
}

static int add_hwpoint_expect_errno(int session_fd, pid_t tid, uint64_t addr,
				    uint32_t type, uint32_t len,
				    uint32_t flags, int expected_errno)
{
	return add_hwpoint_expect_errno_ex(session_fd, tid, addr, type, len,
					   flags, 1, 0, expected_errno);
}

static int remove_hwpoint(int session_fd, uint64_t id)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_HWPOINT, &req) < 0) {
		fprintf(stderr, "REMOVE_HWPOINT failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int rearm_hwpoint(int session_fd, uint64_t id,
			 struct lkmdbg_hwpoint_request *reply_out)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REARM_HWPOINT, &req) < 0) {
		fprintf(stderr, "REARM_HWPOINT failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int query_hwpoints(int session_fd, uint64_t start_id,
			  struct lkmdbg_hwpoint_entry *entries,
			  uint32_t max_entries,
			  struct lkmdbg_hwpoint_query_request *reply_out)
{
	struct lkmdbg_hwpoint_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_id = start_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_HWPOINTS, &req) < 0) {
		fprintf(stderr, "QUERY_HWPOINTS failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int single_step_thread(int session_fd, pid_t tid)
{
	struct lkmdbg_single_step_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SINGLE_STEP, &req) < 0) {
		fprintf(stderr, "SINGLE_STEP failed: %s\n", strerror(errno));
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

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int continue_target(int session_fd, uint64_t stop_cookie,
			   uint32_t timeout_ms, uint32_t flags,
			   struct lkmdbg_continue_request *reply_out)
{
	struct lkmdbg_continue_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.flags = flags,
		.timeout_ms = timeout_ms,
		.stop_cookie = stop_cookie,
	};

	if (ioctl(session_fd, LKMDBG_IOC_CONTINUE_TARGET, &req) < 0) {
		fprintf(stderr, "CONTINUE_TARGET failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int control_target_threads(int session_fd, int thaw, uint32_t timeout_ms,
				  struct lkmdbg_freeze_request *reply_out,
				  int verbose)
{
	struct lkmdbg_freeze_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.timeout_ms = timeout_ms,
	};
	unsigned long cmd =
		thaw ? LKMDBG_IOC_THAW_THREADS : LKMDBG_IOC_FREEZE_THREADS;

	if (ioctl(session_fd, cmd, &req) < 0) {
		fprintf(stderr, "%s failed: %s\n",
			thaw ? "THAW_THREADS" : "FREEZE_THREADS",
			strerror(errno));
		return -1;
	}

	if (verbose) {
		printf("%s total=%u settled=%u parked=%u\n",
		       thaw ? "THAW_THREADS" : "FREEZE_THREADS",
		       req.threads_total, req.threads_settled,
		       req.threads_parked);
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int freeze_target_threads(int session_fd, uint32_t timeout_ms,
				 struct lkmdbg_freeze_request *reply_out,
				 int verbose)
{
	return control_target_threads(session_fd, 0, timeout_ms, reply_out,
				      verbose);
}

static int thaw_target_threads(int session_fd, uint32_t timeout_ms,
			       struct lkmdbg_freeze_request *reply_out,
			       int verbose)
{
	return control_target_threads(session_fd, 1, timeout_ms, reply_out,
				      verbose);
}

static int query_target_pages(int session_fd, uint64_t start_addr,
			      uint64_t length,
			      struct page_query_buffer *buf,
			      struct lkmdbg_page_query_request *reply_out)
{
	struct lkmdbg_page_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.start_addr = start_addr,
		.length = length,
		.entries_addr = (uintptr_t)buf->entries,
		.max_entries = PAGE_QUERY_BATCH,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_PAGES, &req) < 0) {
		fprintf(stderr, "QUERY_PAGES failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int apply_pte_patch(int session_fd, uint64_t addr, uint32_t mode,
			   uint32_t flags, uint64_t raw_pte,
			   struct lkmdbg_pte_patch_request *reply_out)
{
	struct lkmdbg_pte_patch_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.raw_pte = raw_pte,
		.mode = mode,
		.flags = flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_APPLY_PTE_PATCH, &req) < 0) {
		fprintf(stderr, "APPLY_PTE_PATCH failed: %s\n",
			strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int remove_pte_patch(int session_fd, uint64_t id,
			    struct lkmdbg_pte_patch_request *reply_out)
{
	struct lkmdbg_pte_patch_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_PTE_PATCH, &req) < 0) {
		fprintf(stderr, "REMOVE_PTE_PATCH failed: %s\n",
			strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int query_pte_patches(int session_fd, uint64_t start_id,
			     struct lkmdbg_pte_patch_entry *entries,
			     uint32_t max_entries,
			     struct lkmdbg_pte_patch_query_request *reply_out)
{
	struct lkmdbg_pte_patch_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_id = start_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_PTE_PATCHES, &req) < 0) {
		fprintf(stderr, "QUERY_PTE_PATCHES failed: %s\n",
			strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int create_remote_map(int session_fd, uintptr_t remote_addr,
			     uintptr_t local_addr, size_t len, uint32_t prot,
			     uint32_t flags,
			     struct lkmdbg_remote_map_request *reply_out)
{
	struct lkmdbg_remote_map_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.remote_addr = remote_addr,
		.local_addr = local_addr,
		.length = len,
		.prot = prot,
		.flags = flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_CREATE_REMOTE_MAP, &req) < 0) {
		fprintf(stderr, "CREATE_REMOTE_MAP failed: %s\n",
			strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static void *remote_map_wake_thread_main(void *arg)
{
	struct remote_map_wake_ctx *ctx = arg;

	usleep(50000);
	ctx->ret = child_query_nofault_residency(ctx->cmd_fd, ctx->resp_fd);
	return NULL;
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

static int read_maps_line_for_addr(uintptr_t addr, char *buf, size_t buf_size)
{
	FILE *fp;
	char line[512];

	if (!buf || !buf_size)
		return -1;

	fp = fopen("/proc/self/maps", "re");
	if (!fp) {
		fprintf(stderr, "fopen(/proc/self/maps) failed: %s\n",
			strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned long long start = 0;
		unsigned long long end = 0;

		if (sscanf(line, "%llx-%llx", &start, &end) != 2)
			continue;
		if (addr < (uintptr_t)start || addr >= (uintptr_t)end)
			continue;

		snprintf(buf, buf_size, "%s", line);
		fclose(fp);
		return 0;
	}

	fclose(fp);
	fprintf(stderr, "address 0x%" PRIxPTR " not found in /proc/self/maps\n",
		addr);
	return -1;
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

static int xfer_physical_memory(int session_fd, struct lkmdbg_phys_op *ops,
				uint32_t op_count, int write,
				struct lkmdbg_phys_request *reply_out,
				int verbose)
{
	struct lkmdbg_phys_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.ops_addr = (uintptr_t)ops,
		.op_count = op_count,
	};
	unsigned long cmd = write ? LKMDBG_IOC_WRITE_PHYS : LKMDBG_IOC_READ_PHYS;

	if (ioctl(session_fd, cmd, &req) < 0) {
		fprintf(stderr, "%s failed: %s\n",
			write ? "WRITE_PHYS" : "READ_PHYS", strerror(errno));
		return -1;
	}

	if (verbose)
		printf("%s ops_done=%u bytes_done=%" PRIu64 "\n",
		       write ? "WRITE_PHYS" : "READ_PHYS", req.ops_done,
		       (uint64_t)req.bytes_done);

	if (reply_out)
		*reply_out = req;

	return 0;
}

static int read_target_memory_flags(int session_fd, uintptr_t remote_addr,
				    void *buf, size_t len, uint32_t op_flags,
				    uint32_t *bytes_done_out, int verbose);
static int write_target_memory_flags(int session_fd, uintptr_t remote_addr,
				     const void *buf, size_t len,
				     uint32_t op_flags,
				     uint32_t *bytes_done_out, int verbose);
static int read_physical_memory_flags(int session_fd, uint64_t phys_addr,
				      void *buf, size_t len, uint32_t op_flags,
				      uint32_t *bytes_done_out, int verbose);
static int write_physical_memory_flags(int session_fd, uint64_t phys_addr,
				       const void *buf, size_t len,
				       uint32_t op_flags,
				       uint32_t *bytes_done_out, int verbose);

static int read_target_memory(int session_fd, uintptr_t remote_addr, void *buf,
			      size_t len, uint32_t *bytes_done_out,
			      int verbose)
{
	return read_target_memory_flags(session_fd, remote_addr, buf, len, 0,
					bytes_done_out, verbose);
}

static int read_target_memory_flags(int session_fd, uintptr_t remote_addr,
				    void *buf, size_t len, uint32_t op_flags,
				    uint32_t *bytes_done_out, int verbose)
{
	struct lkmdbg_mem_op op = {
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
		.flags = op_flags,
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
	return write_target_memory_flags(session_fd, remote_addr, buf, len, 0,
					 bytes_done_out, verbose);
}

static int write_target_memory_flags(int session_fd, uintptr_t remote_addr,
				     const void *buf, size_t len,
				     uint32_t op_flags,
				     uint32_t *bytes_done_out, int verbose)
{
	struct lkmdbg_mem_op op = {
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
		.flags = op_flags,
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

static int read_physical_memory(int session_fd, uint64_t phys_addr, void *buf,
				size_t len, uint32_t *bytes_done_out,
				int verbose)
{
	return read_physical_memory_flags(session_fd, phys_addr, buf, len, 0,
					  bytes_done_out, verbose);
}

static int read_physical_memory_flags(int session_fd, uint64_t phys_addr,
				      void *buf, size_t len, uint32_t op_flags,
				      uint32_t *bytes_done_out, int verbose)
{
	struct lkmdbg_phys_op op = {
		.phys_addr = phys_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
		.flags = op_flags,
	};
	struct lkmdbg_phys_request req;

	if (xfer_physical_memory(session_fd, &op, 1, 0, &req, verbose) < 0)
		return -1;

	if (bytes_done_out)
		*bytes_done_out = op.bytes_done;

	return req.ops_done == 1 || !op.bytes_done ? 0 : -1;
}

static int write_physical_memory(int session_fd, uint64_t phys_addr,
				 const void *buf, size_t len,
				 uint32_t *bytes_done_out, int verbose)
{
	return write_physical_memory_flags(session_fd, phys_addr, buf, len, 0,
					   bytes_done_out, verbose);
}

static int write_physical_memory_flags(int session_fd, uint64_t phys_addr,
				       const void *buf, size_t len,
				       uint32_t op_flags,
				       uint32_t *bytes_done_out, int verbose)
{
	struct lkmdbg_phys_op op = {
		.phys_addr = phys_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
		.flags = op_flags,
	};
	struct lkmdbg_phys_request req;

	if (xfer_physical_memory(session_fd, &op, 1, 1, &req, verbose) < 0)
		return -1;

	if (bytes_done_out)
		*bytes_done_out = op.bytes_done;

	return req.ops_done == 1 || !op.bytes_done ? 0 : -1;
}

static int query_target_vmas(int session_fd, uint64_t start_addr,
			     struct vma_query_buffer *buf,
			     struct lkmdbg_vma_query_request *reply_out)
{
	struct lkmdbg_vma_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.start_addr = start_addr,
		.entries_addr = (uintptr_t)buf->entries,
		.max_entries = VMA_QUERY_BATCH,
		.names_addr = (uintptr_t)buf->names,
		.names_size = VMA_QUERY_NAMES_SIZE,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_VMAS, &req) < 0) {
		fprintf(stderr, "QUERY_VMAS failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static int query_target_images(int session_fd, uint64_t start_addr,
			       struct image_query_buffer *buf,
			       struct lkmdbg_image_query_request *reply_out)
{
	struct lkmdbg_image_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.start_addr = start_addr,
		.entries_addr = (uintptr_t)buf->entries,
		.max_entries = IMAGE_QUERY_BATCH,
		.names_addr = (uintptr_t)buf->names,
		.names_size = IMAGE_QUERY_NAMES_SIZE,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_IMAGES, &req) < 0) {
		fprintf(stderr, "QUERY_IMAGES failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

static const char *vma_name_ptr(const struct lkmdbg_vma_query_request *reply,
				const struct lkmdbg_vma_entry *entry,
				const struct vma_query_buffer *buf)
{
	if (!entry->name_size)
		return "";

	if ((uint64_t)entry->name_offset + entry->name_size > reply->names_used)
		return "";

	return buf->names + entry->name_offset;
}

static const char *image_name_ptr(const struct lkmdbg_image_query_request *reply,
				  const struct lkmdbg_image_entry *entry,
				  const struct image_query_buffer *buf)
{
	if (!entry->name_size)
		return "";

	if ((uint64_t)entry->name_offset + entry->name_size > reply->names_used)
		return "";

	return buf->names + entry->name_offset;
}

static int lookup_target_vma(int session_fd, uintptr_t remote_addr,
			     struct vma_query_buffer *buf,
			     struct lkmdbg_vma_entry *entry_out,
			     char *name_out, size_t name_out_size)
{
	struct lkmdbg_vma_query_request reply;
	unsigned int i;

	if (query_target_vmas(session_fd, remote_addr, buf, &reply) < 0)
		return -1;

	for (i = 0; i < reply.entries_filled; i++) {
		const struct lkmdbg_vma_entry *entry = &buf->entries[i];
		const char *name;

		if (remote_addr < entry->start_addr || remote_addr >= entry->end_addr)
			continue;

		*entry_out = *entry;
		if (!name_out || !name_out_size)
			return 0;

		name = vma_name_ptr(&reply, entry, buf);
		snprintf(name_out, name_out_size, "%s", name);
		return 0;
	}

	fprintf(stderr, "address 0x%" PRIxPTR " not covered by returned VMA batch\n",
		remote_addr);
	return -1;
}

static int lookup_target_page(int session_fd, uintptr_t remote_addr,
			      uint64_t length,
			      struct page_query_buffer *buf,
			      struct lkmdbg_page_entry *entry_out)
{
	struct lkmdbg_page_query_request reply;
	unsigned int i;

	if (query_target_pages(session_fd, remote_addr, length, buf, &reply) < 0)
		return -1;

	for (i = 0; i < reply.entries_filled; i++) {
		if (buf->entries[i].page_addr !=
		    (uint64_t)(remote_addr & ~(uintptr_t)(getpagesize() - 1)))
			continue;
		*entry_out = buf->entries[i];
		return 0;
	}

	fprintf(stderr, "address 0x%" PRIxPTR " not covered by returned page batch\n",
		remote_addr);
	return -1;
}

static int verify_vma_iteration(int session_fd, const struct child_info *info,
				struct vma_query_buffer *buf)
{
	uint64_t cursor = 0;
	uint64_t prev_end = 0;
	unsigned int passes = 0;
	unsigned int total = 0;
	int saw_force_read = 0;
	int saw_file = 0;

	for (;;) {
		struct lkmdbg_vma_query_request reply;
		unsigned int i;

		if (query_target_vmas(session_fd, cursor, buf, &reply) < 0)
			return -1;

		if (!reply.entries_filled && !reply.done) {
			fprintf(stderr, "QUERY_VMAS returned no entries without done\n");
			return -1;
		}

		for (i = 0; i < reply.entries_filled; i++) {
			const struct lkmdbg_vma_entry *entry = &buf->entries[i];

			if (entry->start_addr >= entry->end_addr) {
				fprintf(stderr,
					"invalid VMA range start=0x%" PRIx64 " end=0x%" PRIx64 "\n",
					(uint64_t)entry->start_addr,
					(uint64_t)entry->end_addr);
				return -1;
			}

			if (total == 0 && i == 0) {
				prev_end = entry->end_addr;
			} else if (prev_end > entry->start_addr) {
				fprintf(stderr, "non-monotonic VMA iteration output\n");
				return -1;
			}

			if (i > 0 && buf->entries[i - 1].end_addr > entry->start_addr) {
				fprintf(stderr, "overlapping VMA iteration output\n");
				return -1;
			}

			if (info->force_read_addr >= entry->start_addr &&
			    info->force_read_addr < entry->end_addr)
				saw_force_read = 1;
			if (info->file_addr >= entry->start_addr &&
			    info->file_addr < entry->end_addr)
				saw_file = 1;

			prev_end = entry->end_addr;
		}

		total += reply.entries_filled;
		passes++;
		if (reply.done)
			break;

		if (reply.next_addr <= cursor) {
			fprintf(stderr,
				"QUERY_VMAS cursor did not advance old=0x%" PRIx64 " new=0x%" PRIx64 "\n",
				(uint64_t)cursor, (uint64_t)reply.next_addr);
			return -1;
		}
		cursor = reply.next_addr;
		if (passes > 1024) {
			fprintf(stderr, "QUERY_VMAS iteration exceeded sane pass count\n");
			return -1;
		}
	}

	if (!saw_force_read || !saw_file || total < 4) {
		fprintf(stderr,
			"QUERY_VMAS iteration missing targets force=%d file=%d total=%u\n",
			saw_force_read, saw_file, total);
		return -1;
	}

	printf("selftest vma iteration ok passes=%u total=%u\n", passes, total);
	return 0;
}

static int verify_vma_query(int session_fd, const struct child_info *info)
{
	struct vma_query_buffer buf = { 0 };
	struct lkmdbg_vma_entry entry;
	char name[256];
	int ret = -1;

	buf.entries = calloc(VMA_QUERY_BATCH, sizeof(*buf.entries));
	buf.names = calloc(1, VMA_QUERY_NAMES_SIZE);
	if (!buf.entries || !buf.names) {
		fprintf(stderr, "VMA query allocation failed\n");
		goto out;
	}

	memset(&entry, 0, sizeof(entry));
	memset(name, 0, sizeof(name));
	if (lookup_target_vma(session_fd, info->force_read_addr, &buf, &entry, name,
			      sizeof(name)) < 0)
		goto out;

	if (!(entry.flags & LKMDBG_VMA_FLAG_ANON) ||
	    (entry.prot & (LKMDBG_VMA_PROT_READ | LKMDBG_VMA_PROT_WRITE |
			   LKMDBG_VMA_PROT_EXEC)) != 0 ||
	    !(entry.prot & (LKMDBG_VMA_PROT_MAYREAD |
			    LKMDBG_VMA_PROT_MAYWRITE))) {
		fprintf(stderr,
			"force_read VMA flags/prot mismatch flags=0x%x prot=0x%x\n",
			entry.flags, entry.prot);
		goto out;
	}

	memset(&entry, 0, sizeof(entry));
	memset(name, 0, sizeof(name));
	if (lookup_target_vma(session_fd, info->file_addr, &buf, &entry, name,
			      sizeof(name)) < 0)
		goto out;

	if (!(entry.flags & LKMDBG_VMA_FLAG_FILE) ||
	    entry.inode != info->file_inode ||
	    entry.dev_major != info->file_dev_major ||
	    entry.dev_minor != info->file_dev_minor ||
	    !entry.name_size || strstr(name, "lkmdbg-vma") == NULL) {
		fprintf(stderr,
			"file VMA mismatch flags=0x%x inode=%" PRIu64 " dev=%u:%u name=%s\n",
			entry.flags, (uint64_t)entry.inode, entry.dev_major,
			entry.dev_minor,
			name);
		goto out;
	}

	if (verify_vma_iteration(session_fd, info, &buf) < 0)
		goto out;

	printf("selftest vma query ok file=%s inode=%" PRIu64 " dev=%u:%u\n",
	       name, (uint64_t)info->file_inode, info->file_dev_major,
	       info->file_dev_minor);
	ret = 0;

out:
	free(buf.names);
	free(buf.entries);
	return ret;
}

static int verify_image_query(int session_fd, const struct child_info *info)
{
	struct image_query_buffer buf = { 0 };
	uint64_t cursor = 0;
	uint64_t prev_end = 0;
	unsigned int passes = 0;
	unsigned int total = 0;
	int saw_file = 0;
	int saw_main = 0;
	int ret = -1;

	buf.entries = calloc(IMAGE_QUERY_BATCH, sizeof(*buf.entries));
	buf.names = calloc(1, IMAGE_QUERY_NAMES_SIZE);
	if (!buf.entries || !buf.names) {
		fprintf(stderr, "image query allocation failed\n");
		goto out;
	}

	for (;;) {
		struct lkmdbg_image_query_request reply;
		unsigned int i;

		if (query_target_images(session_fd, cursor, &buf, &reply) < 0)
			goto out;

		if (!reply.entries_filled && !reply.done) {
			fprintf(stderr,
				"QUERY_IMAGES returned no entries without done\n");
			goto out;
		}

		for (i = 0; i < reply.entries_filled; i++) {
			const struct lkmdbg_image_entry *entry = &buf.entries[i];
			const char *name = image_name_ptr(&reply, entry, &buf);

			if (entry->start_addr >= entry->end_addr) {
				fprintf(stderr,
					"invalid image range start=0x%" PRIx64 " end=0x%" PRIx64 "\n",
					(uint64_t)entry->start_addr,
					(uint64_t)entry->end_addr);
				goto out;
			}

			if (total == 0 && i == 0)
				prev_end = entry->end_addr;
			else if (prev_end > entry->start_addr) {
				fprintf(stderr, "non-monotonic image iteration output\n");
				goto out;
			}

			if (!(entry->flags & LKMDBG_IMAGE_FLAG_FILE) ||
			    !entry->name_size) {
				fprintf(stderr,
					"image entry missing file backing flags=0x%x name_size=%u\n",
					entry->flags, entry->name_size);
				goto out;
			}

			if (info->file_addr >= entry->start_addr &&
			    info->file_addr < entry->end_addr) {
				if (entry->inode != info->file_inode ||
				    entry->dev_major != info->file_dev_major ||
				    entry->dev_minor != info->file_dev_minor ||
				    strstr(name, "lkmdbg-vma") == NULL) {
					fprintf(stderr,
						"file image mismatch inode=%" PRIu64 " dev=%u:%u name=%s\n",
						(uint64_t)entry->inode,
						entry->dev_major,
						entry->dev_minor, name);
					goto out;
				}
				saw_file = 1;
			}

			if ((entry->flags & LKMDBG_IMAGE_FLAG_MAIN_EXE) &&
			    (entry->prot & LKMDBG_VMA_PROT_EXEC))
				saw_main = 1;

			prev_end = entry->end_addr;
		}

		total += reply.entries_filled;
		passes++;
		if (reply.done)
			break;

		if (reply.next_addr <= cursor) {
			fprintf(stderr,
				"QUERY_IMAGES cursor did not advance old=0x%" PRIx64 " new=0x%" PRIx64 "\n",
				(uint64_t)cursor, (uint64_t)reply.next_addr);
			goto out;
		}
		cursor = reply.next_addr;
		if (passes > 1024) {
			fprintf(stderr, "QUERY_IMAGES iteration exceeded sane pass count\n");
			goto out;
		}
	}

	if (!saw_file || !saw_main || !total) {
		fprintf(stderr,
			"QUERY_IMAGES missing targets file=%d main=%d total=%u\n",
			saw_file, saw_main, total);
		goto out;
	}

	printf("selftest image query ok passes=%u total=%u\n", passes, total);
	ret = 0;

out:
	free(buf.names);
	free(buf.entries);
	return ret;
}

static int verify_page_query(int session_fd, const struct child_info *info)
{
	struct page_query_buffer buf = { 0 };
	struct lkmdbg_page_entry entry;
	uint32_t expected_page_shift;
	uint32_t nofault_flags = 0;
	uint32_t force_read_flags = 0;
	uint32_t force_write_flags = 0;
	int ret = -1;

	buf.entries = calloc(PAGE_QUERY_BATCH, sizeof(*buf.entries));
	if (!buf.entries) {
		fprintf(stderr, "page query allocation failed\n");
		return -1;
	}
	expected_page_shift = (uint32_t)__builtin_ctz(info->page_size);

	memset(&entry, 0, sizeof(entry));
	if (lookup_target_page(session_fd, info->nofault_addr, info->page_size,
			       &buf, &entry) < 0)
		goto out;
	if (!(entry.flags & LKMDBG_PAGE_FLAG_MAPPED) ||
	    (entry.flags & LKMDBG_PAGE_FLAG_PRESENT) ||
	    (entry.flags & (LKMDBG_PAGE_FLAG_NOFAULT_READ |
			    LKMDBG_PAGE_FLAG_NOFAULT_WRITE |
			    LKMDBG_PAGE_FLAG_FORCE_READ |
			    LKMDBG_PAGE_FLAG_FORCE_WRITE))) {
		fprintf(stderr, "nofault page flags mismatch flags=0x%x\n",
			entry.flags);
		goto out;
	}
	if ((entry.flags & LKMDBG_PAGE_FLAG_PT_PRESENT) ||
	    entry.pt_level != LKMDBG_PAGE_LEVEL_NONE ||
	    entry.phys_addr != 0 || entry.page_shift != 0) {
		fprintf(stderr,
			"nofault page table state mismatch level=%u shift=%u phys=0x%" PRIx64 " flags=0x%x\n",
			entry.pt_level, entry.page_shift,
			(uint64_t)entry.phys_addr, entry.flags);
		goto out;
	}
	nofault_flags = entry.flags;

	memset(&entry, 0, sizeof(entry));
	if (lookup_target_page(session_fd, info->force_read_addr, info->page_size,
			       &buf, &entry) < 0)
		goto out;
	if (!(entry.flags & LKMDBG_PAGE_FLAG_MAPPED) ||
	    !(entry.flags & LKMDBG_PAGE_FLAG_PRESENT) ||
	    !(entry.flags & LKMDBG_PAGE_FLAG_FORCE_READ) ||
	    (entry.flags & (LKMDBG_PAGE_FLAG_NOFAULT_READ |
			    LKMDBG_PAGE_FLAG_NOFAULT_WRITE))) {
		fprintf(stderr, "force-read page flags mismatch flags=0x%x\n",
			entry.flags);
		goto out;
	}
	if (!(entry.flags & LKMDBG_PAGE_FLAG_PT_PRESENT) ||
	    entry.pt_level != LKMDBG_PAGE_LEVEL_PTE ||
	    entry.page_shift != expected_page_shift ||
	    entry.phys_addr == 0 || entry.pt_entry_raw == 0) {
		fprintf(stderr,
			"force-read page table mismatch level=%u shift=%u phys=0x%" PRIx64 " entry=0x%" PRIx64 " flags=0x%x\n",
			entry.pt_level, entry.page_shift,
			(uint64_t)entry.phys_addr,
			(uint64_t)entry.pt_entry_raw, entry.flags);
		goto out;
	}
	force_read_flags = entry.flags;

	memset(&entry, 0, sizeof(entry));
	if (lookup_target_page(session_fd, info->force_write_addr, info->page_size,
			       &buf, &entry) < 0)
		goto out;
	if (!(entry.flags & LKMDBG_PAGE_FLAG_MAPPED) ||
	    !(entry.flags & LKMDBG_PAGE_FLAG_PRESENT) ||
	    !(entry.flags & LKMDBG_PAGE_FLAG_NOFAULT_READ) ||
	    !(entry.flags & LKMDBG_PAGE_FLAG_FORCE_WRITE) ||
	    (entry.flags & LKMDBG_PAGE_FLAG_NOFAULT_WRITE)) {
		fprintf(stderr, "force-write page flags mismatch flags=0x%x\n",
			entry.flags);
		goto out;
	}
	if (!(entry.flags & LKMDBG_PAGE_FLAG_PT_PRESENT) ||
	    entry.pt_level != LKMDBG_PAGE_LEVEL_PTE ||
	    entry.page_shift != expected_page_shift ||
	    entry.phys_addr == 0 || entry.pt_entry_raw == 0) {
		fprintf(stderr,
			"force-write page table mismatch level=%u shift=%u phys=0x%" PRIx64 " entry=0x%" PRIx64 " flags=0x%x\n",
			entry.pt_level, entry.page_shift,
			(uint64_t)entry.phys_addr,
			(uint64_t)entry.pt_entry_raw, entry.flags);
		goto out;
	}
	force_write_flags = entry.flags;

	memset(&entry, 0, sizeof(entry));
	if (lookup_target_page(session_fd, info->file_addr, info->page_size, &buf,
			       &entry) < 0)
		goto out;
	if (!(entry.flags & LKMDBG_PAGE_FLAG_FILE) ||
	    entry.inode != info->file_inode ||
	    entry.dev_major != info->file_dev_major ||
	    entry.dev_minor != info->file_dev_minor) {
		fprintf(stderr,
			"file page metadata mismatch flags=0x%x inode=%" PRIu64 " dev=%u:%u\n",
			entry.flags, (uint64_t)entry.inode, entry.dev_major,
			entry.dev_minor);
		goto out;
	}
	if (entry.flags & LKMDBG_PAGE_FLAG_PT_PRESENT) {
		if (entry.pt_level == LKMDBG_PAGE_LEVEL_NONE ||
		    entry.phys_addr == 0 || entry.pt_entry_raw == 0) {
			fprintf(stderr,
				"file page table mismatch level=%u phys=0x%" PRIx64 " entry=0x%" PRIx64 " flags=0x%x\n",
				entry.pt_level, (uint64_t)entry.phys_addr,
				(uint64_t)entry.pt_entry_raw, entry.flags);
			goto out;
		}
	} else if (entry.pt_level != LKMDBG_PAGE_LEVEL_NONE ||
		   entry.phys_addr != 0 || entry.pt_entry_raw != 0) {
		fprintf(stderr,
			"file page table absence mismatch level=%u phys=0x%" PRIx64 " entry=0x%" PRIx64 " flags=0x%x\n",
			entry.pt_level, (uint64_t)entry.phys_addr,
			(uint64_t)entry.pt_entry_raw, entry.flags);
		goto out;
	}

	printf("selftest page query ok nofault=0x%x force_read=0x%x force_write=0x%x\n",
	       nofault_flags, force_read_flags, force_write_flags);
	ret = 0;

out:
	free(buf.entries);
	return ret;
}

static int verify_pte_patch_api(int session_fd, const struct child_info *info)
{
	struct lkmdbg_pte_patch_request apply_reply;
	struct lkmdbg_pte_patch_request remove_reply;
	struct lkmdbg_pte_patch_entry entries[8];
	struct lkmdbg_pte_patch_query_request query_reply;
	uint32_t bytes_done = 0;
	unsigned char value = 0;
	size_t i;
	int found = 0;

	memset(&apply_reply, 0, sizeof(apply_reply));
	memset(&remove_reply, 0, sizeof(remove_reply));
	memset(entries, 0, sizeof(entries));
	memset(&query_reply, 0, sizeof(query_reply));

	if (read_target_memory(session_fd, info->basic_addr, &value, sizeof(value),
			       &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(value)) {
		fprintf(stderr, "baseline PTE patch read failed bytes_done=%u\n",
			bytes_done);
		return -1;
	}

	if (apply_pte_patch(session_fd, info->basic_addr,
			    LKMDBG_PTE_MODE_PROTNONE, 0, 0,
			    &apply_reply) < 0)
		return -1;

	if (!apply_reply.id ||
	    !(apply_reply.state & LKMDBG_PTE_PATCH_STATE_ACTIVE) ||
	    apply_reply.expected_pte == apply_reply.baseline_pte) {
		fprintf(stderr,
			"bad PTE patch apply reply id=%" PRIu64 " state=0x%x baseline=0x%" PRIx64 " expected=0x%" PRIx64 "\n",
			(uint64_t)apply_reply.id, apply_reply.state,
			(uint64_t)apply_reply.baseline_pte,
			(uint64_t)apply_reply.expected_pte);
		goto fail_remove;
	}

	if (query_pte_patches(session_fd, 0, entries,
			      (uint32_t)(sizeof(entries) / sizeof(entries[0])),
			      &query_reply) < 0)
		goto fail_remove;

	for (i = 0; i < query_reply.entries_filled; i++) {
		if (entries[i].id != apply_reply.id)
			continue;
		if (entries[i].page_addr !=
		    (info->basic_addr & ~(uintptr_t)(info->page_size - 1))) {
			fprintf(stderr,
				"bad PTE patch query state=0x%x page=0x%" PRIx64 "\n",
				entries[i].state, (uint64_t)entries[i].page_addr);
			goto fail_remove;
		}
		found = 1;
		break;
	}
	if (!found) {
		fprintf(stderr, "PTE patch not visible in query after apply\n");
		goto fail_remove;
	}

	bytes_done = 0;
	if (remove_pte_patch(session_fd, apply_reply.id, &remove_reply) < 0)
		return -1;

	bytes_done = 0;
	if (read_target_memory(session_fd, info->basic_addr, &value, sizeof(value),
			       &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(value)) {
		fprintf(stderr, "restored PTE read failed bytes_done=%u\n",
			bytes_done);
		return -1;
	}

	printf("selftest pte patch ok id=%" PRIu64 " baseline=0x%" PRIx64 " expected=0x%" PRIx64 "\n",
	       (uint64_t)apply_reply.id, (uint64_t)apply_reply.baseline_pte,
	       (uint64_t)apply_reply.expected_pte);
	return 0;

fail_remove:
	remove_pte_patch(session_fd, apply_reply.id, &remove_reply);
	return -1;
}

static int verify_physical_memory_api(int session_fd, const struct child_info *info)
{
	struct page_query_buffer buf = { 0 };
	struct lkmdbg_page_entry entry;
	uint32_t bytes_done = 0;
	uint8_t virt_before;
	uint8_t via_vaddr;
	uint8_t phys_before;
	uint8_t phys_after;
	uint8_t phys_new;
	int ret = -1;

	buf.entries = calloc(PAGE_QUERY_BATCH, sizeof(*buf.entries));
	if (!buf.entries) {
		fprintf(stderr, "physical query allocation failed\n");
		return -1;
	}

	memset(&entry, 0, sizeof(entry));
	if (lookup_target_page(session_fd, info->basic_addr, info->page_size, &buf,
			       &entry) < 0)
		goto out;
	if (!(entry.flags & LKMDBG_PAGE_FLAG_PT_PRESENT) || !entry.phys_addr) {
		fprintf(stderr,
			"basic page missing physical mapping flags=0x%x phys=0x%" PRIx64 "\n",
			entry.flags, (uint64_t)entry.phys_addr);
		goto out;
	}

	if (read_target_memory(session_fd, info->basic_addr, &virt_before,
			       sizeof(virt_before), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(virt_before)) {
		fprintf(stderr, "baseline virtual read failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	bytes_done = 0;
	if (read_physical_memory(session_fd,
				 entry.phys_addr +
					 (info->basic_addr & (info->page_size - 1)),
				 &phys_before, sizeof(phys_before), &bytes_done,
				 0) < 0 ||
	    bytes_done != sizeof(phys_before)) {
		fprintf(stderr, "baseline physical read failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}
	if (phys_before != virt_before) {
		fprintf(stderr, "physical/virtual mismatch before phys=0x%02x virt=0x%02x\n",
			phys_before, virt_before);
		goto out;
	}

	bytes_done = 0;
	if (read_physical_memory_flags(session_fd, info->basic_addr, &via_vaddr,
				       sizeof(via_vaddr),
				       LKMDBG_PHYS_OP_FLAG_TARGET_VADDR,
				       &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(via_vaddr)) {
		fprintf(stderr,
			"target-vaddr physical read failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}
	if (via_vaddr != virt_before) {
		fprintf(stderr,
			"target-vaddr physical mismatch got=0x%02x expected=0x%02x\n",
			via_vaddr, virt_before);
		goto out;
	}

	phys_new = (uint8_t)(phys_before ^ 0x5aU);
	bytes_done = 0;
	if (write_physical_memory_flags(session_fd, info->basic_addr, &phys_new,
					sizeof(phys_new),
					LKMDBG_PHYS_OP_FLAG_TARGET_VADDR,
					&bytes_done, 0) < 0 ||
	    bytes_done != sizeof(phys_new)) {
		fprintf(stderr, "target-vaddr physical write failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	bytes_done = 0;
	if (read_target_memory(session_fd, info->basic_addr, &phys_after,
			       sizeof(phys_after), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(phys_after)) {
		fprintf(stderr, "post-write virtual read failed bytes_done=%u\n",
			bytes_done);
		goto restore;
	}
	if (phys_after != phys_new) {
		fprintf(stderr, "physical write not reflected virt=0x%02x expected=0x%02x\n",
			phys_after, phys_new);
		goto restore;
	}

	ret = 0;
	printf("selftest physical memory ok phys=0x%" PRIx64 " byte=0x%02x\n",
	       (uint64_t)(entry.phys_addr +
			  (info->basic_addr & (info->page_size - 1))),
	       phys_new);

restore:
	bytes_done = 0;
	write_physical_memory_flags(session_fd, info->basic_addr, &phys_before,
				    sizeof(phys_before),
				    LKMDBG_PHYS_OP_FLAG_TARGET_VADDR,
				    &bytes_done, 0);
out:
	free(buf.entries);
	return ret;
}

static int verify_remote_map(int session_fd, const struct child_info *info,
			     int cmd_fd, int resp_fd)
{
	struct lkmdbg_remote_map_request reply;
	struct lkmdbg_remote_map_request ro_reply;
	struct lkmdbg_remote_map_request inject_reply;
	struct lkmdbg_remote_map_request stealth_reply;
	unsigned char *view = MAP_FAILED;
	unsigned char *ro_view = MAP_FAILED;
	unsigned char *local_map = MAP_FAILED;
	unsigned char *stealth_view = MAP_FAILED;
	unsigned char *verify_buf = NULL;
	unsigned char *expected_buf = NULL;
	struct remote_map_wake_ctx wake_ctx;
	pthread_t wake_thread;
	char maps_before[512];
	char maps_after[512];
	uint32_t bytes_done = 0;
	size_t test_len;
	int local_fd = -1;
	int ret = -1;
	int restore_needed = 0;
	int wake_thread_started = 0;

	memset(&reply, 0, sizeof(reply));
	reply.map_fd = -1;
	memset(&ro_reply, 0, sizeof(ro_reply));
	ro_reply.map_fd = -1;
	memset(&inject_reply, 0, sizeof(inject_reply));
	inject_reply.map_fd = -1;
	memset(&stealth_reply, 0, sizeof(stealth_reply));
	stealth_reply.map_fd = -1;

	test_len = info->page_size * 2U;
	if (test_len > info->large_len)
		test_len = info->large_len;

	if (create_remote_map(session_fd, info->large_addr, 0, test_len,
			      LKMDBG_REMOTE_MAP_PROT_READ |
				      LKMDBG_REMOTE_MAP_PROT_WRITE,
			      0,
			      &reply) < 0)
		return -1;

	view = mmap(NULL, reply.mapped_length, PROT_READ | PROT_WRITE, MAP_SHARED,
		    reply.map_fd, 0);
	if (view == MAP_FAILED) {
		fprintf(stderr, "remote map mmap failed: %s\n",
			strerror(errno));
		goto out;
	}

	verify_buf = malloc(test_len);
	expected_buf = malloc(64);
	if (!verify_buf || !expected_buf) {
		fprintf(stderr, "remote map allocation failed\n");
		goto out;
	}

	if (verify_pattern(view, test_len, 1) != 0) {
		fprintf(stderr, "remote map initial view mismatch\n");
		goto out;
	}

	fill_pattern(expected_buf, 64, 23);
	memcpy(view + 128, expected_buf, 64);
	restore_needed = 1;

	memset(verify_buf, 0, test_len);
	if (read_target_memory(session_fd, info->large_addr, verify_buf, test_len,
			       &bytes_done, 0) < 0 ||
	    bytes_done != test_len) {
		fprintf(stderr,
			"remote map target readback failed bytes_done=%u expected=%zu\n",
			bytes_done, test_len);
		goto out;
	}

	if (memcmp(verify_buf + 128, expected_buf, 64) != 0) {
		fprintf(stderr, "remote map write-through verification failed\n");
		goto out;
	}

	stealth_view = mmap(NULL, test_len, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stealth_view == MAP_FAILED) {
		fprintf(stderr, "stealth local map mmap failed: %s\n",
			strerror(errno));
		goto out;
	}
	memset(stealth_view, 0x3C, test_len);
	if (read_maps_line_for_addr((uintptr_t)stealth_view, maps_before,
				    sizeof(maps_before)) < 0)
		goto out;

	if (create_remote_map(session_fd, info->large_addr,
			      (uintptr_t)stealth_view, test_len,
			      LKMDBG_REMOTE_MAP_PROT_READ |
				      LKMDBG_REMOTE_MAP_PROT_WRITE,
			      LKMDBG_REMOTE_MAP_FLAG_STEALTH_LOCAL,
			      &stealth_reply) < 0)
		goto out;

	if (stealth_reply.map_fd != -1 ||
	    stealth_reply.mapped_length != reply.mapped_length) {
		fprintf(stderr,
			"stealth remote map reply mismatch len=%" PRIu64 " fd=%d\n",
			(uint64_t)stealth_reply.mapped_length, stealth_reply.map_fd);
		goto out;
	}

	if (read_maps_line_for_addr((uintptr_t)stealth_view, maps_after,
				    sizeof(maps_after)) < 0)
		goto out;
	if (strcmp(maps_before, maps_after) != 0) {
		fprintf(stderr, "stealth remote map changed /proc/self/maps entry\n");
		goto out;
	}

	memset(verify_buf, 0, test_len);
	if (read_target_memory(session_fd, info->large_addr, verify_buf, test_len,
			       &bytes_done, 0) < 0 ||
	    bytes_done != test_len) {
		fprintf(stderr,
			"stealth remote map target snapshot failed bytes_done=%u expected=%zu\n",
			bytes_done, test_len);
		goto out;
	}
	if (memcmp(stealth_view, verify_buf, test_len) != 0) {
		fprintf(stderr, "stealth remote map initial view mismatch\n");
		goto out;
	}

	fill_pattern(expected_buf, 64, 41);
	memcpy(stealth_view + 256, expected_buf, 64);
	memset(verify_buf, 0, test_len);
	if (read_target_memory(session_fd, info->large_addr, verify_buf, test_len,
			       &bytes_done, 0) < 0 ||
	    bytes_done != test_len) {
		fprintf(stderr,
			"stealth remote map target readback failed bytes_done=%u expected=%zu\n",
			bytes_done, test_len);
		goto out;
	}
	if (memcmp(verify_buf + 256, expected_buf, 64) != 0) {
		fprintf(stderr, "stealth remote map write-through failed\n");
		goto out;
	}

	if (create_remote_map(session_fd, info->large_addr, 0, test_len,
			      LKMDBG_REMOTE_MAP_PROT_READ, 0,
			      &ro_reply) < 0)
		goto out;

	ro_view = mmap(NULL, ro_reply.mapped_length, PROT_READ, MAP_SHARED,
		       ro_reply.map_fd, 0);
	if (ro_view == MAP_FAILED) {
		fprintf(stderr, "remote ro map mmap failed: %s\n",
			strerror(errno));
		goto out;
	}

	errno = 0;
	if (mprotect(ro_view, ro_reply.mapped_length,
		     PROT_READ | PROT_WRITE) == 0 || errno != EACCES) {
		fprintf(stderr,
			"remote ro map unexpectedly upgraded via mprotect errno=%d\n",
			errno);
		goto out;
	}

	local_fd = memfd_create("lkmdbg-rmap-local",
				MFD_CLOEXEC | MFD_ALLOW_SEALING |
					MFD_NOEXEC_SEAL);
	if (local_fd < 0) {
		fprintf(stderr, "remote inject memfd_create failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (ftruncate(local_fd, (off_t)info->page_size) < 0) {
		fprintf(stderr, "remote inject ftruncate failed: %s\n",
			strerror(errno));
		goto out;
	}
	local_map = mmap(NULL, info->page_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, local_fd, 0);
	if (local_map == MAP_FAILED) {
		fprintf(stderr, "remote inject local mmap failed: %s\n",
			strerror(errno));
		goto out;
	}
	fill_pattern(local_map, info->page_size, 31);

	memset(&wake_ctx, 0, sizeof(wake_ctx));
	wake_ctx.cmd_fd = cmd_fd;
	wake_ctx.resp_fd = resp_fd;
	wake_ctx.ret = -1;
	if (pthread_create(&wake_thread, NULL, remote_map_wake_thread_main,
			   &wake_ctx) != 0) {
		fprintf(stderr, "remote inject wake thread create failed\n");
		goto out;
	}
	wake_thread_started = 1;

	if (create_remote_map(session_fd, 0, (uintptr_t)local_map,
			      info->page_size,
			      LKMDBG_REMOTE_MAP_PROT_READ |
				      LKMDBG_REMOTE_MAP_PROT_WRITE,
			      LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET,
			      &inject_reply) < 0)
		goto out;

	if (pthread_join(wake_thread, NULL) != 0) {
		fprintf(stderr, "remote inject wake thread join failed\n");
		wake_thread_started = 0;
		goto out;
	}
	wake_thread_started = 0;
	if (wake_ctx.ret < 0) {
		fprintf(stderr, "remote inject wake helper failed\n");
		goto out;
	}

	if (inject_reply.map_fd != -1 || !inject_reply.remote_addr ||
	    inject_reply.mapped_length != info->page_size) {
		fprintf(stderr,
			"remote inject reply mismatch addr=0x%" PRIx64 " len=%" PRIu64 " fd=%d\n",
			(uint64_t)inject_reply.remote_addr,
			(uint64_t)inject_reply.mapped_length,
			inject_reply.map_fd);
		goto out;
	}

	memset(verify_buf, 0, info->page_size);
	if (child_read_remote_range(cmd_fd, resp_fd,
				    (uintptr_t)inject_reply.remote_addr,
				    verify_buf, info->page_size) < 0 ||
	    memcmp(verify_buf, local_map, info->page_size) != 0) {
		fprintf(stderr, "remote inject child readback mismatch\n");
		goto out;
	}

	if (child_fill_remote_range(cmd_fd, resp_fd,
				    (uintptr_t)inject_reply.remote_addr + 64,
				    64, 0xA5) < 0) {
		fprintf(stderr, "remote inject child write-through failed\n");
		goto out;
	}

	memset(expected_buf, 0xA5, 64);
	if (memcmp(local_map + 64, expected_buf, 64) != 0) {
		fprintf(stderr, "remote inject local page did not reflect target write\n");
		goto out;
	}

	printf("selftest remote map ok export_len=%" PRIu64 " fd=%d inject_addr=0x%" PRIx64 " stealth_len=%" PRIu64 "\n",
	       (uint64_t)reply.mapped_length, reply.map_fd,
	       (uint64_t)inject_reply.remote_addr,
	       (uint64_t)stealth_reply.mapped_length);
	ret = 0;

out:
	if (wake_thread_started)
		pthread_join(wake_thread, NULL);
	if (restore_needed && view != MAP_FAILED) {
		size_t i;

		for (i = 0; i < 64; i++)
			view[128 + i] = pattern_byte(128 + i, 1);
	}
	if (ro_view != MAP_FAILED)
		munmap(ro_view, ro_reply.mapped_length);
	if (ro_reply.map_fd >= 0)
		close(ro_reply.map_fd);
	if (local_map != MAP_FAILED)
		munmap(local_map, info->page_size);
	if (local_fd >= 0)
		close(local_fd);
	if (stealth_view != MAP_FAILED)
		munmap(stealth_view, test_len);
	if (view != MAP_FAILED)
		munmap(view, reply.mapped_length);
	if (reply.map_fd >= 0)
		close(reply.map_fd);
	free(expected_buf);
	free(verify_buf);
	return ret;
}

static void print_vma_prot(uint32_t prot, char out[7])
{
	out[0] = (prot & LKMDBG_VMA_PROT_READ) ? 'r' : '-';
	out[1] = (prot & LKMDBG_VMA_PROT_WRITE) ? 'w' : '-';
	out[2] = (prot & LKMDBG_VMA_PROT_EXEC) ? 'x' : '-';
	out[3] = (prot & LKMDBG_VMA_PROT_MAYREAD) ? 'R' : '-';
	out[4] = (prot & LKMDBG_VMA_PROT_MAYWRITE) ? 'W' : '-';
	out[5] = (prot & LKMDBG_VMA_PROT_MAYEXEC) ? 'X' : '-';
	out[6] = '\0';
}

static const char *page_level_name(uint32_t level)
{
	switch (level) {
	case LKMDBG_PAGE_LEVEL_PTE:
		return "pte";
	case LKMDBG_PAGE_LEVEL_PMD:
		return "pmd";
	case LKMDBG_PAGE_LEVEL_PUD:
		return "pud";
	default:
		return "none";
	}
}

static int dump_target_vmas(int session_fd)
{
	struct vma_query_buffer buf = { 0 };
	uint64_t cursor = 0;
	int ret = -1;

	buf.entries = calloc(VMA_QUERY_BATCH, sizeof(*buf.entries));
	buf.names = calloc(1, VMA_QUERY_NAMES_SIZE);
	if (!buf.entries || !buf.names) {
		fprintf(stderr, "VMA dump allocation failed\n");
		goto out;
	}

	for (;;) {
		struct lkmdbg_vma_query_request reply;
		unsigned int i;

		if (query_target_vmas(session_fd, cursor, &buf, &reply) < 0)
			goto out;

		for (i = 0; i < reply.entries_filled; i++) {
			const struct lkmdbg_vma_entry *entry = &buf.entries[i];
			const char *name = vma_name_ptr(&reply, entry, &buf);
			char prot[7];

			print_vma_prot(entry->prot, prot);
			printf("%016" PRIx64 "-%016" PRIx64 " %s flags=%08x vm=%016" PRIx64
			       " pgoff=%" PRIu64 " inode=%" PRIu64 " dev=%u:%u %s\n",
			       (uint64_t)entry->start_addr,
			       (uint64_t)entry->end_addr, prot, entry->flags,
			       (uint64_t)entry->vm_flags_raw,
			       (uint64_t)entry->pgoff,
			       (uint64_t)entry->inode,
			       entry->dev_major, entry->dev_minor, name);
		}

		if (reply.done) {
			ret = 0;
			break;
		}

		if (reply.next_addr <= cursor) {
			fprintf(stderr, "VMA dump cursor stalled\n");
			goto out;
		}
		cursor = reply.next_addr;
	}

out:
	free(buf.names);
	free(buf.entries);
	return ret;
}

static void print_stop_state(const struct lkmdbg_stop_state *stop)
{
	printf("stop.cookie=%" PRIu64 "\n", (uint64_t)stop->cookie);
	printf("stop.reason=%u\n", stop->reason);
	printf("stop.flags=0x%x\n", stop->flags);
	printf("stop.tgid=%d\n", stop->tgid);
	printf("stop.tid=%d\n", stop->tid);
	printf("stop.event_flags=0x%x\n", stop->event_flags);
	printf("stop.value0=0x%" PRIx64 "\n", (uint64_t)stop->value0);
	printf("stop.value1=0x%" PRIx64 "\n", (uint64_t)stop->value1);
	if (stop->flags & LKMDBG_STOP_FLAG_REGS_VALID) {
		printf("stop.pc=0x%" PRIx64 "\n", (uint64_t)stop->regs.pc);
		printf("stop.sp=0x%" PRIx64 "\n", (uint64_t)stop->regs.sp);
		printf("stop.pstate=0x%" PRIx64 "\n",
		       (uint64_t)stop->regs.pstate);
	}
}

static int dump_target_pages(int session_fd, uint64_t remote_addr,
			     uint64_t length)
{
	struct page_query_buffer buf = { 0 };
	uint64_t cursor = remote_addr;
	uint64_t end = remote_addr + length;
	int ret = -1;

	buf.entries = calloc(PAGE_QUERY_BATCH, sizeof(*buf.entries));
	if (!buf.entries) {
		fprintf(stderr, "page dump allocation failed\n");
		return -1;
	}

	while (cursor < end) {
		struct lkmdbg_page_query_request reply;
		uint64_t last_addr = cursor;
		unsigned int i;

		if (query_target_pages(session_fd, cursor, end - cursor, &buf, &reply) <
		    0)
			goto out;

		for (i = 0; i < reply.entries_filled; i++) {
			last_addr = buf.entries[i].page_addr;
			printf("page=0x%" PRIx64 " flags=0x%x vm_flags=0x%" PRIx64
			       " level=%s shift=%u pt=0x%" PRIx64
			       " phys=0x%" PRIx64
			       " pgoff=0x%" PRIx64 " inode=%" PRIu64 " dev=%u:%u\n",
			       (uint64_t)buf.entries[i].page_addr,
			       buf.entries[i].flags,
			       (uint64_t)buf.entries[i].vm_flags_raw,
			       page_level_name(buf.entries[i].pt_level),
			       buf.entries[i].page_shift,
			       (uint64_t)buf.entries[i].pt_entry_raw,
			       (uint64_t)buf.entries[i].phys_addr,
			       (uint64_t)buf.entries[i].pgoff,
			       (uint64_t)buf.entries[i].inode,
			       buf.entries[i].dev_major,
			       buf.entries[i].dev_minor);
		}

		if (reply.done) {
			ret = 0;
			goto out;
		}
		if (!reply.entries_filled || reply.next_addr <= last_addr) {
			fprintf(stderr, "QUERY_PAGES pagination stalled\n");
			goto out;
		}

		cursor = reply.next_addr;
	}

	ret = 0;

out:
	free(buf.entries);
	return ret;
}

static void print_pte_patch_reply(const char *prefix,
				  const struct lkmdbg_pte_patch_request *reply)
{
	char mode_buf[16];
	char state_buf[32];

	printf("%s.id=%" PRIu64 "\n", prefix, (uint64_t)reply->id);
	printf("%s.addr=0x%" PRIx64 "\n", prefix, (uint64_t)reply->addr);
	printf("%s.mode=%u(%s)\n", prefix, reply->mode,
	       describe_pte_mode(reply->mode, reply->flags, mode_buf,
				 sizeof(mode_buf)));
	printf("%s.flags=0x%x\n", prefix, reply->flags);
	printf("%s.state=0x%x(%s)\n", prefix, reply->state,
	       describe_pte_patch_state(reply->state, state_buf,
					sizeof(state_buf)));
	printf("%s.raw_pte=0x%" PRIx64 "\n", prefix, (uint64_t)reply->raw_pte);
	printf("%s.baseline_pte=0x%" PRIx64 "\n", prefix,
	       (uint64_t)reply->baseline_pte);
	printf("%s.expected_pte=0x%" PRIx64 "\n", prefix,
	       (uint64_t)reply->expected_pte);
	printf("%s.current_pte=0x%" PRIx64 "\n", prefix,
	       (uint64_t)reply->current_pte);
	printf("%s.baseline_vm_flags=0x%" PRIx64 "\n", prefix,
	       (uint64_t)reply->baseline_vm_flags);
	printf("%s.current_vm_flags=0x%" PRIx64 "\n", prefix,
	       (uint64_t)reply->current_vm_flags);
}

static int dump_target_pte_patches(int session_fd)
{
	struct lkmdbg_pte_patch_entry entries[PTE_PATCH_QUERY_BATCH];
	uint64_t cursor = 0;

	for (;;) {
		struct lkmdbg_pte_patch_query_request reply;
		uint32_t i;

		if (query_pte_patches(session_fd, cursor, entries,
				      (uint32_t)(sizeof(entries) /
						 sizeof(entries[0])),
				      &reply) < 0)
			return -1;

		for (i = 0; i < reply.entries_filled; i++) {
			char mode_buf[16];
			char state_buf[32];

			printf("id=%" PRIu64 " page=0x%" PRIx64 " mode=%u(%s) flags=0x%x state=0x%x(%s) raw=0x%" PRIx64 " baseline=0x%" PRIx64 " expected=0x%" PRIx64 " current=0x%" PRIx64 " vm=0x%" PRIx64 "->0x%" PRIx64 "\n",
			       (uint64_t)entries[i].id,
			       (uint64_t)entries[i].page_addr, entries[i].mode,
			       describe_pte_mode(entries[i].mode, entries[i].flags,
						 mode_buf, sizeof(mode_buf)),
			       entries[i].flags, entries[i].state,
			       describe_pte_patch_state(entries[i].state,
							state_buf,
							sizeof(state_buf)),
			       (uint64_t)entries[i].raw_pte,
			       (uint64_t)entries[i].baseline_pte,
			       (uint64_t)entries[i].expected_pte,
			       (uint64_t)entries[i].current_pte,
			       (uint64_t)entries[i].baseline_vm_flags,
			       (uint64_t)entries[i].current_vm_flags);
		}

		if (reply.done)
			return 0;
		if (reply.next_id <= cursor) {
			fprintf(stderr, "QUERY_PTE_PATCHES pagination stalled\n");
			return -1;
		}
		cursor = reply.next_id;
	}
}

static int expect_partial_write_progress(int session_fd, uintptr_t remote_addr,
					 const char *payload)
{
	struct lkmdbg_mem_op ops[2];
	struct lkmdbg_mem_request req;
	char readback[64];
	uint32_t bytes_done = 0;
	size_t payload_len = strlen(payload) + 1;

	memset(ops, 0, sizeof(ops));
	ops[0].remote_addr = remote_addr;
	ops[0].local_addr = (uintptr_t)payload;
	ops[0].length = (uint32_t)payload_len;
	ops[1].remote_addr = remote_addr + 32;
	ops[1].local_addr = 1;
	ops[1].length = 8;

	memset(&req, 0, sizeof(req));
	req.version = LKMDBG_PROTO_VERSION;
	req.size = sizeof(req);
	req.ops_addr = (uintptr_t)ops;
	req.op_count = 2;

	errno = 0;
	if (ioctl(session_fd, LKMDBG_IOC_WRITE_MEM, &req) == 0) {
		fprintf(stderr, "partial WRITE_MEM unexpectedly succeeded\n");
		return -1;
	}

	if (errno != EFAULT) {
		fprintf(stderr, "partial WRITE_MEM errno=%d expected=%d\n", errno,
			EFAULT);
		return -1;
	}

	if (req.ops_done != 1 || req.bytes_done != payload_len ||
	    ops[0].bytes_done != payload_len || ops[1].bytes_done != 0) {
		fprintf(stderr,
			"partial WRITE_MEM progress mismatch ops_done=%u bytes_done=%" PRIu64 " op0=%u op1=%u\n",
			req.ops_done, (uint64_t)req.bytes_done, ops[0].bytes_done,
			ops[1].bytes_done);
		return -1;
	}

	memset(readback, 0, sizeof(readback));
	if (read_target_memory(session_fd, remote_addr, readback, payload_len,
			       &bytes_done, 0) < 0 ||
	    bytes_done != payload_len || strcmp(readback, payload) != 0) {
		fprintf(stderr,
			"partial WRITE_MEM first op readback failed bytes_done=%u\n",
			bytes_done);
		return -1;
	}

	return 0;
}

static int dump_session_events(int session_fd, unsigned int max_events,
			       int timeout_ms)
{
	struct lkmdbg_event_record events[EVENT_READ_BATCH];
	unsigned int printed = 0;

	if (!max_events)
		max_events = EVENT_READ_BATCH;

	while (printed < max_events) {
		size_t events_read = 0;
		size_t batch_limit = max_events - printed;
		unsigned int i;
		int ret;

		if (batch_limit > sizeof(events) / sizeof(events[0]))
			batch_limit = sizeof(events) / sizeof(events[0]);

		ret = read_session_events_timeout(session_fd, events, batch_limit,
						  &events_read, timeout_ms);
		if (ret < 0)
			return -1;
		if (ret > 0)
			break;

		for (i = 0; i < events_read; i++)
			print_event_record(&events[i], printed + i);
		printed += events_read;

		if ((size_t)events_read < batch_limit)
			break;
		timeout_ms = 0;
	}

	printf("events_read=%u\n", printed);
	return 0;
}

static int query_mincore_resident(void *addr, size_t len)
{
	unsigned char vec = 0;

	if (mincore(addr, len, &vec) < 0)
		return -1;

	return !!(vec & 1);
}

static volatile uint64_t child_watch_value;

static void child_signal_handler(int signo)
{
	(void)signo;
}

static __attribute__((noinline)) void
child_exec_target(volatile uint64_t *counter)
{
	(*counter)++;
	asm volatile("" ::: "memory");
}

static void *child_spawn_thread_main(void *arg)
{
	volatile uint64_t *exec_counter = arg;

	child_exec_target(exec_counter);
	return NULL;
}

static void *freeze_child_thread_main(void *arg)
{
	struct freeze_child_ctx *ctx = arg;
	pid_t tid = (pid_t)syscall(SYS_gettid);

	ctx->tid_slot[ctx->thread_index] = tid;
	while (!*ctx->stop) {
		(*ctx->counter)++;
		if (((*ctx->counter) & 0x3ffULL) == 0)
			syscall(SYS_gettid);
	}

	return NULL;
}

static int child_selftest_main(int info_fd, int cmd_fd, int resp_fd)
{
	struct child_info info;
	char child_buf[64] = "child-buffer-initial";
	char *slots_map;
	void *nofault_map;
	void *force_read_map;
	char *force_write_map;
	unsigned char *large_map;
	uint64_t *freeze_counters;
	pid_t *freeze_tids;
	volatile int *freeze_stop;
	pthread_t freeze_threads[SELFTEST_FREEZE_THREADS];
	struct freeze_child_ctx freeze_ctxs[SELFTEST_FREEZE_THREADS];
	void *file_map;
	volatile uint64_t *mmu_watch_map;
	volatile uint64_t exec_counter = 0;
	int file_fd;
	struct stat st;
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

	force_read_map = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (force_read_map == MAP_FAILED)
		return 2;

	force_write_map = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (force_write_map == MAP_FAILED)
		return 2;

	large_map = mmap(NULL, SELFTEST_LARGE_MAP_LEN, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (large_map == MAP_FAILED)
		return 2;

	freeze_counters = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (freeze_counters == MAP_FAILED)
		return 2;

	freeze_tids = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (freeze_tids == MAP_FAILED)
		return 2;

	freeze_stop = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (freeze_stop == MAP_FAILED)
		return 2;

	file_fd = memfd_create("lkmdbg-vma", MFD_CLOEXEC);
	if (file_fd < 0)
		return 2;

	if (signal(SIGUSR1, child_signal_handler) == SIG_ERR)
		return 2;

	if (ftruncate(file_fd, (off_t)page_size) < 0)
		return 2;

	file_map = mmap(NULL, page_size, PROT_READ, MAP_PRIVATE, file_fd, 0);
	if (file_map == MAP_FAILED)
		return 2;

	mmu_watch_map = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mmu_watch_map == MAP_FAILED)
		return 2;

	if (fstat(file_fd, &st) < 0)
		return 2;

	for (i = 0; i < SELFTEST_THREAD_COUNT; i++) {
		snprintf(slots_map + (i * SELFTEST_SLOT_SIZE), SELFTEST_SLOT_SIZE,
			 "slot-%u-initial", i);
	}
	fill_pattern(large_map, SELFTEST_LARGE_MAP_LEN, 1);
	*mmu_watch_map = 0x123456789abcdef0ULL;

	memset(nofault_map, 0xA5, page_size);
	if (madvise(nofault_map, page_size, MADV_DONTNEED) < 0)
		return 2;

	fill_pattern(force_read_map, page_size, 3);
	if (mprotect(force_read_map, page_size, PROT_NONE) < 0)
		return 2;

	snprintf(force_write_map, page_size, "%s", "force-write-initial");
	if (mprotect(force_write_map, page_size, PROT_READ) < 0)
		return 2;

	for (i = 0; i < SELFTEST_FREEZE_THREADS; i++) {
		freeze_ctxs[i].stop = freeze_stop;
		freeze_ctxs[i].counter = &freeze_counters[i];
		freeze_ctxs[i].tid_slot = freeze_tids;
		freeze_ctxs[i].thread_index = i;
		if (pthread_create(&freeze_threads[i], NULL,
				   freeze_child_thread_main,
				   &freeze_ctxs[i]) != 0)
			return 2;
	}

	memset(&info, 0, sizeof(info));
	info.basic_addr = (uintptr_t)child_buf;
	info.slots_addr = (uintptr_t)slots_map;
	info.nofault_addr = (uintptr_t)nofault_map;
	info.force_read_addr = (uintptr_t)force_read_map;
	info.force_write_addr = (uintptr_t)force_write_map;
	info.large_addr = (uintptr_t)large_map;
	info.file_addr = (uintptr_t)file_map;
	info.exec_target_addr = (uintptr_t)child_exec_target;
	info.watch_addr = (uintptr_t)&child_watch_value;
	info.watch_mmu_addr = (uintptr_t)mmu_watch_map;
	info.freeze_counters_addr = (uintptr_t)freeze_counters;
	info.freeze_tids_addr = (uintptr_t)freeze_tids;
	info.file_inode = (uint64_t)st.st_ino;
	info.file_dev_major = (uint32_t)major(st.st_dev);
	info.file_dev_minor = (uint32_t)minor(st.st_dev);
	info.freeze_thread_count = SELFTEST_FREEZE_THREADS;
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
		case CHILD_OP_TRIGGER_EXEC:
			child_exec_target(&exec_counter);
			break;
		case CHILD_OP_TRIGGER_WATCH:
			child_watch_value++;
			break;
		case CHILD_OP_TRIGGER_WATCH_MMU:
			(*mmu_watch_map)++;
			break;
		case CHILD_OP_TRIGGER_SIGNAL:
			if (kill(getpid(), SIGUSR1) < 0)
				return 2;
			break;
		case CHILD_OP_READ_REMOTE:
			if (!cmd.addr || !cmd.length ||
			    cmd.length > SELFTEST_LARGE_MAP_LEN)
				return 2;
			if (write_full(resp_fd, (void *)(uintptr_t)cmd.addr,
				       cmd.length) != (ssize_t)cmd.length)
				return 2;
			break;
		case CHILD_OP_FILL_REMOTE:
			resident = 0;
			if (!cmd.addr || !cmd.length ||
			    cmd.length > SELFTEST_LARGE_MAP_LEN)
				return 2;
			memset((void *)(uintptr_t)cmd.addr, (int)cmd.value,
			       cmd.length);
			if (write_full(resp_fd, &resident, sizeof(resident)) !=
			    (ssize_t)sizeof(resident))
				return 2;
			break;
		case CHILD_OP_SPAWN_THREAD:
		{
			pthread_t temp_thread;

			if (pthread_create(&temp_thread, NULL,
					   child_spawn_thread_main,
					   (void *)&exec_counter) != 0)
				return 2;
			pthread_join(temp_thread, NULL);
			break;
		}
		case CHILD_OP_EXIT:
			*freeze_stop = 1;
			for (i = 0; i < SELFTEST_FREEZE_THREADS; i++)
				pthread_join(freeze_threads[i], NULL);
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

static int send_child_command(int cmd_fd, uint32_t op)
{
	struct child_cmd cmd = {
		.op = op,
	};

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "failed to send child command op=%u\n", op);
		return -1;
	}

	return 0;
}

static int child_read_remote_range(int cmd_fd, int resp_fd, uintptr_t addr,
				   void *buf, size_t len)
{
	struct child_cmd cmd = {
		.op = CHILD_OP_READ_REMOTE,
		.addr = addr,
		.length = (uint32_t)len,
	};

	if (!buf || !len || len > UINT32_MAX)
		return -1;

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "failed to send child remote read command\n");
		return -1;
	}

	if (read_full(resp_fd, buf, len) != (ssize_t)len) {
		fprintf(stderr, "failed to read child remote read reply\n");
		return -1;
	}

	return 0;
}

static int child_fill_remote_range(int cmd_fd, int resp_fd, uintptr_t addr,
				   size_t len, uint8_t value)
{
	struct child_cmd cmd = {
		.op = CHILD_OP_FILL_REMOTE,
		.addr = addr,
		.length = (uint32_t)len,
		.value = value,
	};
	int ret;

	if (!len || len > UINT32_MAX)
		return -1;

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "failed to send child remote fill command\n");
		return -1;
	}

	if (read_full(resp_fd, &ret, sizeof(ret)) != (ssize_t)sizeof(ret)) {
		fprintf(stderr, "failed to read child remote fill reply\n");
		return -1;
	}
	if (ret) {
		fprintf(stderr, "child remote fill failed ret=%d\n", ret);
		return -1;
	}

	return 0;
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

static int read_freeze_counters(int session_fd, const struct child_info *info,
				uint64_t *counters)
{
	uint32_t bytes_done = 0;
	size_t len = info->freeze_thread_count * sizeof(*counters);

	if (read_target_memory(session_fd, info->freeze_counters_addr, counters,
			       len, &bytes_done, 0) < 0 || bytes_done != len) {
		fprintf(stderr, "freeze counter read failed bytes_done=%u\n",
			bytes_done);
		return -1;
	}

	return 0;
}

static int read_freeze_thread_tids(int session_fd, const struct child_info *info,
				   pid_t *tids)
{
	uint32_t bytes_done = 0;
	size_t len = info->freeze_thread_count * sizeof(*tids);

	if (read_target_memory(session_fd, info->freeze_tids_addr, tids, len,
			       &bytes_done, 0) < 0 || bytes_done != len) {
		fprintf(stderr, "freeze tid read failed bytes_done=%u\n",
			bytes_done);
		return -1;
	}

	return 0;
}

static int wait_for_freeze_thread_tids(int session_fd,
				       const struct child_info *info,
				       pid_t *tids)
{
	unsigned int attempt;
	uint32_t i;

	for (attempt = 0; attempt < 40; attempt++) {
		if (read_freeze_thread_tids(session_fd, info, tids) < 0)
			return -1;

		for (i = 0; i < info->freeze_thread_count; i++) {
			if (tids[i] <= 0)
				break;
		}

		if (i == info->freeze_thread_count)
			return 0;

		usleep(50000);
	}

	fprintf(stderr, "freeze tids did not stabilize\n");
	return -1;
}

static int find_target_thread_entry(int session_fd, pid_t tid,
				    struct lkmdbg_thread_entry *entry_out)
{
	struct lkmdbg_thread_entry entries[THREAD_QUERY_BATCH];
	int32_t cursor = 0;

	for (;;) {
		struct lkmdbg_thread_query_request reply;
		uint32_t i;

		if (query_target_threads(session_fd, cursor, entries,
					 THREAD_QUERY_BATCH, &reply) < 0)
			return -1;

		for (i = 0; i < reply.entries_filled; i++) {
			if (entries[i].tid != tid)
				continue;

			*entry_out = entries[i];
			return 0;
		}

		if (reply.done)
			break;
		cursor = reply.next_tid;
	}

	fprintf(stderr, "thread %d not found in QUERY_THREADS\n", tid);
	return -1;
}

static int verify_thread_query(int session_fd, pid_t child,
			       const struct child_info *info, const pid_t *tids)
{
	struct lkmdbg_thread_entry entries[THREAD_QUERY_BATCH];
	int32_t cursor = 0;
	uint32_t total = 0;
	uint32_t found = 0;
	int saw_leader = 0;

	for (;;) {
		struct lkmdbg_thread_query_request reply;
		uint32_t i;

		if (query_target_threads(session_fd, cursor, entries,
					 THREAD_QUERY_BATCH, &reply) < 0)
			return -1;

		total += reply.entries_filled;
		for (i = 0; i < reply.entries_filled; i++) {
			uint32_t j;

			if (entries[i].tid == child &&
			    (entries[i].flags &
			     (LKMDBG_THREAD_FLAG_GROUP_LEADER |
			      LKMDBG_THREAD_FLAG_SESSION_TARGET)) ==
				    (LKMDBG_THREAD_FLAG_GROUP_LEADER |
				     LKMDBG_THREAD_FLAG_SESSION_TARGET))
				saw_leader = 1;

			for (j = 0; j < info->freeze_thread_count; j++) {
				if (entries[i].tid != tids[j])
					continue;
				found |= 1U << j;
				break;
			}
		}

		if (reply.done)
			break;
		cursor = reply.next_tid;
	}

	if (!saw_leader || total < info->freeze_thread_count + 1 ||
	    found != ((1U << info->freeze_thread_count) - 1U)) {
		fprintf(stderr,
			"thread query mismatch leader=%d total=%u found_mask=0x%x expected_threads=%u\n",
			saw_leader, total, found, info->freeze_thread_count);
		return -1;
	}

	printf("selftest thread query ok total=%u worker_mask=0x%x\n", total,
	       found);
	return 0;
}

static void print_arm64_regs(const struct lkmdbg_thread_regs_request *req)
{
	unsigned int i;

	printf("regs tid=%d\n", req->tid);
	for (i = 0; i < 31; i++)
		printf("x%u=0x%016" PRIx64 "\n", i,
		       (uint64_t)req->regs.regs[i]);
	printf("sp=0x%016" PRIx64 "\n", (uint64_t)req->regs.sp);
	printf("pc=0x%016" PRIx64 "\n", (uint64_t)req->regs.pc);
	printf("pstate=0x%016" PRIx64 "\n", (uint64_t)req->regs.pstate);
}

static int set_named_arm64_reg(struct lkmdbg_thread_regs_request *req,
			       const char *name, uint64_t value)
{
	unsigned long index;
	char *endp;

	if (strcmp(name, "sp") == 0) {
		req->regs.sp = value;
		return 0;
	}

	if (strcmp(name, "pc") == 0) {
		req->regs.pc = value;
		return 0;
	}

	if (strcmp(name, "pstate") == 0) {
		req->regs.pstate = value;
		return 0;
	}

	if (name[0] != 'x')
		return -1;

	index = strtoul(name + 1, &endp, 10);
	if (*endp != '\0' || index >= 31)
		return -1;

	req->regs.regs[index] = value;
	return 0;
}

static int parse_hwpoint_type(const char *arg, uint32_t *type_out)
{
	uint32_t type = 0;
	const char *p;

	if (strcmp(arg, "r") == 0) {
		*type_out = LKMDBG_HWPOINT_TYPE_READ;
		return 0;
	}
	if (strcmp(arg, "w") == 0) {
		*type_out = LKMDBG_HWPOINT_TYPE_WRITE;
		return 0;
	}
	if (strcmp(arg, "rw") == 0) {
		*type_out = LKMDBG_HWPOINT_TYPE_READWRITE;
		return 0;
	}
	if (strcmp(arg, "x") == 0) {
		*type_out = LKMDBG_HWPOINT_TYPE_EXEC;
		return 0;
	}

	for (p = arg; *p; p++) {
		switch (*p) {
		case 'r':
			type |= LKMDBG_HWPOINT_TYPE_READ;
			break;
		case 'w':
			type |= LKMDBG_HWPOINT_TYPE_WRITE;
			break;
		case 'x':
			type |= LKMDBG_HWPOINT_TYPE_EXEC;
			break;
		default:
			return -1;
		}
	}

	if (!type)
		return -1;

	*type_out = type;
	return 0;
}

static int parse_hwpoint_flags(const char *arg, uint32_t *flags_out)
{
	char *copy;
	char *token;
	char *saveptr = NULL;
	char *endp = NULL;
	uint32_t flags = 0;

	if (strcmp(arg, "stop") == 0 || strcmp(arg, "0") == 0) {
		*flags_out = 0;
		return 0;
	}
	if (strcmp(arg, "counter") == 0 || strcmp(arg, "count") == 0) {
		*flags_out = LKMDBG_HWPOINT_FLAG_COUNTER_MODE;
		return 0;
	}
	if (strcmp(arg, "mmu") == 0) {
		*flags_out = LKMDBG_HWPOINT_FLAG_MMU_EXEC;
		return 0;
	}

	copy = strdup(arg);
	if (!copy)
		return -1;

	for (token = strtok_r(copy, ",|", &saveptr); token;
	     token = strtok_r(NULL, ",|", &saveptr)) {
		if (strcmp(token, "stop") == 0)
			continue;
		if (strcmp(token, "counter") == 0 || strcmp(token, "count") == 0) {
			flags |= LKMDBG_HWPOINT_FLAG_COUNTER_MODE;
			continue;
		}
		if (strcmp(token, "mmu") == 0) {
			flags |= LKMDBG_HWPOINT_FLAG_MMU_EXEC;
			continue;
		}
		free(copy);
		goto numeric;
	}

	free(copy);
	*flags_out = flags;
	return 0;

numeric:
	*flags_out = (uint32_t)strtoul(arg, &endp, 0);
	if (*endp != '\0' ||
	    (*flags_out &
	     ~(LKMDBG_HWPOINT_FLAG_COUNTER_MODE | LKMDBG_HWPOINT_FLAG_MMU_EXEC)))
		return -1;
	return 0;
}

static int parse_hwpoint_actions(const char *arg, uint32_t *actions_out)
{
	char *copy;
	char *token;
	char *saveptr = NULL;
	char *endp = NULL;
	uint32_t actions = 0;

	if (strcmp(arg, "stop") == 0 || strcmp(arg, "0") == 0) {
		*actions_out = 0;
		return 0;
	}

	copy = strdup(arg);
	if (!copy)
		return -1;

	for (token = strtok_r(copy, ",|", &saveptr); token;
	     token = strtok_r(NULL, ",|", &saveptr)) {
		if (strcmp(token, "stop") == 0)
			continue;
		if (strcmp(token, "oneshot") == 0) {
			actions |= LKMDBG_HWPOINT_ACTION_ONESHOT;
			continue;
		}
		if (strcmp(token, "autocont") == 0 ||
		    strcmp(token, "autocontinue") == 0 ||
		    strcmp(token, "continue") == 0) {
			actions |= LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE;
			continue;
		}
		free(copy);
		goto numeric;
	}

	free(copy);
	*actions_out = actions;
	return 0;

numeric:
	*actions_out = (uint32_t)strtoul(arg, &endp, 0);
	if (*endp != '\0' ||
	    (*actions_out &
	     ~(LKMDBG_HWPOINT_ACTION_ONESHOT |
	       LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE)))
		return -1;
	return 0;
}

static void append_flag_name(char *buf, size_t buf_size, const char *name)
{
	size_t used;

	if (!buf_size)
		return;

	used = strlen(buf);
	if (used >= buf_size - 1)
		return;
	if (used) {
		snprintf(buf + used, buf_size - used, "|%s", name);
		return;
	}
	snprintf(buf, buf_size, "%s", name);
}

static const char *describe_hwpoint_type(uint32_t type, char *buf, size_t buf_size)
{
	buf[0] = '\0';

	if (type & LKMDBG_HWPOINT_TYPE_READ)
		append_flag_name(buf, buf_size, "read");
	if (type & LKMDBG_HWPOINT_TYPE_WRITE)
		append_flag_name(buf, buf_size, "write");
	if (type & LKMDBG_HWPOINT_TYPE_EXEC)
		append_flag_name(buf, buf_size, "exec");
	if (!buf[0])
		snprintf(buf, buf_size, "none");

	return buf;
}

static const char *describe_hwpoint_flags(uint32_t flags, char *buf, size_t buf_size)
{
	buf[0] = '\0';

	if (flags & LKMDBG_HWPOINT_FLAG_COUNTER_MODE)
		append_flag_name(buf, buf_size, "counter");
	if (flags & LKMDBG_HWPOINT_FLAG_MMU)
		append_flag_name(buf, buf_size, "mmu");
	if (!buf[0])
		snprintf(buf, buf_size, "stop");

	return buf;
}

static const char *describe_hwpoint_actions(uint32_t actions, char *buf,
					    size_t buf_size)
{
	buf[0] = '\0';

	if (actions & LKMDBG_HWPOINT_ACTION_ONESHOT)
		append_flag_name(buf, buf_size, "oneshot");
	if (actions & LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE)
		append_flag_name(buf, buf_size, "autocont");
	if (!buf[0])
		snprintf(buf, buf_size, "stop");

	return buf;
}

static const char *describe_hwpoint_state(uint32_t state, char *buf, size_t buf_size)
{
	buf[0] = '\0';

	if (state & LKMDBG_HWPOINT_STATE_ACTIVE)
		append_flag_name(buf, buf_size, "active");
	if (state & LKMDBG_HWPOINT_STATE_LATCHED)
		append_flag_name(buf, buf_size, "latched");
	if (state & LKMDBG_HWPOINT_STATE_LOST)
		append_flag_name(buf, buf_size, "lost");
	if (state & LKMDBG_HWPOINT_STATE_MUTATED)
		append_flag_name(buf, buf_size, "mutated");
	if (!buf[0])
		snprintf(buf, buf_size, "idle");

	return buf;
}

static int parse_pte_mode(const char *arg, uint32_t *mode_out,
			  uint32_t *flags_out, uint64_t *raw_pte_out)
{
	char *endp;

	if (strncmp(arg, "raw:", 4) == 0) {
		uint64_t raw_pte;

		raw_pte = strtoull(arg + 4, &endp, 16);
		if (*endp != '\0')
			return -1;

		*mode_out = LKMDBG_PTE_MODE_RAW;
		*flags_out = LKMDBG_PTE_PATCH_FLAG_RAW;
		*raw_pte_out = raw_pte;
		return 0;
	}

	if (strcmp(arg, "ro") == 0)
		*mode_out = LKMDBG_PTE_MODE_RO;
	else if (strcmp(arg, "rw") == 0)
		*mode_out = LKMDBG_PTE_MODE_RW;
	else if (strcmp(arg, "rx") == 0)
		*mode_out = LKMDBG_PTE_MODE_RX;
	else if (strcmp(arg, "rwx") == 0)
		*mode_out = LKMDBG_PTE_MODE_RWX;
	else if (strcmp(arg, "protnone") == 0)
		*mode_out = LKMDBG_PTE_MODE_PROTNONE;
	else if (strcmp(arg, "execonly") == 0)
		*mode_out = LKMDBG_PTE_MODE_EXECONLY;
	else
		return -1;

	*flags_out = 0;
	*raw_pte_out = 0;
	return 0;
}

static const char *describe_pte_mode(uint32_t mode, uint32_t flags, char *buf,
				     size_t buf_size)
{
	buf[0] = '\0';

	if (flags & LKMDBG_PTE_PATCH_FLAG_RAW)
		snprintf(buf, buf_size, "raw");
	else if (mode == LKMDBG_PTE_MODE_RO)
		snprintf(buf, buf_size, "ro");
	else if (mode == LKMDBG_PTE_MODE_RW)
		snprintf(buf, buf_size, "rw");
	else if (mode == LKMDBG_PTE_MODE_RX)
		snprintf(buf, buf_size, "rx");
	else if (mode == LKMDBG_PTE_MODE_RWX)
		snprintf(buf, buf_size, "rwx");
	else if (mode == LKMDBG_PTE_MODE_PROTNONE)
		snprintf(buf, buf_size, "protnone");
	else if (mode == LKMDBG_PTE_MODE_EXECONLY)
		snprintf(buf, buf_size, "execonly");
	else
		snprintf(buf, buf_size, "unknown");

	return buf;
}

static const char *describe_pte_patch_state(uint32_t state, char *buf,
					    size_t buf_size)
{
	buf[0] = '\0';

	if (state & LKMDBG_PTE_PATCH_STATE_ACTIVE)
		append_flag_name(buf, buf_size, "active");
	if (state & LKMDBG_PTE_PATCH_STATE_LOST)
		append_flag_name(buf, buf_size, "lost");
	if (state & LKMDBG_PTE_PATCH_STATE_MUTATED)
		append_flag_name(buf, buf_size, "mutated");
	if (!buf[0])
		snprintf(buf, buf_size, "idle");

	return buf;
}

static const char *describe_image_flags(uint32_t flags, char *buf,
					size_t buf_size)
{
	buf[0] = '\0';

	if (flags & LKMDBG_IMAGE_FLAG_FILE)
		append_flag_name(buf, buf_size, "file");
	if (flags & LKMDBG_IMAGE_FLAG_MAIN_EXE)
		append_flag_name(buf, buf_size, "main-exe");
	if (flags & LKMDBG_IMAGE_FLAG_SHARED)
		append_flag_name(buf, buf_size, "shared");
	if (flags & LKMDBG_IMAGE_FLAG_DELETED)
		append_flag_name(buf, buf_size, "deleted");
	if (!buf[0])
		snprintf(buf, buf_size, "none");

	return buf;
}

static int dump_target_threads(int session_fd)
{
	struct lkmdbg_thread_entry entries[THREAD_QUERY_BATCH];
	int32_t cursor = 0;

	for (;;) {
		struct lkmdbg_thread_query_request reply;
		uint32_t i;

		if (query_target_threads(session_fd, cursor, entries,
					 THREAD_QUERY_BATCH, &reply) < 0)
			return -1;

		for (i = 0; i < reply.entries_filled; i++) {
			printf("tid=%d tgid=%d flags=0x%x pc=0x%016" PRIx64
			       " sp=0x%016" PRIx64 " comm=%s\n",
			       entries[i].tid, entries[i].tgid, entries[i].flags,
			       (uint64_t)entries[i].user_pc,
			       (uint64_t)entries[i].user_sp,
			       entries[i].comm);
		}

		if (reply.done)
			return 0;
		cursor = reply.next_tid;
	}
}

static int dump_target_images(int session_fd)
{
	struct image_query_buffer buf = { 0 };
	uint64_t cursor = 0;
	int ret = -1;

	buf.entries = calloc(IMAGE_QUERY_BATCH, sizeof(*buf.entries));
	buf.names = calloc(1, IMAGE_QUERY_NAMES_SIZE);
	if (!buf.entries || !buf.names) {
		fprintf(stderr, "image query allocation failed\n");
		goto out;
	}

	for (;;) {
		struct lkmdbg_image_query_request reply;
		uint32_t i;

		if (query_target_images(session_fd, cursor, &buf, &reply) < 0)
			goto out;

		for (i = 0; i < reply.entries_filled; i++) {
			char flag_buf[64];

			printf("start=0x%016" PRIx64 " end=0x%016" PRIx64
			       " base=0x%016" PRIx64 " pgoff=0x%" PRIx64
			       " inode=%" PRIu64 " dev=%u:%u prot=0x%x"
			       " flags=0x%x(%s) segs=%u path=%s\n",
			       (uint64_t)buf.entries[i].start_addr,
			       (uint64_t)buf.entries[i].end_addr,
			       (uint64_t)buf.entries[i].base_addr,
			       (uint64_t)buf.entries[i].pgoff,
			       (uint64_t)buf.entries[i].inode,
			       buf.entries[i].dev_major,
			       buf.entries[i].dev_minor,
			       buf.entries[i].prot,
			       buf.entries[i].flags,
			       describe_image_flags(buf.entries[i].flags,
						    flag_buf,
						    sizeof(flag_buf)),
			       buf.entries[i].segment_count,
			       image_name_ptr(&reply, &buf.entries[i], &buf));
		}

		if (reply.done) {
			ret = 0;
			break;
		}
		cursor = reply.next_addr;
	}

out:
	free(buf.names);
	free(buf.entries);
	return ret;
}

static int get_and_print_regs(int session_fd, pid_t tid)
{
	struct lkmdbg_thread_regs_request req;

	memset(&req, 0, sizeof(req));
	if (freeze_target_threads(session_fd, 2000, NULL, 0) < 0)
		return -1;

	if (get_target_regs(session_fd, tid, &req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	print_arm64_regs(&req);
	if (thaw_target_threads(session_fd, 2000, NULL, 0) < 0)
		return -1;
	return 0;
}

static int set_and_verify_reg(int session_fd, pid_t tid, const char *name,
			      uint64_t value)
{
	struct lkmdbg_thread_regs_request req;

	memset(&req, 0, sizeof(req));
	if (freeze_target_threads(session_fd, 2000, NULL, 0) < 0)
		return -1;

	if (get_target_regs(session_fd, tid, &req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (set_named_arm64_reg(&req, name, value) < 0) {
		fprintf(stderr, "unknown register: %s\n", name);
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (set_target_regs(session_fd, &req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	memset(&req, 0, sizeof(req));
	if (get_target_regs(session_fd, tid, &req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	print_arm64_regs(&req);
	if (thaw_target_threads(session_fd, 2000, NULL, 0) < 0)
		return -1;
	return 0;
}

static int freeze_counters_equal(const uint64_t *a, const uint64_t *b,
				 uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (a[i] != b[i])
			return 0;
	}

	return 1;
}

static int freeze_counters_advanced(const uint64_t *before, const uint64_t *after,
				    uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (after[i] > before[i])
			return 1;
	}

	return 0;
}

static int verify_thread_freeze(int session_fd, pid_t child,
				const struct child_info *info)
{
	struct lkmdbg_freeze_request req;
	struct lkmdbg_thread_entry parked_entry;
	struct lkmdbg_thread_regs_request regs_req;
	uint64_t saved_x19 = 0;
	uint64_t before[SELFTEST_FREEZE_THREADS];
	uint64_t frozen_a[SELFTEST_FREEZE_THREADS];
	uint64_t frozen_b[SELFTEST_FREEZE_THREADS];
	uint64_t after[SELFTEST_FREEZE_THREADS];
	pid_t tids[SELFTEST_FREEZE_THREADS];
	unsigned int i;
	unsigned int parked_index = UINT32_MAX;

	memset(before, 0, sizeof(before));
	memset(frozen_a, 0, sizeof(frozen_a));
	memset(frozen_b, 0, sizeof(frozen_b));
	memset(after, 0, sizeof(after));
	memset(tids, 0, sizeof(tids));
	memset(&parked_entry, 0, sizeof(parked_entry));
	memset(&regs_req, 0, sizeof(regs_req));

	if (wait_for_freeze_thread_tids(session_fd, info, tids) < 0)
		return -1;

	if (verify_thread_query(session_fd, child, info, tids) < 0)
		return -1;

	usleep(50000);
	if (read_freeze_counters(session_fd, info, before) < 0)
		return -1;

	if (freeze_target_threads(session_fd, 2000, &req, 1) < 0)
		return -1;

	if (req.threads_settled < info->freeze_thread_count ||
	    req.threads_parked < info->freeze_thread_count) {
		fprintf(stderr,
			"freeze settle mismatch total=%u settled=%u parked=%u expected_threads=%u\n",
			req.threads_total, req.threads_settled, req.threads_parked,
			info->freeze_thread_count);
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	for (i = 0; i < info->freeze_thread_count; i++) {
		if (find_target_thread_entry(session_fd, tids[i], &parked_entry) < 0) {
			thaw_target_threads(session_fd, 2000, NULL, 0);
			return -1;
		}
		if (!(parked_entry.flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED))
			continue;
		parked_index = i;
		break;
	}

	if (parked_index == UINT32_MAX) {
		fprintf(stderr, "no parked worker thread found after freeze\n");
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (get_target_regs(session_fd, tids[parked_index], &regs_req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	saved_x19 = regs_req.regs.regs[19];
	regs_req.regs.regs[19] = SELFTEST_REG_SENTINEL;
	if (set_target_regs(session_fd, &regs_req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	memset(&regs_req, 0, sizeof(regs_req));
	if (get_target_regs(session_fd, tids[parked_index], &regs_req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (regs_req.regs.regs[19] != SELFTEST_REG_SENTINEL) {
		fprintf(stderr, "SET_REGS x19 did not stick got=0x%" PRIx64 "\n",
			(uint64_t)regs_req.regs.regs[19]);
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	regs_req.regs.regs[19] = saved_x19;
	if (set_target_regs(session_fd, &regs_req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (read_freeze_counters(session_fd, info, frozen_a) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	usleep(50000);
	if (read_freeze_counters(session_fd, info, frozen_b) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (!freeze_counters_equal(frozen_a, frozen_b, info->freeze_thread_count)) {
		fprintf(stderr, "freeze counters changed while frozen\n");
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (thaw_target_threads(session_fd, 2000, &req, 1) < 0)
		return -1;

	usleep(50000);
	if (read_freeze_counters(session_fd, info, after) < 0)
		return -1;

	if (!freeze_counters_advanced(frozen_b, after, info->freeze_thread_count)) {
		fprintf(stderr, "freeze counters did not resume after thaw\n");
		return -1;
	}

	printf("selftest thread freeze ok total=%u settled=%u parked=%u\n",
	       req.threads_total, req.threads_settled, req.threads_parked);
	return 0;
}

static int find_hwpoint_id(int session_fd, uint64_t id)
{
	struct lkmdbg_hwpoint_entry entries[16];
	uint64_t cursor = 0;

	for (;;) {
		struct lkmdbg_hwpoint_query_request reply;
		uint32_t i;

		if (query_hwpoints(session_fd, cursor, entries,
				   (uint32_t)(sizeof(entries) / sizeof(entries[0])),
				   &reply) < 0)
			return -1;

		for (i = 0; i < reply.entries_filled; i++) {
			if (entries[i].id == id)
				return 0;
		}

		if (reply.done)
			break;
		cursor = reply.next_id;
	}

	fprintf(stderr, "hwpoint %" PRIu64 " not found in query\n", id);
	return -1;
}

static int query_hwpoint_entry_by_id(int session_fd, uint64_t id,
				     struct lkmdbg_hwpoint_entry *entry_out)
{
	struct lkmdbg_hwpoint_entry entries[16];
	uint64_t cursor = 0;

	for (;;) {
		struct lkmdbg_hwpoint_query_request reply;
		uint32_t i;

		if (query_hwpoints(session_fd, cursor, entries,
				   (uint32_t)(sizeof(entries) / sizeof(entries[0])),
				   &reply) < 0)
			return -1;

		for (i = 0; i < reply.entries_filled; i++) {
			if (entries[i].id != id)
				continue;
			if (entry_out)
				*entry_out = entries[i];
			return 0;
		}

		if (reply.done)
			break;
		cursor = reply.next_id;
	}

	fprintf(stderr, "hwpoint %" PRIu64 " not found in query\n", id);
	return -1;
}

static int verify_runtime_events(int session_fd, int cmd_fd, pid_t child,
				 const struct child_info *info)
{
	struct lkmdbg_hwpoint_request bp_req;
	struct lkmdbg_hwpoint_request mmu_req;
	struct lkmdbg_hwpoint_request wp_req;
	struct lkmdbg_event_record event;
	struct lkmdbg_stop_query_request stop_req;
	const int event_timeout_ms = 5000;
	int ret;

	memset(&bp_req, 0, sizeof(bp_req));
	memset(&mmu_req, 0, sizeof(mmu_req));
	memset(&wp_req, 0, sizeof(wp_req));
	memset(&stop_req, 0, sizeof(stop_req));
	if (drain_session_events(session_fd) < 0)
		return -1;

	printf("selftest runtime: waiting for clone event\n");
	if (send_child_command(cmd_fd, CHILD_OP_SPAWN_THREAD) < 0)
		return -1;
	ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_CLONE,
				     LKMDBG_TARGET_CLONE_THREAD,
				     event_timeout_ms, &event);
	if (ret == -ETIMEDOUT) {
		printf("selftest runtime: clone/signal lifecycle events unavailable, skipping\n");
		goto breakpoint_checks;
	}
	if (ret < 0)
		return -1;
	if (event.tgid != child) {
		fprintf(stderr, "clone event tgid mismatch got=%d expected=%d\n",
			event.tgid, child);
		return -1;
	}

	printf("selftest runtime: waiting for signal event\n");
	if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_SIGNAL) < 0)
		return -1;
	ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_SIGNAL,
				     SIGUSR1, event_timeout_ms, &event);
	if (ret == -ETIMEDOUT) {
		printf("selftest runtime: signal lifecycle event unavailable, continuing\n");
	} else if (ret < 0) {
		return -1;
	} else if (event.tgid != child || event.tid != child) {
		fprintf(stderr,
			"signal event task mismatch tgid=%d tid=%d expected leader=%d\n",
			event.tgid, event.tid, child);
		return -1;
	}

breakpoint_checks:
	if (add_hwpoint(session_fd, child, info->exec_target_addr,
			LKMDBG_HWPOINT_TYPE_EXEC, 4, 0, &bp_req) < 0)
		return -1;
	if (find_hwpoint_id(session_fd, bp_req.id) < 0) {
		remove_hwpoint(session_fd, bp_req.id);
		return -1;
	}

	if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_EXEC) < 0) {
		remove_hwpoint(session_fd, bp_req.id);
		return -1;
	}
	printf("selftest runtime: waiting for breakpoint stop event\n");
	if (wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
				   LKMDBG_STOP_REASON_BREAKPOINT,
				   event_timeout_ms,
				   &event) < 0) {
		remove_hwpoint(session_fd, bp_req.id);
		return -1;
	}
	if (event.value0 != info->exec_target_addr ||
	    event.flags != LKMDBG_HWPOINT_TYPE_EXEC) {
		fprintf(stderr,
			"breakpoint event mismatch addr=0x%" PRIx64 " flags=0x%x\n",
			(uint64_t)event.value0, event.flags);
		remove_hwpoint(session_fd, bp_req.id);
		return -1;
	}
	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_BREAKPOINT,
			      &stop_req) < 0) {
		remove_hwpoint(session_fd, bp_req.id);
		return -1;
	}
	if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
	    !(stop_req.stop.flags & LKMDBG_STOP_FLAG_REARM_REQUIRED) ||
	    stop_req.stop.value0 != info->exec_target_addr) {
		fprintf(stderr,
			"breakpoint stop state mismatch flags=0x%x value0=0x%" PRIx64 "\n",
			stop_req.stop.flags, (uint64_t)stop_req.stop.value0);
		remove_hwpoint(session_fd, bp_req.id);
		return -1;
	}
	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0) {
		remove_hwpoint(session_fd, bp_req.id);
		return -1;
	}
	if (remove_hwpoint(session_fd, bp_req.id) < 0)
		return -1;

	if (add_hwpoint(session_fd, child, info->exec_target_addr,
			LKMDBG_HWPOINT_TYPE_EXEC, 4,
			LKMDBG_HWPOINT_FLAG_MMU_EXEC, &mmu_req) < 0)
		return -1;
	if (find_hwpoint_id(session_fd, mmu_req.id) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}

	if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_EXEC) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	printf("selftest runtime: waiting for mmu breakpoint stop event\n");
	if (wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
				   LKMDBG_STOP_REASON_BREAKPOINT,
				   event_timeout_ms, &event) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (event.value0 != info->exec_target_addr ||
	    event.flags != (LKMDBG_HWPOINT_TYPE_EXEC |
			    LKMDBG_HWPOINT_FLAG_MMU_EXEC) ||
	    !event.value1) {
		fprintf(stderr,
			"mmu breakpoint event mismatch addr=0x%" PRIx64 " ip=0x%" PRIx64 " flags=0x%x\n",
			(uint64_t)event.value0, (uint64_t)event.value1,
			event.flags);
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_BREAKPOINT,
			      &stop_req) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
	    !(stop_req.stop.flags & LKMDBG_STOP_FLAG_REARM_REQUIRED) ||
	    stop_req.stop.value0 != info->exec_target_addr ||
	    stop_req.stop.event_flags !=
		    (LKMDBG_HWPOINT_TYPE_EXEC |
		     LKMDBG_HWPOINT_FLAG_MMU_EXEC)) {
		fprintf(stderr,
			"mmu breakpoint stop state mismatch flags=0x%x event_flags=0x%x value0=0x%" PRIx64 "\n",
			stop_req.stop.flags, stop_req.stop.event_flags,
			(uint64_t)stop_req.stop.value0);
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}

	if (add_hwpoint_expect_errno(session_fd, child,
				     info->exec_target_addr + 4,
				     LKMDBG_HWPOINT_TYPE_EXEC, 4,
				     LKMDBG_HWPOINT_FLAG_MMU_EXEC,
				     EEXIST) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}

	if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_EXEC) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	printf("selftest runtime: verifying mmu breakpoint one-shot behavior\n");
	ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
				     LKMDBG_STOP_REASON_BREAKPOINT,
				     1000, &event);
	if (ret != -ETIMEDOUT) {
		if (!ret) {
			fprintf(stderr,
				"mmu breakpoint unexpectedly retriggered without rearm addr=0x%" PRIx64 " flags=0x%x\n",
				(uint64_t)event.value0, event.flags);
		}
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}

	if (rearm_hwpoint(session_fd, mmu_req.id, NULL) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}

	if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_EXEC) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	printf("selftest runtime: waiting for rearmed mmu breakpoint stop event\n");
	if (wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
				   LKMDBG_STOP_REASON_BREAKPOINT,
				   event_timeout_ms, &event) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (event.value0 != info->exec_target_addr ||
	    event.flags != (LKMDBG_HWPOINT_TYPE_EXEC |
			    LKMDBG_HWPOINT_FLAG_MMU_EXEC)) {
		fprintf(stderr,
			"rearmed mmu breakpoint event mismatch addr=0x%" PRIx64 " flags=0x%x\n",
			(uint64_t)event.value0, event.flags);
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_BREAKPOINT,
			      &stop_req) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_REARM_REQUIRED) ||
	    stop_req.stop.event_flags !=
		    (LKMDBG_HWPOINT_TYPE_EXEC |
		     LKMDBG_HWPOINT_FLAG_MMU_EXEC)) {
		fprintf(stderr,
			"rearmed mmu breakpoint stop state mismatch flags=0x%x event_flags=0x%x\n",
			stop_req.stop.flags, stop_req.stop.event_flags);
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0) {
		remove_hwpoint(session_fd, mmu_req.id);
		return -1;
	}
	if (remove_hwpoint(session_fd, mmu_req.id) < 0)
		return -1;

	{
		struct lkmdbg_hwpoint_request threshold_req;
		struct lkmdbg_hwpoint_request auto_req;
		struct lkmdbg_hwpoint_request mmu_skip_req;
		struct lkmdbg_hwpoint_entry entry;

		memset(&threshold_req, 0, sizeof(threshold_req));
		memset(&auto_req, 0, sizeof(auto_req));
		memset(&mmu_skip_req, 0, sizeof(mmu_skip_req));
		memset(&entry, 0, sizeof(entry));

		if (add_hwpoint_ex(session_fd, child, info->exec_target_addr,
				   LKMDBG_HWPOINT_TYPE_EXEC, 4, 0, 1,
				   LKMDBG_HWPOINT_ACTION_ONESHOT,
				   &threshold_req) < 0)
			return -1;
		if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_EXEC) < 0) {
			remove_hwpoint(session_fd, threshold_req.id);
			return -1;
		}
		ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
					     LKMDBG_STOP_REASON_BREAKPOINT,
					     event_timeout_ms, &event);
		if (ret < 0) {
			remove_hwpoint(session_fd, threshold_req.id);
			return -1;
		}
		if (query_hwpoint_entry_by_id(session_fd, threshold_req.id,
					      &entry) < 0 ||
		    entry.hits != 1 ||
		    entry.trigger_hit_count != 1 ||
		    entry.action_flags != LKMDBG_HWPOINT_ACTION_ONESHOT ||
		    (entry.state & LKMDBG_HWPOINT_STATE_ACTIVE)) {
			fprintf(stderr,
				"oneshot breakpoint state mismatch hits=%" PRIu64 " after=%" PRIu64 " actions=0x%x state=0x%x\n",
				(uint64_t)entry.hits,
				(uint64_t)entry.trigger_hit_count,
				entry.action_flags, entry.state);
			remove_hwpoint(session_fd, threshold_req.id);
			return -1;
		}
		if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_BREAKPOINT,
				      &stop_req) < 0) {
			remove_hwpoint(session_fd, threshold_req.id);
			return -1;
		}
		if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
				    NULL) < 0) {
			remove_hwpoint(session_fd, threshold_req.id);
			return -1;
		}
		if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_EXEC) < 0) {
			remove_hwpoint(session_fd, threshold_req.id);
			return -1;
		}
		ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
					     LKMDBG_STOP_REASON_BREAKPOINT,
					     1000, &event);
		if (ret != -ETIMEDOUT) {
			remove_hwpoint(session_fd, threshold_req.id);
			fprintf(stderr,
				"oneshot breakpoint retriggered after disarm ret=%d\n",
				ret);
			return -1;
		}
		if (query_hwpoint_entry_by_id(session_fd, threshold_req.id,
					      &entry) < 0 ||
		    entry.hits != 1 ||
		    (entry.state & LKMDBG_HWPOINT_STATE_ACTIVE)) {
			fprintf(stderr,
				"oneshot breakpoint failed to disarm hits=%" PRIu64 " state=0x%x\n",
				(uint64_t)entry.hits, entry.state);
			remove_hwpoint(session_fd, threshold_req.id);
			return -1;
		}
		if (remove_hwpoint(session_fd, threshold_req.id) < 0)
			return -1;

		if (add_hwpoint_ex(session_fd, child, info->watch_mmu_addr,
				   LKMDBG_HWPOINT_TYPE_WRITE, 8,
				   LKMDBG_HWPOINT_FLAG_MMU_EXEC, 1,
				   LKMDBG_HWPOINT_ACTION_ONESHOT |
					   LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE,
				   &auto_req) < 0)
			return -1;
		if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_WATCH_MMU) < 0) {
			remove_hwpoint(session_fd, auto_req.id);
			return -1;
		}
		ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
					     LKMDBG_STOP_REASON_WATCHPOINT,
					     1000, &event);
		if (ret != -ETIMEDOUT) {
			remove_hwpoint(session_fd, auto_req.id);
			fprintf(stderr,
				"mmu auto-continue watchpoint unexpectedly stopped ret=%d\n",
				ret);
			return -1;
		}
		if (query_hwpoint_entry_by_id(session_fd, auto_req.id, &entry) < 0 ||
		    entry.hits != 1 ||
		    entry.action_flags !=
			    (LKMDBG_HWPOINT_ACTION_ONESHOT |
			     LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE) ||
		    (entry.state & LKMDBG_HWPOINT_STATE_ACTIVE)) {
			fprintf(stderr,
				"mmu auto-continue watchpoint state mismatch hits=%" PRIu64 " actions=0x%x state=0x%x\n",
				(uint64_t)entry.hits, entry.action_flags,
				entry.state);
			remove_hwpoint(session_fd, auto_req.id);
			return -1;
		}
		if (remove_hwpoint(session_fd, auto_req.id) < 0)
			return -1;

		if (add_hwpoint_ex(session_fd, child, info->watch_mmu_addr,
				   LKMDBG_HWPOINT_TYPE_WRITE, 8,
				   LKMDBG_HWPOINT_FLAG_MMU_EXEC, 2, 0,
				   &mmu_skip_req) < 0)
			return -1;
		if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_WATCH_MMU) < 0) {
			remove_hwpoint(session_fd, mmu_skip_req.id);
			return -1;
		}
		ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
					     LKMDBG_STOP_REASON_WATCHPOINT,
					     1000, &event);
		if (ret != -ETIMEDOUT) {
			remove_hwpoint(session_fd, mmu_skip_req.id);
			fprintf(stderr,
				"mmu threshold watchpoint unexpectedly stopped on first hit ret=%d\n",
				ret);
			return -1;
		}
		if (query_hwpoint_entry_by_id(session_fd, mmu_skip_req.id,
					      &entry) < 0 ||
		    entry.hits != 1 ||
		    entry.trigger_hit_count != 2 ||
		    !(entry.state & LKMDBG_HWPOINT_STATE_ACTIVE)) {
			fprintf(stderr,
				"mmu threshold watchpoint state mismatch hits=%" PRIu64 " after=%" PRIu64 " state=0x%x\n",
				(uint64_t)entry.hits,
				(uint64_t)entry.trigger_hit_count, entry.state);
			remove_hwpoint(session_fd, mmu_skip_req.id);
			return -1;
		}
		if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_WATCH_MMU) < 0) {
			remove_hwpoint(session_fd, mmu_skip_req.id);
			return -1;
		}
		if (wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
					   LKMDBG_STOP_REASON_WATCHPOINT,
					   event_timeout_ms, &event) < 0) {
			remove_hwpoint(session_fd, mmu_skip_req.id);
			return -1;
		}
		if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_WATCHPOINT,
				      &stop_req) < 0) {
			remove_hwpoint(session_fd, mmu_skip_req.id);
			return -1;
		}
		if (stop_req.stop.event_flags !=
		    (LKMDBG_HWPOINT_TYPE_WRITE |
		     LKMDBG_HWPOINT_FLAG_MMU_EXEC)) {
			fprintf(stderr,
				"mmu threshold watchpoint stop flags mismatch event_flags=0x%x\n",
				stop_req.stop.event_flags);
			remove_hwpoint(session_fd, mmu_skip_req.id);
			return -1;
		}
		if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
				    NULL) < 0) {
			remove_hwpoint(session_fd, mmu_skip_req.id);
			return -1;
		}
		if (remove_hwpoint(session_fd, mmu_skip_req.id) < 0)
			return -1;
	}

	if (add_hwpoint(session_fd, child, info->watch_addr,
			LKMDBG_HWPOINT_TYPE_WRITE, 8, 0, &wp_req) < 0)
		return -1;
	if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_WATCH) < 0) {
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	}
	printf("selftest runtime: waiting for watchpoint stop event\n");
	ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
				     LKMDBG_STOP_REASON_WATCHPOINT,
				     event_timeout_ms, &event);
	if (ret == -ETIMEDOUT) {
		printf("selftest runtime: watchpoint event unavailable, continuing\n");
		if (remove_hwpoint(session_fd, wp_req.id) < 0)
			return -1;
	} else if (ret < 0) {
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	} else if (event.value0 != info->watch_addr ||
		   event.flags != LKMDBG_HWPOINT_TYPE_WRITE) {
		fprintf(stderr,
			"watchpoint event mismatch addr=0x%" PRIx64 " flags=0x%x\n",
			(uint64_t)event.value0, event.flags);
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	} else if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_WATCHPOINT,
				     &stop_req) < 0) {
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	} else if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
		   !(stop_req.stop.flags & LKMDBG_STOP_FLAG_REARM_REQUIRED) ||
		   stop_req.stop.value0 != info->watch_addr) {
		fprintf(stderr,
			"watchpoint stop state mismatch flags=0x%x value0=0x%" PRIx64 "\n",
			stop_req.stop.flags, (uint64_t)stop_req.stop.value0);
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	} else if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
				   NULL) < 0) {
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	}
	if (remove_hwpoint(session_fd, wp_req.id) < 0)
		return -1;

	printf("selftest runtime events ok clone signal breakpoint mmu-breakpoint oneshot rearm watchpoint threshold actions\n");
	return 0;
}

static int verify_single_step_event(int session_fd, const struct child_info *info)
{
	struct lkmdbg_freeze_request req;
	struct lkmdbg_thread_entry parked_entry;
	struct lkmdbg_event_record event;
	struct lkmdbg_stop_query_request stop_req;
	pid_t tids[SELFTEST_FREEZE_THREADS];
	unsigned int i;
	unsigned int parked_index = UINT32_MAX;
	const int event_timeout_ms = 5000;

	memset(&req, 0, sizeof(req));
	memset(&parked_entry, 0, sizeof(parked_entry));
	memset(&stop_req, 0, sizeof(stop_req));
	memset(tids, 0, sizeof(tids));

	if (wait_for_freeze_thread_tids(session_fd, info, tids) < 0)
		return -1;
	if (freeze_target_threads(session_fd, 2000, &req, 0) < 0)
		return -1;
	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_FREEZE, &stop_req) <
	    0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	for (i = 0; i < info->freeze_thread_count; i++) {
		if (find_target_thread_entry(session_fd, tids[i], &parked_entry) < 0) {
			thaw_target_threads(session_fd, 2000, NULL, 0);
			return -1;
		}
		if (!(parked_entry.flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED))
			continue;
		parked_index = i;
		break;
	}

	if (parked_index == UINT32_MAX) {
		fprintf(stderr, "no parked thread available for single-step\n");
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (drain_session_events(session_fd) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (single_step_thread(session_fd, tids[parked_index]) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0)
		return -1;

	printf("selftest runtime: waiting for single-step stop event\n");
	if (wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
				   LKMDBG_STOP_REASON_SINGLE_STEP,
				   event_timeout_ms,
				   &event) < 0)
		return -1;
	if (event.tid != tids[parked_index]) {
		fprintf(stderr, "single-step event tid mismatch got=%d expected=%d\n",
			event.tid, tids[parked_index]);
		return -1;
	}
	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_SINGLE_STEP,
			      &stop_req) < 0)
		return -1;
	if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
	    !(stop_req.stop.flags & LKMDBG_STOP_FLAG_REGS_VALID)) {
		fprintf(stderr, "single-step stop flags mismatch flags=0x%x\n",
			stop_req.stop.flags);
		return -1;
	}
	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0)
		return -1;

	printf("selftest single-step event ok tid=%d pc=0x%" PRIx64 "\n",
	       event.tid, (uint64_t)event.value0);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s selftest\n"
		"  %s read <pid> <remote_addr_hex> <length>\n"
		"  %s threads <pid>\n"
		"  %s images <pid>\n"
		"  %s getregs <pid> <tid>\n"
		"  %s setreg <pid> <tid> <reg> <value_hex>\n"
		"  %s hwadd <pid> <tid> <r|w|rw|x|rx|wx|rwx> <addr_hex> <len> [stop|counter|mmu|flags] [after_hits] [oneshot|autocont|actions]\n"
		"  %s hwdel <pid> <id>\n"
		"  %s hwrearm <pid> <id>\n"
		"  %s hwlist <pid>\n"
		"  %s stop <pid>\n"
		"  %s cont <pid> [timeout_ms] [rearm|norearm]\n"
		"  %s step <pid> <tid>\n"
		"  %s freeze <pid> [timeout_ms]\n"
		"  %s thaw <pid> [timeout_ms]\n"
		"  %s events <pid> [max_events] [timeout_ms]\n"
		"  %s vmas <pid>\n"
		"  %s pages <pid> <remote_addr_hex> <length>\n"
		"  %s ptset <pid> <remote_addr_hex> <ro|rw|rx|rwx|protnone|execonly|raw:<pte_hex>>\n"
		"  %s ptdel <pid> <id>\n"
		"  %s ptlist <pid>\n"
		"  %s physread <phys_addr_hex> <length>\n"
		"  %s physwrite <phys_addr_hex> <ascii_data>\n"
		"  %s physreadv <pid> <remote_addr_hex> <length>\n"
		"  %s physwritev <pid> <remote_addr_hex> <ascii_data>\n"
		"  %s write <pid> <remote_addr_hex> <ascii_data>\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog, prog);
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
	unsigned char force_buf[64];
	char force_write_buf[64];
	const char *payload = "patched-by-lkmdbg";
	const char *force_write_payload = "force-write-patched";
	const char *partial_payload = "partial-write-ok";
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

	printf("selftest child pid=%d basic=0x%llx slots=0x%llx nofault=0x%llx force_read=0x%llx force_write=0x%llx file=0x%llx\n",
	       child, (unsigned long long)info.basic_addr,
	       (unsigned long long)info.slots_addr,
	       (unsigned long long)info.nofault_addr,
	       (unsigned long long)info.force_read_addr,
	       (unsigned long long)info.force_write_addr,
	       (unsigned long long)info.file_addr);

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

	if (verify_batch_event_read(session_fd) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (drain_session_events(session_fd) < 0) {
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

	memset(force_buf, 0xCC, sizeof(force_buf));
	if (read_target_memory(session_fd, info.force_read_addr, force_buf,
			       sizeof(force_buf), &bytes_done, 1) < 0 ||
	    bytes_done != 0) {
		fprintf(stderr,
			"protected READ_MEM bytes_done=%u expected=0\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	memset(force_buf, 0, sizeof(force_buf));
	if (read_target_memory_flags(session_fd, info.force_read_addr, force_buf,
				     sizeof(force_buf),
				     LKMDBG_MEM_OP_FLAG_FORCE_ACCESS,
				     &bytes_done, 1) < 0 ||
	    bytes_done != sizeof(force_buf) ||
	    verify_pattern(force_buf, sizeof(force_buf), 3) != 0) {
		fprintf(stderr,
			"force READ_MEM verify failed bytes_done=%u\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	memset(force_write_buf, 0, sizeof(force_write_buf));
	if (write_target_memory(session_fd, info.force_write_addr,
				force_write_payload,
				strlen(force_write_payload) + 1, &bytes_done,
				1) < 0 ||
	    bytes_done != 0) {
		fprintf(stderr,
			"protected WRITE_MEM bytes_done=%u expected=0\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (write_target_memory_flags(session_fd, info.force_write_addr,
				      force_write_payload,
				      strlen(force_write_payload) + 1,
				      LKMDBG_MEM_OP_FLAG_FORCE_ACCESS,
				      &bytes_done, 1) < 0 ||
	    bytes_done != strlen(force_write_payload) + 1) {
		fprintf(stderr,
			"force WRITE_MEM verify failed bytes_done=%u\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	memset(force_write_buf, 0, sizeof(force_write_buf));
	if (read_target_memory(session_fd, info.force_write_addr,
			       force_write_buf,
			       strlen(force_write_payload) + 1, &bytes_done,
			       0) < 0 ||
	    bytes_done != strlen(force_write_payload) + 1 ||
	    strcmp(force_write_buf, force_write_payload) != 0) {
		fprintf(stderr,
			"force write readback failed bytes_done=%u\n",
			bytes_done);
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	printf("selftest force-read and force-write on protected present pages ok\n");

	if (verify_remote_map(session_fd, &info, cmd_pipe[1], resp_pipe[0]) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_page_query(session_fd, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_pte_patch_api(session_fd, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_physical_memory_api(session_fd, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_vma_query(session_fd, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_image_query(session_fd, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_thread_freeze(session_fd, child, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_runtime_events(session_fd, cmd_pipe[1], child, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_single_step_event(session_fd, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (expect_partial_write_progress(session_fd, info.basic_addr,
					  partial_payload) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	printf("selftest partial batch progress survives runtime write fault\n");

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
	pid_t tid = 0;
	uintptr_t remote_addr = 0;
	uint64_t phys_addr = 0;
	char *endp;
	int needs_remote_addr;
	int needs_target = 1;
	pid_t target_tid = 0;
	uint32_t timeout_ms = 0;
	uint64_t reg_value = 0;
	uint64_t hwpoint_id = 0;
	uint64_t hwpoint_addr = 0;
	uint64_t hwpoint_trigger_hits = 1;
	uint32_t hwpoint_type = 0;
	uint32_t hwpoint_len = 0;
	uint32_t hwpoint_flags = 0;
	uint32_t hwpoint_action_flags = 0;
	uint64_t pte_patch_id = 0;
	uint64_t pte_patch_raw = 0;
	uint32_t pte_patch_mode = 0;
	uint32_t pte_patch_flags = 0;
	uint32_t continue_flags = 0;
	unsigned int max_events = EVENT_READ_BATCH;

	if (argc == 2 && strcmp(argv[1], "selftest") == 0)
		return run_selftest(argv[0]);

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "physread") == 0 || strcmp(argv[1], "physwrite") == 0)
		needs_target = 0;

	if (needs_target) {
		pid = (pid_t)strtol(argv[2], &endp, 10);
		if (*endp != '\0' || pid <= 0) {
			fprintf(stderr, "invalid pid: %s\n", argv[2]);
			return 1;
		}
	} else {
		pid = 0;
	}

	needs_remote_addr = strcmp(argv[1], "read") == 0 ||
			    strcmp(argv[1], "write") == 0 ||
			    strcmp(argv[1], "pages") == 0 ||
			    strcmp(argv[1], "ptset") == 0 ||
			    strcmp(argv[1], "physreadv") == 0 ||
			    strcmp(argv[1], "physwritev") == 0;
	if (needs_remote_addr) {
		if (argc < 5) {
			usage(argv[0]);
			return 1;
		}

		remote_addr = (uintptr_t)strtoull(argv[3], &endp, 16);
		if (*endp != '\0') {
			fprintf(stderr, "invalid remote address: %s\n", argv[3]);
			return 1;
		}
	}

	if (!needs_target) {
		if (argc < 4) {
			usage(argv[0]);
			return 1;
		}

		phys_addr = strtoull(argv[2], &endp, 16);
		if (*endp != '\0') {
			fprintf(stderr, "invalid physical address: %s\n", argv[2]);
			return 1;
		}
	}

	if (strcmp(argv[1], "getregs") == 0 || strcmp(argv[1], "setreg") == 0) {
		if (argc < 4) {
			usage(argv[0]);
			return 1;
		}

		tid = (pid_t)strtol(argv[3], &endp, 10);
		if (*endp != '\0' || tid <= 0) {
			fprintf(stderr, "invalid tid: %s\n", argv[3]);
			return 1;
		}
		target_tid = tid;
	}

	if (strcmp(argv[1], "setreg") == 0) {
		if (argc < 6) {
			usage(argv[0]);
			return 1;
		}

		reg_value = strtoull(argv[5], &endp, 16);
		if (*endp != '\0') {
			fprintf(stderr, "invalid register value: %s\n", argv[5]);
			return 1;
		}
	}

	if (strcmp(argv[1], "step") == 0) {
		if (argc < 4) {
			usage(argv[0]);
			return 1;
		}

		tid = (pid_t)strtol(argv[3], &endp, 10);
		if (*endp != '\0' || tid <= 0) {
			fprintf(stderr, "invalid tid: %s\n", argv[3]);
			return 1;
		}
		target_tid = tid;
	}

	if (strcmp(argv[1], "cont") == 0) {
		continue_flags = LKMDBG_CONTINUE_FLAG_REARM_HWPOINTS;
		if (argc >= 4) {
			timeout_ms = (uint32_t)strtoul(argv[3], &endp, 0);
			if (*endp != '\0') {
				if (strcmp(argv[3], "rearm") == 0)
					timeout_ms = 0;
				else if (strcmp(argv[3], "norearm") == 0) {
					timeout_ms = 0;
					continue_flags = 0;
				} else {
					fprintf(stderr, "invalid timeout: %s\n",
						argv[3]);
					return 1;
				}
			}
		}
		if (argc >= 5) {
			if (strcmp(argv[4], "rearm") == 0)
				continue_flags =
					LKMDBG_CONTINUE_FLAG_REARM_HWPOINTS;
			else if (strcmp(argv[4], "norearm") == 0)
				continue_flags = 0;
			else {
				fprintf(stderr, "invalid continue mode: %s\n",
					argv[4]);
				return 1;
			}
		}
	}

	if (strcmp(argv[1], "events") == 0) {
		if (argc >= 4) {
			max_events = (unsigned int)strtoul(argv[3], &endp, 0);
			if (*endp != '\0' || !max_events) {
				fprintf(stderr, "invalid max_events: %s\n",
					argv[3]);
				return 1;
			}
		}
		if (argc >= 5) {
			timeout_ms = (uint32_t)strtoul(argv[4], &endp, 0);
			if (*endp != '\0') {
				fprintf(stderr, "invalid timeout: %s\n", argv[4]);
				return 1;
			}
		}
	}

	if (strcmp(argv[1], "hwadd") == 0) {
		if (argc < 7) {
			usage(argv[0]);
			return 1;
		}

		tid = (pid_t)strtol(argv[3], &endp, 10);
		if (*endp != '\0' || tid <= 0) {
			fprintf(stderr, "invalid tid: %s\n", argv[3]);
			return 1;
		}
		target_tid = tid;

		if (parse_hwpoint_type(argv[4], &hwpoint_type) < 0) {
			fprintf(stderr, "invalid hwpoint type: %s\n", argv[4]);
			return 1;
		}

		hwpoint_addr = strtoull(argv[5], &endp, 16);
		if (*endp != '\0') {
			fprintf(stderr, "invalid hwpoint address: %s\n", argv[5]);
			return 1;
		}

		hwpoint_len = (uint32_t)strtoul(argv[6], &endp, 0);
		if (*endp != '\0' || !hwpoint_len) {
			fprintf(stderr, "invalid hwpoint length: %s\n", argv[6]);
			return 1;
		}

		if (argc >= 8 &&
		    parse_hwpoint_flags(argv[7], &hwpoint_flags) < 0) {
			fprintf(stderr, "invalid hwpoint flags: %s\n", argv[7]);
			return 1;
		}

		if (argc >= 9) {
			hwpoint_trigger_hits = strtoull(argv[8], &endp, 0);
			if (*endp != '\0' || !hwpoint_trigger_hits) {
				fprintf(stderr, "invalid trigger hit count: %s\n",
					argv[8]);
				return 1;
			}
		}

		if (argc >= 10 &&
		    parse_hwpoint_actions(argv[9], &hwpoint_action_flags) < 0) {
			fprintf(stderr, "invalid hwpoint actions: %s\n", argv[9]);
			return 1;
		}
	}

	if (strcmp(argv[1], "hwdel") == 0 || strcmp(argv[1], "hwrearm") == 0) {
		if (argc < 4) {
			usage(argv[0]);
			return 1;
		}

		hwpoint_id = strtoull(argv[3], &endp, 0);
		if (*endp != '\0' || !hwpoint_id) {
			fprintf(stderr, "invalid hwpoint id: %s\n", argv[3]);
			return 1;
		}
	}

	if (strcmp(argv[1], "ptset") == 0) {
		if (argc < 5) {
			usage(argv[0]);
			return 1;
		}

		if (parse_pte_mode(argv[4], &pte_patch_mode, &pte_patch_flags,
				   &pte_patch_raw) < 0) {
			fprintf(stderr, "invalid PTE patch mode: %s\n", argv[4]);
			return 1;
		}
	}

	if (strcmp(argv[1], "ptdel") == 0) {
		if (argc < 4) {
			usage(argv[0]);
			return 1;
		}

		pte_patch_id = strtoull(argv[3], &endp, 0);
		if (*endp != '\0' || !pte_patch_id) {
			fprintf(stderr, "invalid PTE patch id: %s\n", argv[3]);
			return 1;
		}
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		return 1;

	if (needs_target && set_target_ex(session_fd, pid, target_tid) < 0) {
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
	} else if (strcmp(argv[1], "threads") == 0) {
		if (dump_target_threads(session_fd) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "images") == 0) {
		if (dump_target_images(session_fd) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "getregs") == 0) {
		if (get_and_print_regs(session_fd, tid) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "setreg") == 0) {
		if (set_and_verify_reg(session_fd, tid, argv[4], reg_value) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "hwadd") == 0) {
		struct lkmdbg_hwpoint_request reply;
		char type_buf[32];
		char flags_buf[32];
		char action_buf[32];

		memset(&reply, 0, sizeof(reply));
		if (add_hwpoint_ex(session_fd, tid, hwpoint_addr, hwpoint_type,
				   hwpoint_len, hwpoint_flags,
				   hwpoint_trigger_hits,
				   hwpoint_action_flags, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("hwpoint.id=%" PRIu64 "\n", (uint64_t)reply.id);
		printf("hwpoint.tid=%d\n", reply.tid);
		printf("hwpoint.type=0x%x(%s)\n", hwpoint_type,
		       describe_hwpoint_type(hwpoint_type, type_buf,
					      sizeof(type_buf)));
		printf("hwpoint.flags=0x%x(%s)\n", reply.flags,
		       describe_hwpoint_flags(reply.flags, flags_buf,
					       sizeof(flags_buf)));
		printf("hwpoint.after_hits=%" PRIu64 "\n",
		       (uint64_t)reply.trigger_hit_count);
		printf("hwpoint.actions=0x%x(%s)\n", reply.action_flags,
		       describe_hwpoint_actions(reply.action_flags, action_buf,
						sizeof(action_buf)));
	} else if (strcmp(argv[1], "hwdel") == 0) {
		if (remove_hwpoint(session_fd, hwpoint_id) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "hwrearm") == 0) {
		struct lkmdbg_hwpoint_request reply;
		char flags_buf[32];
		char action_buf[32];

		memset(&reply, 0, sizeof(reply));
		if (rearm_hwpoint(session_fd, hwpoint_id, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("hwpoint.id=%" PRIu64 "\n", (uint64_t)reply.id);
		printf("hwpoint.tid=%d\n", reply.tid);
		printf("hwpoint.flags=0x%x(%s)\n", reply.flags,
		       describe_hwpoint_flags(reply.flags, flags_buf,
					       sizeof(flags_buf)));
		printf("hwpoint.after_hits=%" PRIu64 "\n",
		       (uint64_t)reply.trigger_hit_count);
		printf("hwpoint.actions=0x%x(%s)\n", reply.action_flags,
		       describe_hwpoint_actions(reply.action_flags, action_buf,
						sizeof(action_buf)));
	} else if (strcmp(argv[1], "hwlist") == 0) {
		struct lkmdbg_hwpoint_entry entries[16];
		uint64_t cursor = 0;

		for (;;) {
			struct lkmdbg_hwpoint_query_request reply;
			uint32_t i;

			if (query_hwpoints(session_fd, cursor, entries,
					   (uint32_t)(sizeof(entries) /
						      sizeof(entries[0])),
					   &reply) < 0) {
				close(session_fd);
				return 1;
			}

			for (i = 0; i < reply.entries_filled; i++) {
				char type_buf[32];
				char flags_buf[32];
				char action_buf[32];
				char state_buf[48];

				printf("id=%" PRIu64 " tgid=%d tid=%d type=0x%x(%s) len=%u flags=0x%x(%s) actions=0x%x(%s) after_hits=%" PRIu64 " state=0x%x(%s) hits=%" PRIu64 " addr=0x%" PRIx64 "\n",
				       (uint64_t)entries[i].id, entries[i].tgid,
				       entries[i].tid, entries[i].type,
				       describe_hwpoint_type(entries[i].type,
							      type_buf,
							      sizeof(type_buf)),
				       entries[i].len, entries[i].flags,
				       describe_hwpoint_flags(entries[i].flags,
							       flags_buf,
							       sizeof(flags_buf)),
				       entries[i].action_flags,
				       describe_hwpoint_actions(
					       entries[i].action_flags,
					       action_buf,
					       sizeof(action_buf)),
				       (uint64_t)entries[i].trigger_hit_count,
				       entries[i].state,
				       describe_hwpoint_state(entries[i].state,
							       state_buf,
							       sizeof(state_buf)),
				       (uint64_t)entries[i].hits,
				       (uint64_t)entries[i].addr);
			}

			if (reply.done)
				break;
			cursor = reply.next_id;
		}
	} else if (strcmp(argv[1], "step") == 0) {
		struct lkmdbg_stop_query_request stop_req;

		memset(&stop_req, 0, sizeof(stop_req));
		if (freeze_target_threads(session_fd, 2000, NULL, 1) < 0) {
			close(session_fd);
			return 1;
		}
		if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_FREEZE,
				      &stop_req) < 0) {
			close(session_fd);
			return 1;
		}
		if (drain_session_events(session_fd) < 0) {
			close(session_fd);
			return 1;
		}
		if (single_step_thread(session_fd, tid) < 0) {
			thaw_target_threads(session_fd, 2000, NULL, 0);
			close(session_fd);
			return 1;
		}
		if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
				    NULL) < 0) {
			close(session_fd);
			return 1;
		}
		{
			struct lkmdbg_event_record event;

			if (wait_for_session_event(session_fd,
						   LKMDBG_EVENT_TARGET_STOP,
						   LKMDBG_STOP_REASON_SINGLE_STEP,
						   2000, &event) < 0) {
				close(session_fd);
				return 1;
			}
			printf("single_step.tid=%d\n", event.tid);
			printf("single_step.pc=0x%" PRIx64 "\n",
			       (uint64_t)event.value0);
		}
	} else if (strcmp(argv[1], "stop") == 0) {
		struct lkmdbg_stop_query_request stop_req;

		memset(&stop_req, 0, sizeof(stop_req));
		if (get_stop_state(session_fd, &stop_req) < 0) {
			close(session_fd);
			return 1;
		}
		print_stop_state(&stop_req.stop);
	} else if (strcmp(argv[1], "cont") == 0) {
		struct lkmdbg_continue_request reply;
		struct lkmdbg_stop_query_request stop_req;

		memset(&reply, 0, sizeof(reply));
		memset(&stop_req, 0, sizeof(stop_req));
		if (get_stop_state(session_fd, &stop_req) < 0) {
			close(session_fd);
			return 1;
		}
		if (continue_target(session_fd, stop_req.stop.cookie, timeout_ms,
				    continue_flags, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("continue.total=%u\n", reply.threads_total);
		printf("continue.settled=%u\n", reply.threads_settled);
		printf("continue.parked=%u\n", reply.threads_parked);
	} else if (strcmp(argv[1], "write") == 0) {
		const char *data = argv[4];
		size_t len = strlen(data);

		if (write_target_memory(session_fd, remote_addr, data, len, NULL,
					1) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "freeze") == 0) {
		if (argc >= 4) {
			timeout_ms = (uint32_t)strtoul(argv[3], &endp, 0);
			if (*endp != '\0') {
				fprintf(stderr, "invalid timeout: %s\n", argv[3]);
				close(session_fd);
				return 1;
			}
		}

		if (freeze_target_threads(session_fd, timeout_ms, NULL, 1) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "thaw") == 0) {
		if (argc >= 4) {
			timeout_ms = (uint32_t)strtoul(argv[3], &endp, 0);
			if (*endp != '\0') {
				fprintf(stderr, "invalid timeout: %s\n", argv[3]);
				close(session_fd);
				return 1;
			}
		}

		if (thaw_target_threads(session_fd, timeout_ms, NULL, 1) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "events") == 0) {
		if (dump_session_events(session_fd, max_events, timeout_ms) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "vmas") == 0) {
		if (dump_target_vmas(session_fd) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "pages") == 0) {
		uint64_t length;

		length = strtoull(argv[4], &endp, 0);
		if (*endp != '\0' || !length) {
			fprintf(stderr, "invalid length: %s\n", argv[4]);
			close(session_fd);
			return 1;
		}
		if (dump_target_pages(session_fd, remote_addr, length) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "ptset") == 0) {
		struct lkmdbg_pte_patch_request reply;

		memset(&reply, 0, sizeof(reply));
		if (apply_pte_patch(session_fd, remote_addr, pte_patch_mode,
				    pte_patch_flags, pte_patch_raw,
				    &reply) < 0) {
			close(session_fd);
			return 1;
		}
		print_pte_patch_reply("pte_patch", &reply);
	} else if (strcmp(argv[1], "ptdel") == 0) {
		struct lkmdbg_pte_patch_request reply;

		memset(&reply, 0, sizeof(reply));
		if (remove_pte_patch(session_fd, pte_patch_id, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		print_pte_patch_reply("pte_patch", &reply);
	} else if (strcmp(argv[1], "ptlist") == 0) {
		if (dump_target_pte_patches(session_fd) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "physread") == 0) {
		size_t len;
		unsigned char *buf;
		size_t i;
		uint32_t bytes_done = 0;

		len = (size_t)strtoul(argv[3], &endp, 0);
		if (*endp != '\0' || len == 0) {
			fprintf(stderr, "invalid length: %s\n", argv[3]);
			close(session_fd);
			return 1;
		}

		buf = calloc(1, len);
		if (!buf) {
			fprintf(stderr, "calloc failed\n");
			close(session_fd);
			return 1;
		}

		if (read_physical_memory(session_fd, phys_addr, buf, len,
					 &bytes_done, 1) < 0) {
			free(buf);
			close(session_fd);
			return 1;
		}
		if (bytes_done != len) {
			fprintf(stderr,
				"short physical read bytes_done=%u expected=%zu\n",
				bytes_done, len);
			free(buf);
			close(session_fd);
			return 1;
		}

		for (i = 0; i < len; i++)
			printf("%02x", buf[i]);
		printf("\n");
		free(buf);
	} else if (strcmp(argv[1], "physwrite") == 0) {
		const char *data = argv[3];
		size_t len = strlen(data);
		uint32_t bytes_done = 0;

		if (write_physical_memory(session_fd, phys_addr, data, len,
					  &bytes_done, 1) < 0) {
			close(session_fd);
			return 1;
		}
		if (bytes_done != len) {
			fprintf(stderr,
				"short physical write bytes_done=%u expected=%zu\n",
				bytes_done, len);
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "physreadv") == 0) {
		size_t len;
		unsigned char *buf;
		size_t i;
		uint32_t bytes_done = 0;

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

		if (read_physical_memory_flags(session_fd, remote_addr, buf, len,
					       LKMDBG_PHYS_OP_FLAG_TARGET_VADDR,
					       &bytes_done, 1) < 0) {
			free(buf);
			close(session_fd);
			return 1;
		}
		if (bytes_done != len) {
			fprintf(stderr,
				"short translated physical read bytes_done=%u expected=%zu\n",
				bytes_done, len);
			free(buf);
			close(session_fd);
			return 1;
		}

		for (i = 0; i < len; i++)
			printf("%02x", buf[i]);
		printf("\n");
		free(buf);
	} else if (strcmp(argv[1], "physwritev") == 0) {
		const char *data = argv[4];
		size_t len = strlen(data);
		uint32_t bytes_done = 0;

		if (write_physical_memory_flags(session_fd, remote_addr, data, len,
						LKMDBG_PHYS_OP_FLAG_TARGET_VADDR,
						&bytes_done, 1) < 0) {
			close(session_fd);
			return 1;
		}
		if (bytes_done != len) {
			fprintf(stderr,
				"short translated physical write bytes_done=%u expected=%zu\n",
				bytes_done, len);
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
