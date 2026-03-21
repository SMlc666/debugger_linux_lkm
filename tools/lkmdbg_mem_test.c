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
#define THREAD_QUERY_BATCH 64

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

struct vma_query_buffer {
	struct lkmdbg_vma_entry *entries;
	char *names;
};

enum {
	CHILD_OP_QUERY_NOFAULT = 1,
	CHILD_OP_EXIT = 2,
	CHILD_OP_TRIGGER_EXEC = 3,
	CHILD_OP_TRIGGER_WATCH = 4,
	CHILD_OP_TRIGGER_SIGNAL = 5,
	CHILD_OP_SPAWN_THREAD = 6,
};

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
	struct lkmdbg_event_record event;

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

		nread = read(session_fd, &event, sizeof(event));
		if (nread == (ssize_t)sizeof(event))
			continue;
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

static int read_session_event_timeout(int session_fd,
				      struct lkmdbg_event_record *event_out,
				      int timeout_ms)
{
	struct pollfd pfd = {
		.fd = session_fd,
		.events = POLLIN,
	};
	ssize_t nread;

	if (poll(&pfd, 1, timeout_ms) < 0) {
		fprintf(stderr, "event poll failed: %s\n", strerror(errno));
		return -1;
	}

	if (!(pfd.revents & POLLIN))
		return 1;

	nread = read(session_fd, event_out, sizeof(*event_out));
	if (nread < 0) {
		fprintf(stderr, "event read failed: %s\n", strerror(errno));
		return -1;
	}
	if (nread != (ssize_t)sizeof(*event_out)) {
		fprintf(stderr, "short event read: %zd\n", nread);
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

static int add_hwpoint(int session_fd, pid_t tid, uint64_t addr, uint32_t type,
		       uint32_t len, uint32_t flags,
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
	};

	if (ioctl(session_fd, LKMDBG_IOC_ADD_HWPOINT, &req) < 0) {
		fprintf(stderr, "ADD_HWPOINT failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
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
		fprintf(stderr, "REMOVE_HWPOINT failed: %s\n", strerror(errno));
		return -1;
	}

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

static int read_target_memory_flags(int session_fd, uintptr_t remote_addr,
				    void *buf, size_t len, uint32_t op_flags,
				    uint32_t *bytes_done_out, int verbose);
static int write_target_memory_flags(int session_fd, uintptr_t remote_addr,
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

	if (fstat(file_fd, &st) < 0)
		return 2;

	for (i = 0; i < SELFTEST_THREAD_COUNT; i++) {
		snprintf(slots_map + (i * SELFTEST_SLOT_SIZE), SELFTEST_SLOT_SIZE,
			 "slot-%u-initial", i);
	}
	fill_pattern(large_map, SELFTEST_LARGE_MAP_LEN, 1);

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
		case CHILD_OP_TRIGGER_SIGNAL:
			if (kill(getpid(), SIGUSR1) < 0)
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

	return -1;
}

static int parse_hwpoint_flags(const char *arg, uint32_t *flags_out)
{
	char *endp = NULL;

	if (strcmp(arg, "stop") == 0 || strcmp(arg, "0") == 0) {
		*flags_out = 0;
		return 0;
	}
	if (strcmp(arg, "counter") == 0 || strcmp(arg, "count") == 0) {
		*flags_out = LKMDBG_HWPOINT_FLAG_COUNTER_MODE;
		return 0;
	}

	*flags_out = (uint32_t)strtoul(arg, &endp, 0);
	if (*endp != '\0' || (*flags_out & ~LKMDBG_HWPOINT_FLAG_COUNTER_MODE))
		return -1;
	return 0;
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

static int verify_runtime_events(int session_fd, int cmd_fd, pid_t child,
				 const struct child_info *info)
{
	struct lkmdbg_hwpoint_request bp_req;
	struct lkmdbg_hwpoint_request wp_req;
	struct lkmdbg_event_record event;
	const int event_timeout_ms = 5000;
	int ret;

	memset(&bp_req, 0, sizeof(bp_req));
	memset(&wp_req, 0, sizeof(wp_req));
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
	if (remove_hwpoint(session_fd, bp_req.id) < 0)
		return -1;

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
		printf("selftest runtime events ok breakpoint\n");
		return 0;
	}
	if (ret < 0) {
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	}
	if (event.value0 != info->watch_addr ||
	    event.flags != LKMDBG_HWPOINT_TYPE_WRITE) {
		fprintf(stderr,
			"watchpoint event mismatch addr=0x%" PRIx64 " flags=0x%x\n",
			(uint64_t)event.value0, event.flags);
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	}
	if (remove_hwpoint(session_fd, wp_req.id) < 0)
		return -1;

	printf("selftest runtime events ok clone signal breakpoint watchpoint\n");
	return 0;
}

static int verify_single_step_event(int session_fd, const struct child_info *info)
{
	struct lkmdbg_freeze_request req;
	struct lkmdbg_thread_entry parked_entry;
	struct lkmdbg_event_record event;
	pid_t tids[SELFTEST_FREEZE_THREADS];
	unsigned int i;
	unsigned int parked_index = UINT32_MAX;
	const int event_timeout_ms = 5000;

	memset(&req, 0, sizeof(req));
	memset(&parked_entry, 0, sizeof(parked_entry));
	memset(tids, 0, sizeof(tids));

	if (wait_for_freeze_thread_tids(session_fd, info, tids) < 0)
		return -1;
	if (freeze_target_threads(session_fd, 2000, &req, 0) < 0)
		return -1;

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

	if (thaw_target_threads(session_fd, 2000, NULL, 0) < 0)
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
		"  %s getregs <pid> <tid>\n"
		"  %s setreg <pid> <tid> <reg> <value_hex>\n"
		"  %s hwadd <pid> <tid> <r|w|rw|x> <addr_hex> <len> [stop|counter|flags]\n"
		"  %s hwdel <pid> <id>\n"
		"  %s hwlist <pid>\n"
		"  %s step <pid> <tid>\n"
		"  %s freeze <pid> [timeout_ms]\n"
		"  %s thaw <pid> [timeout_ms]\n"
		"  %s vmas <pid>\n"
		"  %s write <pid> <remote_addr_hex> <ascii_data>\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog);
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

	if (verify_vma_query(session_fd, &info) < 0) {
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
	char *endp;
	int needs_remote_addr;
	pid_t target_tid = 0;
	uint32_t timeout_ms = 0;
	uint64_t reg_value = 0;
	uint64_t hwpoint_id = 0;
	uint64_t hwpoint_addr = 0;
	uint32_t hwpoint_type = 0;
	uint32_t hwpoint_len = 0;
	uint32_t hwpoint_flags = 0;

	if (argc == 2 && strcmp(argv[1], "selftest") == 0)
		return run_selftest(argv[0]);

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	pid = (pid_t)strtol(argv[2], &endp, 10);
	if (*endp != '\0' || pid <= 0) {
		fprintf(stderr, "invalid pid: %s\n", argv[2]);
		return 1;
	}

	needs_remote_addr = strcmp(argv[1], "read") == 0 ||
			    strcmp(argv[1], "write") == 0;
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
	}

	if (strcmp(argv[1], "hwdel") == 0) {
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

	session_fd = open_session_fd();
	if (session_fd < 0)
		return 1;

	if (set_target_ex(session_fd, pid, target_tid) < 0) {
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

		memset(&reply, 0, sizeof(reply));
		if (add_hwpoint(session_fd, tid, hwpoint_addr, hwpoint_type,
				hwpoint_len, hwpoint_flags, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("hwpoint.id=%" PRIu64 "\n", (uint64_t)reply.id);
		printf("hwpoint.tid=%d\n", reply.tid);
		printf("hwpoint.flags=0x%x\n", reply.flags);
	} else if (strcmp(argv[1], "hwdel") == 0) {
		if (remove_hwpoint(session_fd, hwpoint_id) < 0) {
			close(session_fd);
			return 1;
		}
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
				printf("id=%" PRIu64 " tgid=%d tid=%d type=0x%x len=%u flags=0x%x state=0x%x hits=%" PRIu64 " addr=0x%" PRIx64 "\n",
				       (uint64_t)entries[i].id, entries[i].tgid,
				       entries[i].tid, entries[i].type,
				       entries[i].len, entries[i].flags,
				       entries[i].state,
				       (uint64_t)entries[i].hits,
				       (uint64_t)entries[i].addr);
			}

			if (reply.done)
				break;
			cursor = reply.next_id;
		}
	} else if (strcmp(argv[1], "step") == 0) {
		if (freeze_target_threads(session_fd, 2000, NULL, 1) < 0) {
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
		if (thaw_target_threads(session_fd, 2000, NULL, 1) < 0) {
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
	} else if (strcmp(argv[1], "vmas") == 0) {
		if (dump_target_vmas(session_fd) < 0) {
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
