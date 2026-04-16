#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/memfd.h>
#include <linux/limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../include/lkmdbg_ioctl.h"
#include "driver/common.hpp"
#include "driver/bridge_c.h"
#include "driver/bridge_control.h"
#include "driver/bridge_events.h"
#include "driver/bridge_memory.h"

#define fprintf lkmdbg_fprintf

#define SELFTEST_THREAD_COUNT 4
#define SELFTEST_THREAD_ITERS 64
#define SELFTEST_SLOT_SIZE 64
#define SELFTEST_LARGE_MAP_LEN (192U * 1024U)
#define SELFTEST_BATCH_OPS 4
#define SELFTEST_FREEZE_THREADS 2
#define SELFTEST_MULTI_SESSION_COUNT 3
#define SELFTEST_MULTI_SESSION_FREEZE_ROUNDS 4
#define SELFTEST_SOAK_ITERS 96
#define SELFTEST_SOAK_REMOTE_ALLOC_LEN (2U * 4096U)
#define SELFTEST_PERMISSION_SPAN 128U
#define SELFTEST_RACE_OPS 48U
#define SELFTEST_RACE_MAX_BATCH_BYTES (1024U * 1024U)
#define SELFTEST_REG_SENTINEL 0x5A17C0DEULL
#define VMA_QUERY_BATCH 64
#define VMA_QUERY_NAMES_SIZE 65536
#define IMAGE_QUERY_BATCH 64
#define IMAGE_QUERY_NAMES_SIZE 65536
#define THREAD_QUERY_BATCH 64
#define PAGE_QUERY_BATCH 64
#define PTE_PATCH_QUERY_BATCH 64
#define REMOTE_MAP_QUERY_BATCH 16
#define REMOTE_ALLOC_QUERY_BATCH 16
#define EVENT_READ_BATCH 16

struct child_info {
	uintptr_t basic_addr;
	uintptr_t slots_addr;
	uintptr_t nofault_addr;
	uintptr_t force_read_addr;
	uintptr_t force_write_addr;
	uintptr_t large_addr;
	uintptr_t view_exec_addr;
	uintptr_t file_addr;
	uintptr_t exec_target_addr;
	uintptr_t remote_call_addr;
	uintptr_t remote_call_x8_addr;
	uintptr_t remote_thread_launcher_addr;
	uintptr_t remote_thread_start_addr;
	uintptr_t remote_thread_tid_addr;
	uintptr_t remote_thread_arg_addr;
	uintptr_t remote_thread_counter_addr;
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

struct remote_map_query_buffer {
	struct lkmdbg_remote_map_entry *entries;
};

struct remote_alloc_query_buffer {
	struct lkmdbg_remote_alloc_entry *entries;
};

struct delayed_signal_ctx {
	pid_t pid;
	int signo;
	useconds_t delay_us;
	int ret;
	int err;
};

static int create_test_memfd(const char *name)
{
	unsigned int flags = MFD_CLOEXEC | MFD_ALLOW_SEALING;
	int fd;

#ifdef MFD_NOEXEC_SEAL
	flags |= MFD_NOEXEC_SEAL;
#endif

	fd = memfd_create(name, flags);
	if (fd >= 0)
		return fd;

#ifdef MFD_NOEXEC_SEAL
	if ((flags & MFD_NOEXEC_SEAL) && errno == EINVAL) {
		fd = memfd_create(name, flags & ~MFD_NOEXEC_SEAL);
		if (fd >= 0)
			return fd;
	}
#endif

	return -1;
}

static int64_t child_raw_syscall0(long nr)
{
#if defined(__aarch64__)
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0");

	__asm__ volatile("svc #0"
			 : "=r"(x0)
			 : "r"(x8)
			 : "memory");
	return (int64_t)x0;
#else
	long ret;

	errno = 0;
	ret = syscall(nr);
	if (ret == -1 && errno)
		return -(int64_t)errno;
	return (int64_t)ret;
#endif
}

static int64_t child_raw_syscall1(long nr, uint64_t arg0)
{
#if defined(__aarch64__)
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0") = (long)arg0;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8)
			 : "memory");
	return (int64_t)x0;
#else
	long ret;

	errno = 0;
	ret = syscall(nr, (unsigned long)arg0);
	if (ret == -1 && errno)
		return -(int64_t)errno;
	return (int64_t)ret;
#endif
}

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
	CHILD_OP_TRIGGER_SYSCALL = 10,
	CHILD_OP_PING = 11,
	CHILD_OP_WRITE_REMOTE = 12,
	CHILD_OP_CALL_REMOTE = 13,
	CHILD_OP_MPROTECT = 14,
};

static int expect_stop_state(int session_fd, uint32_t reason,
			     struct lkmdbg_stop_query_request *reply_out);
static int child_query_nofault_residency(int cmd_fd, int resp_fd);
static int child_ping(int cmd_fd, int resp_fd);
static int child_trigger_syscall(int cmd_fd, int resp_fd, int64_t *retval_out);
static int child_trigger_syscall_ex(int cmd_fd, int resp_fd, long syscall_nr,
				    uint64_t arg0, int use_arg0,
				    int64_t *retval_out);
static int child_mprotect_range(int cmd_fd, int resp_fd, uintptr_t addr,
				size_t len, int prot);
static int child_read_remote_range(int cmd_fd, int resp_fd, uintptr_t addr,
				   void *buf, size_t len);
static int child_write_remote_range(int cmd_fd, int resp_fd, uintptr_t addr,
				    const void *buf, size_t len);
static int child_fill_remote_range(int cmd_fd, int resp_fd, uintptr_t addr,
				   size_t len, uint8_t value);
static int child_call_remote0(int cmd_fd, int resp_fd, uintptr_t addr,
			      uint64_t *retval_out);
static int query_target_pages_ex(int session_fd, uint64_t start_addr,
				 uint64_t length, uint32_t flags,
				 struct page_query_buffer *buf,
				 struct lkmdbg_page_query_request *reply_out);
static int query_target_vmas_ex(int session_fd, uint64_t start_addr,
				uint32_t flags, uint32_t match_flags_mask,
				uint32_t match_flags_value,
				uint32_t match_prot_mask,
				uint32_t match_prot_value,
				struct vma_query_buffer *buf,
				struct lkmdbg_vma_query_request *reply_out);
static int lookup_target_vma_ex(int session_fd, uintptr_t remote_addr,
				uint32_t flags, uint32_t match_flags_mask,
				uint32_t match_flags_value,
				uint32_t match_prot_mask,
				uint32_t match_prot_value,
				struct vma_query_buffer *buf,
				struct lkmdbg_vma_entry *entry_out,
				char *name_out, size_t name_out_size,
				struct lkmdbg_vma_query_request *reply_out);
static const char *describe_pte_mode(uint32_t mode, uint32_t flags, char *buf,
				     size_t buf_size);
static const char *describe_pte_patch_state(uint32_t state, char *buf,
					    size_t buf_size);
static int start_selftest_child(pid_t *child_out, int *info_fd_out,
				int *cmd_fd_out, int *resp_fd_out,
				struct child_info *info_out);
static int request_child_exit(int cmd_fd);
static int wait_for_child_exit(pid_t child, int *status_out);
static int stop_selftest_child(pid_t child, int cmd_fd, int resp_fd,
			       int send_exit);

static int wait_for_stop_event_or_state(int session_fd, uint32_t reason,
					int timeout_ms,
					struct lkmdbg_event_record *event_out,
					struct lkmdbg_stop_query_request *stop_out)
{
	struct lkmdbg_stop_query_request stop_req;
	struct lkmdbg_event_record event;
	int ret;

	ret = wait_for_session_event_common(session_fd, LKMDBG_EVENT_TARGET_STOP,
					    reason, timeout_ms, &event, false);
	if (!ret) {
		if (event_out)
			*event_out = event;
		if (!stop_out)
			return 0;
		if (expect_stop_state(session_fd, reason, &stop_req) < 0)
			return -1;
		*stop_out = stop_req;
		return 0;
	}
	if (ret != -ETIMEDOUT)
		return ret;

	memset(&stop_req, 0, sizeof(stop_req));
	if (expect_stop_state(session_fd, reason, &stop_req) < 0)
		return ret;

	memset(&event, 0, sizeof(event));
	event.version = LKMDBG_PROTO_VERSION;
	event.type = LKMDBG_EVENT_TARGET_STOP;
	event.size = sizeof(event);
	event.code = reason;
	event.tgid = stop_req.stop.tgid;
	event.tid = stop_req.stop.tid;
	event.flags = stop_req.stop.event_flags;
	event.value0 = stop_req.stop.value0;
	event.value1 = stop_req.stop.value1;

	printf("selftest runtime: stop event missing, using stop-state fallback for reason=%u\n",
	       reason);

	if (event_out)
		*event_out = event;
	if (stop_out)
		*stop_out = stop_req;
	return 0;
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

	if (bridge_reset_session(session_fd) < 0) {
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

static int query_target_pages(int session_fd, uint64_t start_addr,
			      uint64_t length,
			      struct page_query_buffer *buf,
			      struct lkmdbg_page_query_request *reply_out)
{
	return query_target_pages_ex(session_fd, start_addr, length, 0, buf,
				     reply_out);
}

static int query_target_pages_ex(int session_fd, uint64_t start_addr,
				 uint64_t length, uint32_t flags,
				 struct page_query_buffer *buf,
				 struct lkmdbg_page_query_request *reply_out)
{
	return bridge_query_target_pages_ex(session_fd, start_addr, length, flags,
					    buf->entries, PAGE_QUERY_BATCH,
					    reply_out);
}

static int apply_pte_patch(int session_fd, uint64_t addr, uint32_t mode,
			   uint32_t flags, uint64_t raw_pte,
			   struct lkmdbg_pte_patch_request *reply_out)
{
	return bridge_apply_pte_patch(session_fd, addr, mode, flags, raw_pte,
				      reply_out);
}

static int remove_pte_patch(int session_fd, uint64_t id,
			    struct lkmdbg_pte_patch_request *reply_out)
{
	return bridge_remove_pte_patch(session_fd, id, reply_out);
}

static int query_pte_patches(int session_fd, uint64_t start_id,
			     struct lkmdbg_pte_patch_entry *entries,
			     uint32_t max_entries,
			     struct lkmdbg_pte_patch_query_request *reply_out)
{
	return bridge_query_pte_patches(session_fd, start_id, entries, max_entries,
					reply_out);
}

static int create_remote_map(int session_fd, uintptr_t remote_addr,
			     uintptr_t local_addr, size_t len, uint32_t prot,
			     uint32_t flags,
			     struct lkmdbg_remote_map_request *reply_out)
{
	return bridge_create_remote_map(session_fd, remote_addr, local_addr, len,
					prot, flags, 0, reply_out);
}

static int remove_remote_map(int session_fd, uint64_t map_id,
			     struct lkmdbg_remote_map_handle_request *reply_out)
{
	return bridge_remove_remote_map(session_fd, map_id, reply_out);
}

static int query_remote_maps(int session_fd, uint64_t start_id,
			     struct remote_map_query_buffer *buf,
			     struct lkmdbg_remote_map_query_request *reply_out)
{
	return bridge_query_remote_maps(session_fd, start_id, buf->entries,
					REMOTE_MAP_QUERY_BATCH, reply_out);
}

static int create_remote_alloc(int session_fd, uintptr_t remote_addr, size_t len,
			       uint32_t prot,
			       struct lkmdbg_remote_alloc_request *reply_out)
{
	return bridge_create_remote_alloc(session_fd, remote_addr, len, prot, 0,
					  reply_out);
}

static int remove_remote_alloc(
	int session_fd, uint64_t alloc_id,
	struct lkmdbg_remote_alloc_handle_request *reply_out)
{
	return bridge_remove_remote_alloc(session_fd, alloc_id, reply_out);
}

static int query_remote_allocs(
	int session_fd, uint64_t start_id, struct remote_alloc_query_buffer *buf,
	struct lkmdbg_remote_alloc_query_request *reply_out)
{
	return bridge_query_remote_allocs(session_fd, start_id, buf->entries,
					  REMOTE_ALLOC_QUERY_BATCH, reply_out);
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

static int build_view_exec_page(unsigned char *buf, size_t len, uint16_t retval)
{
#if defined(__aarch64__)
	uint32_t insn[2];

	if (!buf || len < 8U)
		return -1;

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

static int expect_set_syscall_trace_errno(int session_fd, pid_t tid,
					  int syscall_nr, uint32_t mode,
					  uint32_t phases, int expected_errno)
{
	return bridge_set_syscall_trace_expect_errno(session_fd, tid, syscall_nr,
						     mode, phases,
						     expected_errno);
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

static int read_maps_line_for_pid_addr(pid_t pid, uintptr_t addr, char *buf,
				       size_t buf_size)
{
	FILE *fp;
	char line[512];
	char path[64];

	if (!buf || !buf_size)
		return -1;

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	fp = fopen(path, "re");
	if (!fp) {
		fprintf(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
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
	fprintf(stderr, "address 0x%" PRIxPTR " not found in %s\n", addr, path);
	return -1;
}

static int read_maps_line_for_addr(uintptr_t addr, char *buf, size_t buf_size)
{
	return read_maps_line_for_pid_addr(getpid(), addr, buf, buf_size);
}

static int query_target_vmas(int session_fd, uint64_t start_addr,
			     struct vma_query_buffer *buf,
			     struct lkmdbg_vma_query_request *reply_out)
{
	return query_target_vmas_ex(session_fd, start_addr, 0, 0, 0, 0, 0, buf,
				    reply_out);
}

static int query_target_vmas_ex(int session_fd, uint64_t start_addr,
				uint32_t flags, uint32_t match_flags_mask,
				uint32_t match_flags_value,
				uint32_t match_prot_mask,
				uint32_t match_prot_value,
				struct vma_query_buffer *buf,
				struct lkmdbg_vma_query_request *reply_out)
{
	return bridge_query_target_vmas_ex(
		session_fd, start_addr, flags, match_flags_mask,
		match_flags_value, match_prot_mask, match_prot_value, buf->entries,
		VMA_QUERY_BATCH, buf->names, VMA_QUERY_NAMES_SIZE, reply_out);
}

static int query_target_images(int session_fd, uint64_t start_addr,
			       struct image_query_buffer *buf,
			       struct lkmdbg_image_query_request *reply_out)
{
	return bridge_query_target_images(
		session_fd, start_addr, 0, buf->entries, IMAGE_QUERY_BATCH,
		buf->names, IMAGE_QUERY_NAMES_SIZE, reply_out);
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
	return lookup_target_vma_ex(session_fd, remote_addr, 0, 0, 0, 0, 0, buf,
				    entry_out, name_out, name_out_size, NULL);
}

static int lookup_target_vma_ex(int session_fd, uintptr_t remote_addr,
				uint32_t flags, uint32_t match_flags_mask,
				uint32_t match_flags_value,
				uint32_t match_prot_mask,
				uint32_t match_prot_value,
				struct vma_query_buffer *buf,
				struct lkmdbg_vma_entry *entry_out,
				char *name_out, size_t name_out_size,
				struct lkmdbg_vma_query_request *reply_out)
{
	struct lkmdbg_vma_query_request reply;
	unsigned int i;

	if (query_target_vmas_ex(session_fd, remote_addr, flags,
				 match_flags_mask, match_flags_value,
				 match_prot_mask, match_prot_value, buf,
				 &reply) < 0)
		return -1;

	for (i = 0; i < reply.entries_filled; i++) {
		const struct lkmdbg_vma_entry *entry = &buf->entries[i];
		const char *name;

		if (remote_addr < entry->start_addr || remote_addr >= entry->end_addr)
			continue;

		*entry_out = *entry;
		if (reply_out)
			*reply_out = reply;
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
	struct lkmdbg_vma_query_request full_reply;
	struct lkmdbg_vma_query_request full_reply_again;
	struct lkmdbg_vma_query_request filtered_reply;
	char name[256];
	char full_name[PATH_MAX];
	uint64_t cursor = 0;
	unsigned int filtered_total = 0;
	int saw_filtered_file = 0;
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

	memset(&entry, 0, sizeof(entry));
	memset(full_name, 0, sizeof(full_name));
	memset(&full_reply, 0, sizeof(full_reply));
	if (lookup_target_vma_ex(session_fd, info->file_addr,
				 LKMDBG_VMA_QUERY_FLAG_FULL_PATH, 0, 0, 0, 0,
				 &buf, &entry, full_name, sizeof(full_name),
				 &full_reply) < 0)
		goto out;
	if (!full_reply.generation || !entry.name_size ||
	    strstr(full_name, "lkmdbg-vma") == NULL) {
		fprintf(stderr,
			"full-path VMA mismatch generation=0x%" PRIx64 " name=%s\n",
			(uint64_t)full_reply.generation, full_name);
		goto out;
	}

	memset(&full_reply_again, 0, sizeof(full_reply_again));
	if (query_target_vmas_ex(session_fd, 0, LKMDBG_VMA_QUERY_FLAG_FULL_PATH,
				 0, 0, 0, 0, &buf, &full_reply_again) < 0)
		goto out;
	if (full_reply_again.generation != full_reply.generation) {
		fprintf(stderr,
			"VMA generation changed unexpectedly old=0x%" PRIx64 " new=0x%" PRIx64 "\n",
			(uint64_t)full_reply.generation,
			(uint64_t)full_reply_again.generation);
		goto out;
	}

	for (;;) {
		unsigned int i;

		if (query_target_vmas_ex(session_fd, cursor, 0,
					 LKMDBG_VMA_FLAG_FILE,
					 LKMDBG_VMA_FLAG_FILE, 0, 0, &buf,
					 &filtered_reply) < 0)
			goto out;
		if (filtered_reply.generation != full_reply.generation) {
			fprintf(stderr,
				"filtered VMA generation mismatch full=0x%" PRIx64 " filtered=0x%" PRIx64 "\n",
				(uint64_t)full_reply.generation,
				(uint64_t)filtered_reply.generation);
			goto out;
		}
		for (i = 0; i < filtered_reply.entries_filled; i++) {
			const struct lkmdbg_vma_entry *filtered = &buf.entries[i];

			if (!(filtered->flags & LKMDBG_VMA_FLAG_FILE)) {
				fprintf(stderr,
					"filtered VMA missing file flag flags=0x%x\n",
					filtered->flags);
				goto out;
			}
			if (info->file_addr >= filtered->start_addr &&
			    info->file_addr < filtered->end_addr)
				saw_filtered_file = 1;
			if (info->force_read_addr >= filtered->start_addr &&
			    info->force_read_addr < filtered->end_addr) {
				fprintf(stderr,
					"filtered VMA unexpectedly returned anon range\n");
				goto out;
			}
		}
		filtered_total += filtered_reply.entries_filled;
		if (filtered_reply.done)
			break;
		if (filtered_reply.next_addr <= cursor) {
			fprintf(stderr,
				"filtered VMA cursor stalled old=0x%" PRIx64 " new=0x%" PRIx64 "\n",
				(uint64_t)cursor, (uint64_t)filtered_reply.next_addr);
			goto out;
		}
		cursor = filtered_reply.next_addr;
	}
	if (!filtered_total || !saw_filtered_file) {
		fprintf(stderr,
			"filtered VMA query missing file mapping total=%u saw_file=%d\n",
			filtered_total, saw_filtered_file);
		goto out;
	}

	if (verify_vma_iteration(session_fd, info, &buf) < 0)
		goto out;

	printf("selftest vma query ok file=%s full=%s generation=0x%" PRIx64 " inode=%" PRIu64 " dev=%u:%u\n",
	       name, full_name, (uint64_t)full_reply.generation,
	       (uint64_t)info->file_inode, info->file_dev_major,
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
			"force-read page table mismatch level=%u shift=%u phys=0x%" PRIx64 " entry=0x%" PRIx64 " flags=0x%x pt_flags=0x%x\n",
			entry.pt_level, entry.page_shift,
			(uint64_t)entry.phys_addr,
			(uint64_t)entry.pt_entry_raw, entry.flags, entry.pt_flags);
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
			"force-write page table mismatch level=%u shift=%u phys=0x%" PRIx64 " entry=0x%" PRIx64 " flags=0x%x pt_flags=0x%x\n",
			entry.pt_level, entry.page_shift,
			(uint64_t)entry.phys_addr,
			(uint64_t)entry.pt_entry_raw, entry.flags, entry.pt_flags);
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
		    entry.phys_addr == 0 || entry.pt_entry_raw == 0 ||
		    !(entry.pt_flags & LKMDBG_PAGE_PT_FLAG_VALID)) {
			fprintf(stderr,
				"file page table mismatch level=%u phys=0x%" PRIx64 " entry=0x%" PRIx64 " flags=0x%x pt_flags=0x%x\n",
				entry.pt_level, (uint64_t)entry.phys_addr,
				(uint64_t)entry.pt_entry_raw, entry.flags,
				entry.pt_flags);
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

	{
		struct lkmdbg_page_query_request reply;
		uint64_t start = info->large_addr;
		uint64_t end = info->large_addr + (3ULL * info->page_size);
		uint64_t last_addr = 0;
		unsigned int i;

		if (query_target_pages_ex(session_fd, start, end - start,
					  LKMDBG_PAGE_QUERY_FLAG_LEAF_STEP,
					  &buf, &reply) < 0)
			goto out;
		if (!reply.entries_filled) {
			fprintf(stderr, "leaf-step QUERY_PAGES returned no entries\n");
			goto out;
		}
		for (i = 0; i < reply.entries_filled; i++) {
			if (i && buf.entries[i].page_addr <= last_addr) {
				fprintf(stderr,
					"leaf-step QUERY_PAGES not monotonic old=0x%" PRIx64 " new=0x%" PRIx64 "\n",
					(uint64_t)last_addr,
					(uint64_t)buf.entries[i].page_addr);
				goto out;
			}
			if (buf.entries[i].page_shift &&
			    buf.entries[i].page_shift < expected_page_shift) {
				fprintf(stderr,
					"leaf-step QUERY_PAGES returned invalid shift=%u\n",
					buf.entries[i].page_shift);
				goto out;
			}
			last_addr = buf.entries[i].page_addr;
		}
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
	struct lkmdbg_phys_op translate_op;
	struct lkmdbg_phys_request translate_req;
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

	memset(&translate_op, 0, sizeof(translate_op));
	memset(&translate_req, 0, sizeof(translate_req));
	translate_op.phys_addr = info->basic_addr;
	translate_op.length = sizeof(via_vaddr);
	translate_op.flags = LKMDBG_PHYS_OP_FLAG_TARGET_VADDR |
			     LKMDBG_PHYS_OP_FLAG_TRANSLATE_ONLY;
	if (xfer_physical_memory(session_fd, &translate_op, 1, 0, &translate_req,
				 0) < 0) {
		fprintf(stderr, "target-vaddr translate-only request failed\n");
		goto out;
	}
	if (translate_req.ops_done != 1 || translate_req.bytes_done != 0 ||
	    translate_op.bytes_done != 0 ||
	    translate_op.resolved_phys_addr !=
		    entry.phys_addr +
			    (info->basic_addr & (info->page_size - 1)) ||
	    translate_op.page_shift != entry.page_shift ||
	    translate_op.pt_level != entry.pt_level ||
	    !(translate_op.pt_flags & LKMDBG_PAGE_PT_FLAG_VALID) ||
	    !translate_op.phys_span_length) {
		fprintf(stderr,
			"translate-only mismatch ops_done=%u bytes_done=%" PRIu64 " phys=0x%" PRIx64 " expected=0x%" PRIx64 " shift=%u/%u level=%u/%u pt_flags=0x%x span=%u\n",
			translate_req.ops_done, (uint64_t)translate_req.bytes_done,
			(uint64_t)translate_op.resolved_phys_addr,
			(uint64_t)(entry.phys_addr +
				   (info->basic_addr &
				    (info->page_size - 1))),
			translate_op.page_shift, entry.page_shift,
			translate_op.pt_level, entry.pt_level,
			translate_op.pt_flags, translate_op.phys_span_length);
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
	printf("selftest physical memory ok phys=0x%" PRIx64 " xlate=0x%" PRIx64 " byte=0x%02x\n",
	       (uint64_t)(entry.phys_addr +
			  (info->basic_addr & (info->page_size - 1))),
	       (uint64_t)translate_op.resolved_phys_addr, phys_new);

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

static int verify_view_external_read(int session_fd, pid_t child,
				     const struct child_info *info, int cmd_fd,
				     int resp_fd)
{
	struct lkmdbg_view_region_request region_reply;
	struct lkmdbg_view_backing_request backing_reply;
	struct lkmdbg_view_backing_request policy_backing_reply;
	struct lkmdbg_view_backing_request reset_reply;
	struct lkmdbg_view_region_query_request query_reply;
	struct lkmdbg_view_region_handle_request remove_reply;
	struct lkmdbg_view_region_entry entry;
	struct iovec local_iov;
	struct iovec remote_iov;
	uint8_t *fake_buf = NULL;
	uint8_t *original_buf = NULL;
	uint8_t *child_buf = NULL;
	uint8_t *kernel_buf = NULL;
	uint8_t *external_buf = NULL;
	uintptr_t region_addr;
	uint32_t bytes_done = 0;
	int region_active = 0;
	int remote_restore_needed = 0;
	int ret = -1;

	memset(&region_reply, 0, sizeof(region_reply));
	memset(&backing_reply, 0, sizeof(backing_reply));
	memset(&policy_backing_reply, 0, sizeof(policy_backing_reply));
	memset(&reset_reply, 0, sizeof(reset_reply));
	memset(&query_reply, 0, sizeof(query_reply));
	memset(&remove_reply, 0, sizeof(remove_reply));
	memset(&entry, 0, sizeof(entry));

	region_addr = info->large_addr + (info->page_size * 12U);
	if (region_addr + info->page_size > info->large_addr + info->large_len) {
		fprintf(stderr, "view external read region outside test mapping\n");
		return -1;
	}

	fake_buf = malloc(info->page_size);
	original_buf = malloc(info->page_size);
	child_buf = malloc(info->page_size);
	kernel_buf = malloc(info->page_size);
	external_buf = malloc(info->page_size);
	if (!fake_buf || !original_buf || !child_buf || !kernel_buf ||
	    !external_buf) {
		fprintf(stderr, "view external read allocation failed\n");
		goto out;
	}

	if (child_read_remote_range(cmd_fd, resp_fd, region_addr, original_buf,
				    info->page_size) < 0)
		goto out;
	if (child_fill_remote_range(cmd_fd, resp_fd, region_addr, info->page_size,
				    0x44U) < 0)
		goto out;
	remote_restore_needed = 1;
	memset(fake_buf, 0xC3, info->page_size);

	if (create_view_region(session_fd, region_addr, info->page_size,
			       LKMDBG_VIEW_ACCESS_READ |
				       LKMDBG_VIEW_ACCESS_EXEC,
			       LKMDBG_VIEW_BACKEND_AUTO,
			       LKMDBG_VIEW_FAULT_POLICY_TRAP_ONLY,
			       LKMDBG_VIEW_SYNC_NONE,
			       LKMDBG_VIEW_WRITEBACK_DISCARD,
			       &region_reply) < 0) {
		if (errno == EOPNOTSUPP || errno == ENOENT) {
			printf("selftest view external read skipped: backend unavailable errno=%d\n",
			       errno);
			ret = 0;
			goto out;
		}
		goto out;
	}
	region_active = 1;

	if (set_view_region_read_backing(session_fd, region_reply.region_id, fake_buf,
					 info->page_size,
					 LKMDBG_VIEW_BACKING_USER_BUFFER,
					 &backing_reply) < 0)
		goto out;
	if (set_view_region_write_backing(session_fd, region_reply.region_id, NULL,
					  0, LKMDBG_VIEW_BACKING_ORIGINAL,
					  &policy_backing_reply) < 0)
		goto out;
	if (set_view_region_exec_backing(session_fd, region_reply.region_id, NULL,
					 0, LKMDBG_VIEW_BACKING_ORIGINAL,
					 &policy_backing_reply) < 0)
		goto out;

	if (query_view_regions(session_fd, region_reply.region_id, &entry, 1,
			       &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.region_id != region_reply.region_id ||
	    entry.active_backend != LKMDBG_VIEW_BACKEND_EXTERNAL_READ ||
	    entry.read_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER ||
	    entry.write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL) {
		fprintf(stderr,
			"bad view region query filled=%u backend=%u read=%u write=%u exec=%u region=%" PRIu64 "\n",
			query_reply.entries_filled, entry.active_backend,
			entry.read_backing_type, entry.write_backing_type,
			entry.exec_backing_type, (uint64_t)entry.region_id);
		goto out;
	}

	if (child_read_remote_range(cmd_fd, resp_fd, region_addr, child_buf,
				    info->page_size) < 0)
		goto out;

	if (read_target_memory(session_fd, region_addr, kernel_buf, info->page_size,
			       &bytes_done, 0) < 0 ||
	    bytes_done != info->page_size) {
		fprintf(stderr, "view external read READ_MEM failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}
	if (memcmp(child_buf, kernel_buf, info->page_size) != 0) {
		fprintf(stderr,
			"view external read actual mismatch between child and READ_MEM\n");
		goto out;
	}
	if (memcmp(kernel_buf, fake_buf, info->page_size) == 0) {
		fprintf(stderr,
			"view external read kernel path unexpectedly returned fake backing\n");
		goto out;
	}

	local_iov.iov_base = external_buf;
	local_iov.iov_len = info->page_size;
	remote_iov.iov_base = (void *)region_addr;
	remote_iov.iov_len = info->page_size;
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)info->page_size) {
		fprintf(stderr,
			"view external read process_vm_readv failed errno=%d\n",
			errno);
		goto out;
	}
	if (memcmp(external_buf, fake_buf, info->page_size) != 0) {
		fprintf(stderr, "view external read fake overlay mismatch\n");
		goto out;
	}

	if (set_view_region_read_backing(session_fd, region_reply.region_id, NULL,
					 0, LKMDBG_VIEW_BACKING_ORIGINAL,
					 &reset_reply) < 0)
		goto out;

	memset(&entry, 0, sizeof(entry));
	memset(&query_reply, 0, sizeof(query_reply));
	if (query_view_regions(session_fd, region_reply.region_id, &entry, 1,
			       &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.read_source_id != 0) {
		fprintf(stderr,
			"view external read reset query mismatch filled=%u read_backing=%u source=%" PRIu64 "\n",
			query_reply.entries_filled, entry.read_backing_type,
			(uint64_t)entry.read_source_id);
		goto out;
	}

	memset(external_buf, 0, info->page_size);
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)info->page_size) {
		fprintf(stderr,
			"view external read post-reset process_vm_readv failed errno=%d\n",
			errno);
		goto out;
	}
	if (memcmp(external_buf, child_buf, info->page_size) != 0) {
		fprintf(stderr, "view external read reset did not restore original bytes\n");
		goto out;
	}

	ret = 0;
	printf("selftest view external read ok region=%" PRIu64 " backend=%u source=%" PRIu64 "\n",
	       (uint64_t)region_reply.region_id, entry.active_backend,
	       (uint64_t)entry.read_source_id);

out:
	if (remote_restore_needed) {
		(void)child_write_remote_range(cmd_fd, resp_fd, region_addr,
					       original_buf, info->page_size);
	}
	if (region_active)
		remove_view_region(session_fd, region_reply.region_id, &remove_reply);
	free(external_buf);
	free(kernel_buf);
	free(child_buf);
	free(original_buf);
	free(fake_buf);
	return ret;
}

static int verify_view_wxshadow(int session_fd, pid_t child,
				const struct child_info *info, int cmd_fd,
				int resp_fd)
{
#if !defined(__aarch64__)
	(void)session_fd;
	(void)child;
	(void)info;
	(void)cmd_fd;
	(void)resp_fd;
	printf("selftest view wxshadow skipped: non-aarch64 userspace\n");
	return 0;
#else
	struct lkmdbg_view_region_request region_reply;
	struct lkmdbg_view_backing_request write_backing_reply;
	struct lkmdbg_view_backing_request exec_backing_reply;
	struct lkmdbg_view_backing_request reset_reply;
	struct lkmdbg_view_region_query_request query_reply;
	struct lkmdbg_view_region_handle_request remove_reply;
	struct lkmdbg_view_region_entry entry;
	struct iovec local_iov;
	struct iovec remote_iov;
	uint8_t *original_buf = NULL;
	uint8_t *shadow_buf = NULL;
	uint8_t *patch_buf = NULL;
	uint8_t *kernel_buf = NULL;
	uint8_t *external_buf = NULL;
	uintptr_t region_addr = info->view_exec_addr;
	uint64_t retval = 0;
	uint32_t bytes_done = 0;
	int region_active = 0;
	int ret = -1;

	memset(&region_reply, 0, sizeof(region_reply));
	memset(&write_backing_reply, 0, sizeof(write_backing_reply));
	memset(&exec_backing_reply, 0, sizeof(exec_backing_reply));
	memset(&reset_reply, 0, sizeof(reset_reply));
	memset(&query_reply, 0, sizeof(query_reply));
	memset(&remove_reply, 0, sizeof(remove_reply));
	memset(&entry, 0, sizeof(entry));

	original_buf = malloc(info->page_size);
	shadow_buf = malloc(info->page_size);
	patch_buf = malloc(info->page_size);
	kernel_buf = malloc(info->page_size);
	external_buf = malloc(info->page_size);
	if (!original_buf || !shadow_buf || !patch_buf || !kernel_buf ||
	    !external_buf) {
		fprintf(stderr, "view wxshadow allocation failed\n");
		goto out;
	}

	if (build_view_exec_page(original_buf, info->page_size, 17U) < 0 ||
	    build_view_exec_page(shadow_buf, info->page_size, 34U) < 0 ||
	    build_view_exec_page(patch_buf, info->page_size, 51U) < 0) {
		fprintf(stderr, "view wxshadow build exec pages failed\n");
		goto out;
	}

	if (child_write_remote_range(cmd_fd, resp_fd, region_addr, original_buf,
				     info->page_size) < 0)
		goto out;
	if (child_call_remote0(cmd_fd, resp_fd, region_addr, &retval) < 0)
		goto out;
	if (retval != 17U) {
		fprintf(stderr,
			"view wxshadow original retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	if (create_view_region(session_fd, region_addr, info->page_size,
			       LKMDBG_VIEW_ACCESS_READ |
				       LKMDBG_VIEW_ACCESS_WRITE |
				       LKMDBG_VIEW_ACCESS_EXEC,
			       LKMDBG_VIEW_BACKEND_AUTO,
			       LKMDBG_VIEW_FAULT_POLICY_TRAP_ONLY,
			       LKMDBG_VIEW_SYNC_NONE,
			       LKMDBG_VIEW_WRITEBACK_DISCARD,
			       &region_reply) < 0) {
		if (errno == EOPNOTSUPP || errno == ENOENT) {
			printf("selftest view wxshadow skipped: backend unavailable errno=%d\n",
			       errno);
			ret = 0;
			goto out;
		}
		goto out;
	}
	region_active = 1;

	if (set_view_region_write_backing(session_fd, region_reply.region_id,
					  shadow_buf, info->page_size,
					  LKMDBG_VIEW_BACKING_USER_BUFFER,
					  &write_backing_reply) < 0)
		goto out;
	if (set_view_region_exec_backing(session_fd, region_reply.region_id,
					 shadow_buf, info->page_size,
					 LKMDBG_VIEW_BACKING_USER_BUFFER,
					 &exec_backing_reply) < 0)
		goto out;

	if (query_view_regions(session_fd, region_reply.region_id, &entry, 1,
			       &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 ||
	    entry.region_id != region_reply.region_id ||
	    entry.active_backend != LKMDBG_VIEW_BACKEND_WXSHADOW ||
	    entry.read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.write_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER ||
	    entry.exec_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER) {
		fprintf(stderr,
			"bad wxshadow query filled=%u backend=%u read=%u write=%u exec=%u region=%" PRIu64 "\n",
			query_reply.entries_filled, entry.active_backend,
			entry.read_backing_type, entry.write_backing_type,
			entry.exec_backing_type,
			(uint64_t)entry.region_id);
		goto out;
	}

	local_iov.iov_base = external_buf;
	local_iov.iov_len = info->page_size;
	remote_iov.iov_base = (void *)region_addr;
	remote_iov.iov_len = info->page_size;
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)info->page_size) {
		fprintf(stderr,
			"view wxshadow process_vm_readv failed errno=%d\n",
			errno);
		goto out;
	}
	if (memcmp(external_buf, original_buf, info->page_size) != 0) {
		fprintf(stderr, "view wxshadow external read mismatch\n");
		goto out;
	}

	if (read_target_memory(session_fd, region_addr, kernel_buf, info->page_size,
			       &bytes_done, 0) < 0 ||
	    bytes_done != info->page_size) {
		fprintf(stderr, "view wxshadow READ_MEM failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}
	if (memcmp(kernel_buf, shadow_buf, info->page_size) != 0) {
		fprintf(stderr, "view wxshadow READ_MEM shadow mismatch\n");
		goto out;
	}

	if (child_call_remote0(cmd_fd, resp_fd, region_addr, &retval) < 0)
		goto out;
	if (retval != 34U) {
		fprintf(stderr,
			"view wxshadow shadow retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	bytes_done = 0;
	if (write_target_memory(session_fd, region_addr, patch_buf, info->page_size,
				&bytes_done, 0) < 0 ||
	    bytes_done != info->page_size) {
		fprintf(stderr, "view wxshadow WRITE_MEM failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	memset(kernel_buf, 0, info->page_size);
	bytes_done = 0;
	if (read_target_memory(session_fd, region_addr, kernel_buf, info->page_size,
			       &bytes_done, 0) < 0 ||
	    bytes_done != info->page_size ||
	    memcmp(kernel_buf, patch_buf, info->page_size) != 0) {
		fprintf(stderr, "view wxshadow patched READ_MEM mismatch bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	memset(external_buf, 0, info->page_size);
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)info->page_size ||
	    memcmp(external_buf, original_buf, info->page_size) != 0) {
		fprintf(stderr, "view wxshadow patched external read mismatch errno=%d\n",
			errno);
		goto out;
	}

	if (child_call_remote0(cmd_fd, resp_fd, region_addr, &retval) < 0)
		goto out;
	if (retval != 51U) {
		fprintf(stderr,
			"view wxshadow patched retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	if (set_view_region_write_backing(session_fd, region_reply.region_id, NULL,
					  0, LKMDBG_VIEW_BACKING_ORIGINAL,
					  &reset_reply) < 0)
		goto out;
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
	    entry.write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.write_source_id != 0 ||
	    entry.exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    entry.exec_source_id != 0) {
		fprintf(stderr,
			"view wxshadow reset query mismatch filled=%u write=%u write_source=%" PRIu64 " exec=%u exec_source=%" PRIu64 "\n",
			query_reply.entries_filled, entry.write_backing_type,
			(uint64_t)entry.write_source_id, entry.exec_backing_type,
			(uint64_t)entry.exec_source_id);
		goto out;
	}

	memset(external_buf, 0, info->page_size);
	if (process_vm_readv(child, &local_iov, 1, &remote_iov, 1, 0) !=
	    (ssize_t)info->page_size) {
		fprintf(stderr,
			"view wxshadow post-reset process_vm_readv failed errno=%d\n",
			errno);
		goto out;
	}
	if (memcmp(external_buf, original_buf, info->page_size) != 0) {
		fprintf(stderr, "view wxshadow reset did not restore original bytes\n");
		goto out;
	}

	if (child_call_remote0(cmd_fd, resp_fd, region_addr, &retval) < 0)
		goto out;
	if (retval != 17U) {
		fprintf(stderr,
			"view wxshadow reset retval mismatch got=%" PRIu64 "\n",
			retval);
		goto out;
	}

	ret = 0;
	printf("selftest view wxshadow ok region=%" PRIu64 " backend=%u write_source=%" PRIu64 " exec_source=%" PRIu64 "\n",
	       (uint64_t)region_reply.region_id, entry.active_backend,
	       (uint64_t)write_backing_reply.source_id,
	       (uint64_t)exec_backing_reply.source_id);

out:
	if (region_active)
		remove_view_region(session_fd, region_reply.region_id, &remove_reply);
	free(external_buf);
	free(kernel_buf);
	free(patch_buf);
	free(shadow_buf);
	free(original_buf);
	return ret;
#endif
}

static int verify_remote_map(int session_fd, pid_t child,
			     const struct child_info *info, int cmd_fd,
			     int resp_fd)
{
	struct lkmdbg_remote_map_request reply;
	struct lkmdbg_remote_map_request ro_reply;
	struct lkmdbg_remote_map_request inject_reply;
	struct lkmdbg_remote_map_request stealth_reply;
	struct lkmdbg_remote_map_request stealth_inject_reply;
	struct lkmdbg_remote_map_handle_request remove_reply;
	struct lkmdbg_remote_map_query_request query_reply;
	unsigned char *view = MAP_FAILED;
	unsigned char *ro_view = MAP_FAILED;
	unsigned char *local_map = MAP_FAILED;
	unsigned char *stealth_view = MAP_FAILED;
	unsigned char *verify_buf = NULL;
	unsigned char *expected_buf = NULL;
	struct remote_map_query_buffer query_buf = { 0 };
	struct remote_map_wake_ctx wake_ctx;
	pthread_t wake_thread;
	char maps_before[512];
	char maps_after[512];
	char target_maps_before[512];
	char target_maps_after[512];
	uint32_t bytes_done = 0;
	uintptr_t stealth_target_addr = 0;
	size_t test_len;
	int local_fd = -1;
	int stealth_fd = -1;
	int ret = -1;
	int restore_needed = 0;
	int stealth_restore_needed = 0;
	int stealth_map_active = 0;
	int wake_thread_started = 0;

	memset(&reply, 0, sizeof(reply));
	reply.map_fd = -1;
	memset(&ro_reply, 0, sizeof(ro_reply));
	ro_reply.map_fd = -1;
	memset(&inject_reply, 0, sizeof(inject_reply));
	inject_reply.map_fd = -1;
	memset(&stealth_reply, 0, sizeof(stealth_reply));
	stealth_reply.map_fd = -1;
	memset(&stealth_inject_reply, 0, sizeof(stealth_inject_reply));
	stealth_inject_reply.map_fd = -1;
	memset(&remove_reply, 0, sizeof(remove_reply));
	memset(&query_reply, 0, sizeof(query_reply));

	query_buf.entries =
		calloc(REMOTE_MAP_QUERY_BATCH, sizeof(*query_buf.entries));
	if (!query_buf.entries) {
		fprintf(stderr, "remote map query allocation failed\n");
		goto out;
	}

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

	stealth_fd = create_test_memfd("lkmdbg-rmap-stealth");
	if (stealth_fd < 0) {
		fprintf(stderr, "stealth local memfd_create failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (ftruncate(stealth_fd, (off_t)test_len) < 0) {
		fprintf(stderr, "stealth local ftruncate failed: %s\n",
			strerror(errno));
		goto out;
	}
	stealth_view = mmap(NULL, test_len, PROT_READ | PROT_WRITE, MAP_SHARED,
			    stealth_fd, 0);
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
	stealth_map_active = 1;

	if (!stealth_reply.map_id || stealth_reply.map_fd != -1 ||
	    stealth_reply.mapped_length != reply.mapped_length) {
		fprintf(stderr,
			"stealth remote map reply mismatch id=%" PRIu64 " len=%" PRIu64 " fd=%d\n",
			(uint64_t)stealth_reply.map_id,
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
	stealth_restore_needed = 1;
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

	if (query_remote_maps(session_fd, 0, &query_buf, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 || !query_reply.done ||
	    query_reply.next_id != stealth_reply.map_id ||
	    query_buf.entries[0].map_id != stealth_reply.map_id ||
	    query_buf.entries[0].local_addr != (uintptr_t)stealth_view ||
	    query_buf.entries[0].remote_addr != info->large_addr ||
	    query_buf.entries[0].mapped_length != stealth_reply.mapped_length ||
	    query_buf.entries[0].prot != (LKMDBG_REMOTE_MAP_PROT_READ |
					  LKMDBG_REMOTE_MAP_PROT_WRITE) ||
	    query_buf.entries[0].flags != LKMDBG_REMOTE_MAP_FLAG_STEALTH_LOCAL) {
		fprintf(stderr,
			"stealth remote map query mismatch filled=%u done=%u next=%" PRIu64 " entry_id=%" PRIu64 "\n",
			query_reply.entries_filled, query_reply.done,
			(uint64_t)query_reply.next_id,
			query_reply.entries_filled ?
				(uint64_t)query_buf.entries[0].map_id :
				(uint64_t)0);
		goto out;
	}

	if (stealth_restore_needed) {
		size_t i;

		for (i = 0; i < 64; i++)
			stealth_view[256 + i] = pattern_byte(256 + i, 1);
		stealth_restore_needed = 0;
	}

	if (remove_remote_map(session_fd, stealth_reply.map_id, &remove_reply) < 0)
		goto out;
	stealth_map_active = 0;
	if (remove_reply.map_id != stealth_reply.map_id ||
	    remove_reply.local_addr != (uintptr_t)stealth_view ||
	    remove_reply.remote_addr != info->large_addr ||
	    remove_reply.mapped_length != stealth_reply.mapped_length ||
	    remove_reply.prot != (LKMDBG_REMOTE_MAP_PROT_READ |
				  LKMDBG_REMOTE_MAP_PROT_WRITE) ||
	    remove_reply.flags != LKMDBG_REMOTE_MAP_FLAG_STEALTH_LOCAL) {
		fprintf(stderr,
			"stealth remote map remove reply mismatch id=%" PRIu64 " local=0x%" PRIx64 " remote=0x%" PRIx64 " len=%" PRIu64 " prot=0x%x flags=0x%x\n",
			(uint64_t)remove_reply.map_id,
			(uint64_t)remove_reply.local_addr,
			(uint64_t)remove_reply.remote_addr,
			(uint64_t)remove_reply.mapped_length,
			remove_reply.prot, remove_reply.flags);
		goto out;
	}
	if (read_maps_line_for_addr((uintptr_t)stealth_view, maps_after,
				    sizeof(maps_after)) < 0)
		goto out;
	if (strcmp(maps_before, maps_after) != 0) {
		fprintf(stderr,
			"stealth remote map remove changed /proc/self/maps entry\n");
		goto out;
	}
	memset(verify_buf, 0, test_len);
	if (read_target_memory(session_fd, info->large_addr, verify_buf, test_len,
			       &bytes_done, 0) < 0 ||
	    bytes_done != test_len) {
		fprintf(stderr,
			"stealth remote map remove target snapshot failed bytes_done=%u expected=%zu\n",
			bytes_done, test_len);
		goto out;
	}
	if (memcmp(stealth_view, verify_buf, test_len) == 0) {
		fprintf(stderr,
			"stealth remote map local view still aliases target after remove\n");
		goto out;
	}
	if (stealth_view[0] != 0x3C || stealth_view[test_len - 1] != 0x3C) {
		fprintf(stderr,
			"stealth remote map local view did not restore baseline bytes\n");
		goto out;
	}
	memset(query_buf.entries, 0,
	       REMOTE_MAP_QUERY_BATCH * sizeof(*query_buf.entries));
	if (query_remote_maps(session_fd, 0, &query_buf, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 0 || !query_reply.done ||
	    query_reply.next_id != 0) {
		fprintf(stderr,
			"stealth remote map query not empty after remove filled=%u done=%u next=%" PRIu64 "\n",
			query_reply.entries_filled, query_reply.done,
			(uint64_t)query_reply.next_id);
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

	local_fd = create_test_memfd("lkmdbg-rmap-local");
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

	fill_pattern(local_map, info->page_size, 53);
	stealth_target_addr = info->large_addr + (info->page_size * 8U);
	if (stealth_target_addr + info->page_size >
	    info->large_addr + info->large_len) {
		fprintf(stderr, "stealth target test range exceeds child buffer\n");
		goto out;
	}
	if (read_maps_line_for_pid_addr(child, stealth_target_addr,
					target_maps_before,
					sizeof(target_maps_before)) < 0)
		goto out;

	if (create_remote_map(session_fd, stealth_target_addr,
			      (uintptr_t)local_map, info->page_size,
			      LKMDBG_REMOTE_MAP_PROT_READ |
				      LKMDBG_REMOTE_MAP_PROT_WRITE,
			      LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET |
				      LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET,
			      &stealth_inject_reply) < 0)
		goto out;
	if (!stealth_inject_reply.map_id || stealth_inject_reply.map_fd != -1 ||
	    stealth_inject_reply.remote_addr != stealth_target_addr ||
	    stealth_inject_reply.local_addr != (uintptr_t)local_map ||
	    stealth_inject_reply.mapped_length != info->page_size) {
		fprintf(stderr,
			"stealth inject reply mismatch id=%" PRIu64 " remote=0x%" PRIx64 " local=0x%" PRIx64 " len=%" PRIu64 " fd=%d\n",
			(uint64_t)stealth_inject_reply.map_id,
			(uint64_t)stealth_inject_reply.remote_addr,
			(uint64_t)stealth_inject_reply.local_addr,
			(uint64_t)stealth_inject_reply.mapped_length,
			stealth_inject_reply.map_fd);
		goto out;
	}

	if (read_maps_line_for_pid_addr(child, stealth_target_addr,
					target_maps_after,
					sizeof(target_maps_after)) < 0)
		goto out;
	if (strcmp(target_maps_before, target_maps_after) != 0) {
		fprintf(stderr,
			"stealth target map changed target /proc/maps entry\n");
		goto out;
	}

	memset(verify_buf, 0, info->page_size);
	if (child_read_remote_range(cmd_fd, resp_fd, stealth_target_addr,
				    verify_buf, info->page_size) < 0 ||
	    memcmp(verify_buf, local_map, info->page_size) != 0) {
		fprintf(stderr, "stealth target child readback mismatch\n");
		goto out;
	}

	if (query_remote_maps(session_fd, 0, &query_buf, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 || !query_reply.done ||
	    query_reply.next_id != stealth_inject_reply.map_id ||
	    query_buf.entries[0].map_id != stealth_inject_reply.map_id ||
	    query_buf.entries[0].local_addr != (uintptr_t)local_map ||
	    query_buf.entries[0].remote_addr != stealth_target_addr ||
	    query_buf.entries[0].mapped_length != info->page_size ||
	    query_buf.entries[0].prot != (LKMDBG_REMOTE_MAP_PROT_READ |
					  LKMDBG_REMOTE_MAP_PROT_WRITE) ||
	    query_buf.entries[0].flags !=
		    (LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET |
		     LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET)) {
		fprintf(stderr,
			"stealth target query mismatch filled=%u done=%u next=%" PRIu64 " entry_id=%" PRIu64 "\n",
			query_reply.entries_filled, query_reply.done,
			(uint64_t)query_reply.next_id,
			query_reply.entries_filled ?
				(uint64_t)query_buf.entries[0].map_id :
				(uint64_t)0);
		goto out;
	}

	if (child_fill_remote_range(cmd_fd, resp_fd, stealth_target_addr + 96, 32,
				    0xC7) < 0) {
		fprintf(stderr, "stealth target child write-through failed\n");
		goto out;
	}
	memset(expected_buf, 0xC7, 32);
	if (memcmp(local_map + 96, expected_buf, 32) != 0) {
		fprintf(stderr,
			"stealth target local page did not reflect target write\n");
		goto out;
	}

	if (remove_remote_map(session_fd, stealth_inject_reply.map_id,
			      &remove_reply) < 0)
		goto out;
	if (remove_reply.map_id != stealth_inject_reply.map_id ||
	    remove_reply.local_addr != (uintptr_t)local_map ||
	    remove_reply.remote_addr != stealth_target_addr ||
	    remove_reply.mapped_length != info->page_size ||
	    remove_reply.prot != (LKMDBG_REMOTE_MAP_PROT_READ |
				  LKMDBG_REMOTE_MAP_PROT_WRITE) ||
	    remove_reply.flags !=
		    (LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET |
		     LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET)) {
		fprintf(stderr,
			"stealth target remove reply mismatch id=%" PRIu64 " local=0x%" PRIx64 " remote=0x%" PRIx64 " len=%" PRIu64 " prot=0x%x flags=0x%x\n",
			(uint64_t)remove_reply.map_id,
			(uint64_t)remove_reply.local_addr,
			(uint64_t)remove_reply.remote_addr,
			(uint64_t)remove_reply.mapped_length,
			remove_reply.prot, remove_reply.flags);
		goto out;
	}

	if (read_maps_line_for_pid_addr(child, stealth_target_addr,
					target_maps_after,
					sizeof(target_maps_after)) < 0)
		goto out;
	if (strcmp(target_maps_before, target_maps_after) != 0) {
		fprintf(stderr,
			"stealth target remove changed target /proc/maps entry\n");
		goto out;
	}

	memset(verify_buf, 0, info->page_size);
	if (child_read_remote_range(cmd_fd, resp_fd, stealth_target_addr,
				    verify_buf, info->page_size) < 0 ||
	    verify_pattern_range(verify_buf, info->page_size, 1,
				 info->page_size * 8U) != 0) {
		fprintf(stderr, "stealth target restore mismatch\n");
		goto out;
	}

	memset(query_buf.entries, 0,
	       REMOTE_MAP_QUERY_BATCH * sizeof(*query_buf.entries));
	if (query_remote_maps(session_fd, 0, &query_buf, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 0 || !query_reply.done ||
	    query_reply.next_id != 0) {
		fprintf(stderr,
			"stealth target query not empty after remove filled=%u done=%u next=%" PRIu64 "\n",
			query_reply.entries_filled, query_reply.done,
			(uint64_t)query_reply.next_id);
		goto out;
	}

	printf("selftest remote map ok export_len=%" PRIu64 " fd=%d inject_addr=0x%" PRIx64 " stealth_len=%" PRIu64 " stealth_target=0x%" PRIxPTR "\n",
	       (uint64_t)reply.mapped_length, reply.map_fd,
	       (uint64_t)inject_reply.remote_addr,
	       (uint64_t)stealth_reply.mapped_length, stealth_target_addr);
	ret = 0;

out:
	if (wake_thread_started)
		pthread_join(wake_thread, NULL);
	if (restore_needed && view != MAP_FAILED) {
		size_t i;

		for (i = 0; i < 64; i++)
			view[128 + i] = pattern_byte(128 + i, 1);
	}
	if (stealth_restore_needed && stealth_view != MAP_FAILED) {
		size_t i;

		for (i = 0; i < 64; i++)
			stealth_view[256 + i] = pattern_byte(256 + i, 1);
	}
	if (ro_view != MAP_FAILED)
		munmap(ro_view, ro_reply.mapped_length);
	if (ro_reply.map_fd >= 0)
		close(ro_reply.map_fd);
	if (local_map != MAP_FAILED)
		munmap(local_map, info->page_size);
	if (local_fd >= 0)
		close(local_fd);
	if (!stealth_map_active && stealth_view != MAP_FAILED)
		munmap(stealth_view, test_len);
	if (!stealth_map_active && stealth_fd >= 0)
		close(stealth_fd);
	if (view != MAP_FAILED)
		munmap(view, reply.mapped_length);
	if (reply.map_fd >= 0)
		close(reply.map_fd);
	free(query_buf.entries);
	free(expected_buf);
	free(verify_buf);
	return ret;
}

static int verify_remote_alloc(int session_fd, pid_t child,
			       const struct child_info *info, int cmd_fd,
			       int resp_fd)
{
	struct lkmdbg_remote_alloc_request reply;
	struct lkmdbg_remote_alloc_handle_request remove_reply;
	struct lkmdbg_remote_alloc_query_request query_reply;
	struct remote_alloc_query_buffer query_buf = { 0 };
	unsigned char *verify_buf = NULL;
	unsigned char expected_buf[64];
	char maps_before[512];
	char maps_after[512];
	uint32_t bytes_done = 0;
	uintptr_t shell_addr;
	size_t shell_offset;
	size_t test_len;
	int ret = -1;

	memset(&reply, 0, sizeof(reply));
	memset(&remove_reply, 0, sizeof(remove_reply));
	memset(&query_reply, 0, sizeof(query_reply));

	query_buf.entries =
		calloc(REMOTE_ALLOC_QUERY_BATCH, sizeof(*query_buf.entries));
	if (!query_buf.entries) {
		fprintf(stderr, "remote alloc query allocation failed\n");
		goto out;
	}

	test_len = info->page_size * 2U;
	shell_offset = info->page_size * 8U;
	if (shell_offset + test_len > info->large_len) {
		fprintf(stderr, "remote alloc test range exceeds child buffer\n");
		goto out;
	}
	shell_addr = info->large_addr + shell_offset;

	verify_buf = malloc(test_len);
	if (!verify_buf) {
		fprintf(stderr, "remote alloc verification allocation failed\n");
		goto out;
	}

	if (read_maps_line_for_pid_addr(child, shell_addr, maps_before,
					sizeof(maps_before)) < 0)
		goto out;

	if (create_remote_alloc(session_fd, shell_addr, test_len,
				LKMDBG_REMOTE_ALLOC_PROT_READ |
					LKMDBG_REMOTE_ALLOC_PROT_WRITE,
				&reply) < 0)
		goto out;

	if (!reply.alloc_id || reply.remote_addr != shell_addr ||
	    reply.mapped_length != test_len ||
	    reply.prot != (LKMDBG_REMOTE_ALLOC_PROT_READ |
			   LKMDBG_REMOTE_ALLOC_PROT_WRITE)) {
		fprintf(stderr,
			"remote alloc reply mismatch id=%" PRIu64 " addr=0x%" PRIx64 " len=%" PRIu64 " prot=0x%x\n",
			(uint64_t)reply.alloc_id, (uint64_t)reply.remote_addr,
			(uint64_t)reply.mapped_length, reply.prot);
		goto out;
	}

	if (read_maps_line_for_pid_addr(child, shell_addr, maps_after,
					sizeof(maps_after)) < 0)
		goto out;
	if (strcmp(maps_before, maps_after) != 0) {
		fprintf(stderr, "remote alloc changed target /proc/maps entry\n");
		goto out;
	}

	memset(verify_buf, 0xA5, test_len);
	if (read_target_memory(session_fd, shell_addr, verify_buf, test_len,
			       &bytes_done, 0) < 0 ||
	    bytes_done != test_len) {
		fprintf(stderr,
			"remote alloc initial readback failed bytes_done=%u expected=%zu\n",
			bytes_done, test_len);
		goto out;
	}
	{
		size_t i;

		for (i = 0; i < test_len; i++) {
			if (verify_buf[i] == 0)
				continue;
			fprintf(stderr, "remote alloc pages are not zero-filled\n");
			goto out;
		}
	}

	fill_pattern(expected_buf, sizeof(expected_buf), 77);
	if (write_target_memory(session_fd, shell_addr + 128, expected_buf,
				sizeof(expected_buf), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(expected_buf)) {
		fprintf(stderr,
			"remote alloc kernel write failed bytes_done=%u expected=%zu\n",
			bytes_done, sizeof(expected_buf));
		goto out;
	}

	memset(verify_buf, 0, test_len);
	if (child_read_remote_range(cmd_fd, resp_fd, shell_addr, verify_buf,
				    test_len) < 0) {
		fprintf(stderr, "remote alloc child readback failed\n");
		goto out;
	}
	if (memcmp(verify_buf + 128, expected_buf, sizeof(expected_buf)) != 0) {
		fprintf(stderr, "remote alloc child view mismatch after write\n");
		goto out;
	}

	if (child_fill_remote_range(cmd_fd, resp_fd, shell_addr + 256, 64,
				    0x6D) < 0) {
		fprintf(stderr, "remote alloc child fill failed\n");
		goto out;
	}

	memset(verify_buf, 0, test_len);
	if (read_target_memory(session_fd, shell_addr, verify_buf, test_len,
			       &bytes_done, 0) < 0 ||
	    bytes_done != test_len) {
		fprintf(stderr,
			"remote alloc post-fill readback failed bytes_done=%u expected=%zu\n",
			bytes_done, test_len);
		goto out;
	}
	{
		size_t i;

		for (i = 0; i < 64; i++) {
			if (verify_buf[256 + i] == 0x6D)
				continue;
			fprintf(stderr, "remote alloc child fill did not stick\n");
			goto out;
		}
	}

	if (query_remote_allocs(session_fd, 0, &query_buf, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 1 || !query_reply.done ||
	    query_reply.next_id != reply.alloc_id ||
	    query_buf.entries[0].alloc_id != reply.alloc_id ||
	    query_buf.entries[0].target_tgid != child ||
	    query_buf.entries[0].remote_addr != shell_addr ||
	    query_buf.entries[0].mapped_length != test_len ||
	    query_buf.entries[0].prot != (LKMDBG_REMOTE_ALLOC_PROT_READ |
					  LKMDBG_REMOTE_ALLOC_PROT_WRITE)) {
		fprintf(stderr,
			"remote alloc query mismatch filled=%u done=%u next=%" PRIu64 " entry_id=%" PRIu64 "\n",
			query_reply.entries_filled, query_reply.done,
			(uint64_t)query_reply.next_id,
			query_reply.entries_filled ?
				(uint64_t)query_buf.entries[0].alloc_id :
				(uint64_t)0);
		goto out;
	}

	if (remove_remote_alloc(session_fd, reply.alloc_id, &remove_reply) < 0)
		goto out;
	if (remove_reply.alloc_id != reply.alloc_id ||
	    remove_reply.target_tgid != child ||
	    remove_reply.remote_addr != shell_addr ||
	    remove_reply.mapped_length != test_len ||
	    remove_reply.prot != (LKMDBG_REMOTE_ALLOC_PROT_READ |
				  LKMDBG_REMOTE_ALLOC_PROT_WRITE)) {
		fprintf(stderr,
			"remote alloc remove mismatch id=%" PRIu64 " tgid=%d addr=0x%" PRIx64 " len=%" PRIu64 " prot=0x%x\n",
			(uint64_t)remove_reply.alloc_id, remove_reply.target_tgid,
			(uint64_t)remove_reply.remote_addr,
			(uint64_t)remove_reply.mapped_length, remove_reply.prot);
		goto out;
	}

	if (read_maps_line_for_pid_addr(child, shell_addr, maps_after,
					sizeof(maps_after)) < 0)
		goto out;
	if (strcmp(maps_before, maps_after) != 0) {
		fprintf(stderr,
			"remote alloc remove changed target /proc/maps entry\n");
		goto out;
	}

	memset(verify_buf, 0, test_len);
	if (child_read_remote_range(cmd_fd, resp_fd, shell_addr, verify_buf,
				    test_len) < 0) {
		fprintf(stderr, "remote alloc restore child read failed\n");
		goto out;
	}
	if (verify_pattern_range(verify_buf, test_len, 1, shell_offset) != 0) {
		fprintf(stderr, "remote alloc baseline restore mismatch\n");
		goto out;
	}

	memset(query_buf.entries, 0,
	       REMOTE_ALLOC_QUERY_BATCH * sizeof(*query_buf.entries));
	if (query_remote_allocs(session_fd, 0, &query_buf, &query_reply) < 0)
		goto out;
	if (query_reply.entries_filled != 0 || !query_reply.done ||
	    query_reply.next_id != 0) {
		fprintf(stderr,
			"remote alloc query not empty after remove filled=%u done=%u next=%" PRIu64 "\n",
			query_reply.entries_filled, query_reply.done,
			(uint64_t)query_reply.next_id);
		goto out;
	}

	ret = 0;
out:
	free(verify_buf);
	free(query_buf.entries);
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

		if (query_target_vmas_ex(session_fd, cursor,
					 LKMDBG_VMA_QUERY_FLAG_FULL_PATH, 0, 0,
					 0, 0, &buf, &reply) < 0)
			goto out;

		if (!cursor)
			printf("vma.generation=0x%" PRIx64 "\n",
			       (uint64_t)reply.generation);

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
			       " phys=0x%" PRIx64 " pt_flags=0x%x"
			       " pgoff=0x%" PRIx64 " inode=%" PRIu64 " dev=%u:%u\n",
			       (uint64_t)buf.entries[i].page_addr,
			       buf.entries[i].flags,
			       (uint64_t)buf.entries[i].vm_flags_raw,
			       page_level_name(buf.entries[i].pt_level),
			       buf.entries[i].page_shift,
			       (uint64_t)buf.entries[i].pt_entry_raw,
			       (uint64_t)buf.entries[i].phys_addr,
			       buf.entries[i].pt_flags,
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
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	void *fault_src;
	int attempt;
	int ret = -1;

	if (!page_size) {
		fprintf(stderr, "partial WRITE_MEM page_size unavailable\n");
		return -1;
	}

	fault_src = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (fault_src == MAP_FAILED) {
		fprintf(stderr, "partial WRITE_MEM fault mapping failed: %s\n",
			strerror(errno));
		return -1;
	}
	if (mprotect(fault_src, page_size, PROT_NONE) < 0) {
		fprintf(stderr, "partial WRITE_MEM fault mprotect failed: %s\n",
			strerror(errno));
		goto out;
	}

	memset(ops, 0, sizeof(ops));
	ops[0].remote_addr = remote_addr;
	ops[0].local_addr = (uintptr_t)payload;
	ops[0].length = (uint32_t)payload_len;
	ops[1].remote_addr = remote_addr + 32;
	ops[1].local_addr = (uintptr_t)fault_src;
	ops[1].length = 8;

	memset(&req, 0, sizeof(req));
	req.version = LKMDBG_PROTO_VERSION;
	req.size = sizeof(req);
	req.ops_addr = (uintptr_t)ops;
	req.op_count = 2;

		for (attempt = 0; attempt < 8; attempt++) {
			errno = 0;
			if (xfer_target_memory(session_fd, ops, 2, 1, &req, 0) == 0) {
				printf("partial WRITE_MEM ioctl success ops_done=%u bytes_done=%" PRIu64 " op0=%u op1=%u\n",
				       req.ops_done, (uint64_t)req.bytes_done,
				       ops[0].bytes_done, ops[1].bytes_done);
		} else {
			if (errno != EFAULT) {
				fprintf(stderr,
					"partial WRITE_MEM errno=%d expected=%d\n",
					errno, EFAULT);
				goto out;
			}
			printf("partial WRITE_MEM ioctl fault ops_done=%u bytes_done=%" PRIu64 " op0=%u op1=%u\n",
			       req.ops_done, (uint64_t)req.bytes_done,
			       ops[0].bytes_done, ops[1].bytes_done);
		}

		if (ops[0].bytes_done == payload_len)
			break;
		usleep(1000);
	}
	if (ops[0].bytes_done != payload_len) {
		fprintf(stderr,
			"partial WRITE_MEM no progress after retries ops_done=%u bytes_done=%" PRIu64 " op0=%u op1=%u\n",
			req.ops_done, (uint64_t)req.bytes_done,
			ops[0].bytes_done, ops[1].bytes_done);
		goto out;
	}

	memset(readback, 0, sizeof(readback));
	if (read_target_memory(session_fd, remote_addr, readback, payload_len,
			       &bytes_done, 0) < 0 ||
	    bytes_done != payload_len || strcmp(readback, payload) != 0) {
		fprintf(stderr,
			"partial WRITE_MEM first op readback failed bytes_done=%u\n",
			bytes_done);
		goto out;
	}

	ret = 0;

out:
	if (munmap(fault_src, page_size) < 0) {
		fprintf(stderr, "partial WRITE_MEM fault munmap failed: %s\n",
			strerror(errno));
		ret = -1;
	}
	return ret;
}

static int open_target_session(pid_t child)
{
	int session_fd;

	session_fd = open_session_fd();
	if (session_fd < 0)
		return -1;
	if (set_target(session_fd, child) < 0) {
		close(session_fd);
		return -1;
	}

	return session_fd;
}

static int verify_remote_string(int session_fd, uintptr_t remote_addr,
				const char *expected)
{
	char buf[SELFTEST_SLOT_SIZE];
	uint32_t bytes_done = 0;
	size_t len = strlen(expected) + 1;

	memset(buf, 0, sizeof(buf));
	if (len > sizeof(buf)) {
		fprintf(stderr, "verify remote string length too large\n");
		return -1;
	}
	if (read_target_memory(session_fd, remote_addr, buf, len, &bytes_done,
			       0) < 0 ||
	    bytes_done != len || strcmp(buf, expected) != 0) {
		fprintf(stderr,
			"remote string verify failed addr=0x%" PRIxPTR " bytes_done=%u got=%s expected=%s\n",
			remote_addr, bytes_done, buf, expected);
		return -1;
	}

	return 0;
}

static int wait_for_target_exit_event(int session_fd, pid_t child,
				      struct lkmdbg_event_record *event_out)
{
	struct lkmdbg_event_record event;
	int waited = 0;

	while (waited < 5000) {
		int slice = 5000 - waited;
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

		if (event.type != LKMDBG_EVENT_TARGET_EXIT)
			continue;
		if (event.tgid != child) {
			fprintf(stderr,
				"target exit event tgid mismatch got=%d expected=%d\n",
				event.tgid, child);
			return -1;
		}
		if (event.tid <= 0) {
			fprintf(stderr, "target exit event tid invalid=%d\n",
				event.tid);
			return -1;
		}
		if (event.tid != child) {
			printf("selftest target exit cleanup stage=ignoring thread-exit tid=%d tgid=%d code=%" PRIu64 "\n",
			       event.tid, event.tgid, (uint64_t)event.value0);
			fflush(stdout);
			continue;
		}
		if (event_out)
			*event_out = event;
		return 0;
	}

	fprintf(stderr, "target exit leader event timed out child=%d\n", child);
	return -1;
}

static int expect_dead_mem_accesses_fail(int session_fd,
					 const struct child_info *info)
{
	unsigned char read_buf[64] = { 0 };
	unsigned char write_buf[64];
	struct lkmdbg_mem_op ops[2];
	struct lkmdbg_mem_request req;
	size_t payload_len = sizeof("dead-target-write");
	unsigned int i;

	memset(write_buf, 0x4b, sizeof(write_buf));

	memset(ops, 0, sizeof(ops));
	ops[0].remote_addr = info->basic_addr;
	ops[0].local_addr = (uintptr_t)read_buf;
	ops[0].length = sizeof(read_buf);
	memset(&req, 0, sizeof(req));
	errno = 0;
	if (xfer_target_memory(session_fd, ops, 1, 0, &req, 0) == 0 ||
	    errno != ESRCH || req.ops_done || req.bytes_done ||
	    ops[0].bytes_done) {
		fprintf(stderr,
			"dead target READ_MEM mismatch errno=%d ops_done=%u bytes_done=%" PRIu64 " op0=%u\n",
			errno, req.ops_done, (uint64_t)req.bytes_done,
			ops[0].bytes_done);
		return -1;
	}

	memset(ops, 0, sizeof(ops));
	ops[0].remote_addr = info->basic_addr;
	ops[0].local_addr = (uintptr_t)write_buf;
	ops[0].length = payload_len;
	memset(&req, 0, sizeof(req));
	errno = 0;
	if (xfer_target_memory(session_fd, ops, 1, 1, &req, 0) == 0 ||
	    errno != ESRCH || req.ops_done || req.bytes_done ||
	    ops[0].bytes_done) {
		fprintf(stderr,
			"dead target WRITE_MEM mismatch errno=%d ops_done=%u bytes_done=%" PRIu64 " op0=%u\n",
			errno, req.ops_done, (uint64_t)req.bytes_done,
			ops[0].bytes_done);
		return -1;
	}

	memset(ops, 0, sizeof(ops));
	for (i = 0; i < 2; i++) {
		ops[i].remote_addr = info->large_addr + (uintptr_t)(i * 64U);
		ops[i].local_addr = (uintptr_t)(read_buf + (i * 16U));
		ops[i].length = 16;
	}
	memset(&req, 0, sizeof(req));
	errno = 0;
	if (xfer_target_memory(session_fd, ops, 2, 0, &req, 0) == 0 ||
	    errno != ESRCH || req.ops_done || req.bytes_done ||
	    ops[0].bytes_done || ops[1].bytes_done) {
		fprintf(stderr,
			"dead target READ_MEMV mismatch errno=%d ops_done=%u bytes_done=%" PRIu64 " op0=%u op1=%u\n",
			errno, req.ops_done, (uint64_t)req.bytes_done,
			ops[0].bytes_done, ops[1].bytes_done);
		return -1;
	}

	memset(ops, 0, sizeof(ops));
	for (i = 0; i < 2; i++) {
		ops[i].remote_addr = info->large_addr + (uintptr_t)(i * 64U);
		ops[i].local_addr = (uintptr_t)(write_buf + (i * 16U));
		ops[i].length = 16;
	}
	memset(&req, 0, sizeof(req));
	errno = 0;
	if (xfer_target_memory(session_fd, ops, 2, 1, &req, 0) == 0 ||
	    errno != ESRCH || req.ops_done || req.bytes_done ||
	    ops[0].bytes_done || ops[1].bytes_done) {
		fprintf(stderr,
			"dead target WRITE_MEMV mismatch errno=%d ops_done=%u bytes_done=%" PRIu64 " op0=%u op1=%u\n",
			errno, req.ops_done, (uint64_t)req.bytes_done,
			ops[0].bytes_done, ops[1].bytes_done);
		return -1;
	}

	printf("selftest target exit hard failure ok errno=%d\n", ESRCH);
	return 0;
}

static void *delayed_signal_thread_main(void *arg)
{
	struct delayed_signal_ctx *ctx = arg;

	usleep(ctx->delay_us);
	errno = 0;
	ctx->ret = kill(ctx->pid, ctx->signo);
	ctx->err = ctx->ret < 0 ? errno : 0;
	return NULL;
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
static volatile uint64_t child_remote_thread_counter;
static volatile uint64_t child_remote_thread_arg_seen;
static volatile pid_t child_remote_thread_tid;

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

static __attribute__((noinline)) uint64_t
child_remote_call_target(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	asm volatile("" ::: "memory");
	return (a0 ^ (a1 << 1)) + (a2 * 3U) + (a3 ^ 0x13579BDF2468ACE0ULL);
}

static __attribute__((noinline)) uint64_t
child_remote_call_x8_target(uint64_t marker)
{
	uint64_t x8_value;

	asm volatile("mov %0, x8" : "=r"(x8_value));
	return x8_value ^ marker;
}

static void *child_remote_thread_start(void *arg)
{
	child_remote_thread_arg_seen = (uint64_t)(uintptr_t)arg;
	child_remote_thread_tid = (pid_t)syscall(SYS_gettid);
	__sync_fetch_and_add(&child_remote_thread_counter, 1);
	return NULL;
}

static __attribute__((noinline)) int64_t
child_remote_thread_launcher(uint64_t start_pc, uint64_t start_arg,
			     uint64_t stack_top, uint64_t tls)
{
	pthread_t thread;
	void *(*start_fn)(void *) = (void *(*)(void *))(uintptr_t)start_pc;
	int i;

	(void)stack_top;
	(void)tls;

	child_remote_thread_tid = 0;
	child_remote_thread_arg_seen = 0;
	if (pthread_create(&thread, NULL, start_fn,
			   (void *)(uintptr_t)start_arg) != 0)
		return -1;
	pthread_detach(thread);

	for (i = 0; i < 100000; i++) {
		if (child_remote_thread_tid > 0)
			return child_remote_thread_tid;
		sched_yield();
	}

	return -2;
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
	unsigned char *view_exec_map;
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

	view_exec_map = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (view_exec_map == MAP_FAILED)
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
	if (build_view_exec_page(view_exec_map, page_size, 17U) < 0)
		return 2;
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
	info.view_exec_addr = (uintptr_t)view_exec_map;
	info.file_addr = (uintptr_t)file_map;
	info.exec_target_addr = (uintptr_t)child_exec_target;
	info.remote_call_addr = (uintptr_t)child_remote_call_target;
	info.remote_call_x8_addr = (uintptr_t)child_remote_call_x8_target;
	info.remote_thread_launcher_addr =
		(uintptr_t)child_remote_thread_launcher;
	info.remote_thread_start_addr = (uintptr_t)child_remote_thread_start;
	info.remote_thread_tid_addr = (uintptr_t)&child_remote_thread_tid;
	info.remote_thread_arg_addr = (uintptr_t)&child_remote_thread_arg_seen;
	info.remote_thread_counter_addr =
		(uintptr_t)&child_remote_thread_counter;
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
		case CHILD_OP_TRIGGER_SYSCALL:
		{
			int64_t retval;
			long syscall_nr = cmd.addr ? (long)cmd.addr : SYS_getppid;

			if (cmd.length)
				retval =
					child_raw_syscall1(syscall_nr, cmd.value);
			else
				retval = child_raw_syscall0(syscall_nr);

			if (write_full(resp_fd, &retval, sizeof(retval)) !=
			    (ssize_t)sizeof(retval))
				return 2;
			break;
		}
		case CHILD_OP_PING:
			resident = 0;
			if (write_full(resp_fd, &resident, sizeof(resident)) !=
			    (ssize_t)sizeof(resident))
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
		case CHILD_OP_WRITE_REMOTE:
		{
			void *tmp;

			resident = 0;
			if (!cmd.addr || !cmd.length ||
			    cmd.length > SELFTEST_LARGE_MAP_LEN)
				return 2;
			tmp = malloc(cmd.length);
			if (!tmp)
				return 2;
			if (read_full(cmd_fd, tmp, cmd.length) !=
			    (ssize_t)cmd.length) {
				free(tmp);
				return 2;
			}
			memcpy((void *)(uintptr_t)cmd.addr, tmp, cmd.length);
			free(tmp);
			if (write_full(resp_fd, &resident, sizeof(resident)) !=
			    (ssize_t)sizeof(resident))
				return 2;
			break;
		}
		case CHILD_OP_MPROTECT:
			resident = 0;
			if (!cmd.addr || !cmd.length)
				return 2;
			if (mprotect((void *)(uintptr_t)cmd.addr, cmd.length,
				     (int)cmd.value) < 0)
				resident = -errno;
			if (write_full(resp_fd, &resident, sizeof(resident)) !=
			    (ssize_t)sizeof(resident))
				return 2;
			break;
		case CHILD_OP_CALL_REMOTE:
		{
			uint64_t (*fn)(void);
			uint64_t retval;

			if (!cmd.addr)
				return 2;
			__builtin___clear_cache((char *)(uintptr_t)cmd.addr,
						(char *)(uintptr_t)cmd.addr + 64);
			fn = (uint64_t (*)(void))(uintptr_t)cmd.addr;
			retval = fn();
			if (write_full(resp_fd, &retval, sizeof(retval)) !=
			    (ssize_t)sizeof(retval))
				return 2;
			break;
		}
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

static int child_ping(int cmd_fd, int resp_fd)
{
	struct child_cmd cmd = {
		.op = CHILD_OP_PING,
	};
	int ack;
	unsigned int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		if (write_full(cmd_fd, &cmd, sizeof(cmd)) !=
		    (ssize_t)sizeof(cmd)) {
			if (attempt < 2) {
				usleep(10000);
				continue;
			}
			fprintf(stderr, "failed to send child ping\n");
			return -1;
		}

		if (read_full(resp_fd, &ack, sizeof(ack)) ==
		    (ssize_t)sizeof(ack))
			return 0;
		if (attempt < 2) {
			usleep(10000);
			continue;
		}
		fprintf(stderr, "failed to read child ping ack\n");
		return -1;
	}

	return -1;
}

static int child_trigger_syscall_ex(int cmd_fd, int resp_fd, long syscall_nr,
				    uint64_t arg0, int use_arg0,
				    int64_t *retval_out)
{
	struct child_cmd cmd = {
		.op = CHILD_OP_TRIGGER_SYSCALL,
		.addr = (uint64_t)syscall_nr,
		.length = use_arg0 ? 1U : 0U,
		.value = (uint32_t)arg0,
	};
	int64_t retval;

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "failed to send child syscall trigger\n");
		return -1;
	}

	if (read_full(resp_fd, &retval, sizeof(retval)) !=
	    (ssize_t)sizeof(retval)) {
		fprintf(stderr, "failed to read child syscall retval\n");
		return -1;
	}

	if (retval_out)
		*retval_out = retval;

	return 0;
}

static int child_trigger_syscall(int cmd_fd, int resp_fd, int64_t *retval_out)
{
	return child_trigger_syscall_ex(cmd_fd, resp_fd, SYS_getppid, 0, 0,
					retval_out);
}

static int child_mprotect_range(int cmd_fd, int resp_fd, uintptr_t addr,
				size_t len, int prot)
{
	struct child_cmd cmd = {
		.op = CHILD_OP_MPROTECT,
		.addr = addr,
		.length = (uint32_t)len,
		.value = (uint32_t)prot,
	};
	int ret;

	if (!addr || !len || len > UINT32_MAX)
		return -1;

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "failed to send child mprotect command\n");
		return -1;
	}

	if (read_full(resp_fd, &ret, sizeof(ret)) != (ssize_t)sizeof(ret)) {
		fprintf(stderr, "failed to read child mprotect reply\n");
		return -1;
	}
	if (ret) {
		fprintf(stderr, "child mprotect failed ret=%d\n", ret);
		return -1;
	}

	return 0;
}

static int child_read_syscall_result(int resp_fd, int64_t *retval_out)
{
	int64_t retval;

	if (read_full(resp_fd, &retval, sizeof(retval)) !=
	    (ssize_t)sizeof(retval)) {
		fprintf(stderr, "failed to read child syscall completion\n");
		return -1;
	}

	if (retval_out)
		*retval_out = retval;
	return 0;
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

static int request_child_exit(int cmd_fd)
{
	return send_child_command(cmd_fd, CHILD_OP_EXIT);
}

static int wait_for_child_exit(pid_t child, int *status_out)
{
	int status;

	if (waitpid(child, &status, 0) < 0) {
		fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
		return -1;
	}

	if (status_out)
		*status_out = status;
	return 0;
}

static int stop_selftest_child(pid_t child, int cmd_fd, int resp_fd,
			       int send_exit)
{
	int status;

	if (send_exit && request_child_exit(cmd_fd) < 0)
		fprintf(stderr, "failed to send child exit command\n");
	close(cmd_fd);
	close(resp_fd);
	if (wait_for_child_exit(child, &status) < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "child exited abnormally status=%d\n", status);
		return -1;
	}

	return 0;
}

static int start_selftest_child(pid_t *child_out, int *info_fd_out,
				int *cmd_fd_out, int *resp_fd_out,
				struct child_info *info_out)
{
	int info_pipe[2];
	int cmd_pipe[2];
	int resp_pipe[2];
	pid_t child;

	if (!child_out || !info_fd_out || !cmd_fd_out || !resp_fd_out ||
	    !info_out)
		return -1;

	if (pipe(info_pipe) < 0 || pipe(cmd_pipe) < 0 || pipe(resp_pipe) < 0) {
		fprintf(stderr, "pipe failed: %s\n", strerror(errno));
		return -1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "fork failed: %s\n", strerror(errno));
		close(info_pipe[0]);
		close(info_pipe[1]);
		close(cmd_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		close(resp_pipe[1]);
		return -1;
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
	if (read_full(info_pipe[0], info_out, sizeof(*info_out)) !=
	    (ssize_t)sizeof(*info_out)) {
		fprintf(stderr, "failed to read child info\n");
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return -1;
	}

	*child_out = child;
	*info_fd_out = info_pipe[0];
	*cmd_fd_out = cmd_pipe[1];
	*resp_fd_out = resp_pipe[0];
	return 0;
}

static int continue_target_and_wait_for_user_loop(int session_fd,
						  uint64_t cookie, int cmd_fd,
						  int resp_fd)
{
	if (continue_target(session_fd, cookie, 2000, 0, NULL) < 0)
		return -1;

	return child_ping(cmd_fd, resp_fd);
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

static int child_write_remote_range(int cmd_fd, int resp_fd, uintptr_t addr,
				    const void *buf, size_t len)
{
	struct child_cmd cmd = {
		.op = CHILD_OP_WRITE_REMOTE,
		.addr = addr,
		.length = (uint32_t)len,
	};
	int ret;

	if (!buf || !len || len > UINT32_MAX)
		return -1;

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "failed to send child remote write command\n");
		return -1;
	}
	if (write_full(cmd_fd, buf, len) != (ssize_t)len) {
		fprintf(stderr, "failed to send child remote write payload\n");
		return -1;
	}
	if (read_full(resp_fd, &ret, sizeof(ret)) != (ssize_t)sizeof(ret)) {
		fprintf(stderr, "failed to read child remote write reply\n");
		return -1;
	}
	if (ret) {
		fprintf(stderr, "child remote write failed ret=%d\n", ret);
		return -1;
	}

	return 0;
}

static int child_call_remote0(int cmd_fd, int resp_fd, uintptr_t addr,
			      uint64_t *retval_out)
{
	struct child_cmd cmd = {
		.op = CHILD_OP_CALL_REMOTE,
		.addr = addr,
	};
	uint64_t retval;

	if (!addr)
		return -1;

	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		fprintf(stderr, "failed to send child remote call command\n");
		return -1;
	}

	if (read_full(resp_fd, &retval, sizeof(retval)) !=
	    (ssize_t)sizeof(retval)) {
		fprintf(stderr, "failed to read child remote call reply\n");
		return -1;
	}

	if (retval_out)
		*retval_out = retval;
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

static int proc_modules_has_name(const char *name)
{
	char buf[8192];
	ssize_t nr;
	int fd;

	if (!name || !*name)
		return -1;

	fd = open("/proc/modules", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open(/proc/modules) failed: %s\n",
			strerror(errno));
		return -1;
	}

	nr = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (nr < 0) {
		fprintf(stderr, "read(/proc/modules) failed: %s\n",
			strerror(errno));
		return -1;
	}

	buf[nr] = '\0';
	return strstr(buf, name) != NULL;
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

static int verify_multi_session_conflict(int session_fd, pid_t child,
					 const struct child_info *info)
{
	int aux_sessions[SELFTEST_MULTI_SESSION_COUNT - 1] = { -1, -1 };
	pthread_t threads[SELFTEST_MULTI_SESSION_COUNT - 1];
	struct worker_ctx workers[SELFTEST_MULTI_SESSION_COUNT - 1];
	struct lkmdbg_freeze_request freeze_req;
	struct lkmdbg_stop_query_request stop_req;
	unsigned int i;
	int ret = -1;

	memset(&freeze_req, 0, sizeof(freeze_req));
	memset(&stop_req, 0, sizeof(stop_req));
	memset(workers, 0, sizeof(workers));

	for (i = 0; i < SELFTEST_MULTI_SESSION_COUNT - 1; i++) {
		aux_sessions[i] = open_target_session(child);
		if (aux_sessions[i] < 0)
			goto out;
		workers[i].session_fd = aux_sessions[i];
		workers[i].remote_addr =
			info->slots_addr + (uintptr_t)(i * info->slot_size);
		workers[i].thread_index = i + 10U;
		if (pthread_create(&threads[i], NULL, worker_thread_main,
				   &workers[i]) != 0) {
			fprintf(stderr,
				"multi-session worker create failed index=%u\n", i);
			goto out;
		}
	}

	for (i = 0; i < SELFTEST_MULTI_SESSION_FREEZE_ROUNDS; i++) {
		if (freeze_target_threads(session_fd, 2000, &freeze_req, 0) < 0)
			goto out_join;
		if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_FREEZE,
				      &stop_req) < 0)
			goto out_join;
		if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
				    NULL) < 0)
			goto out_join;
	}

	for (i = 0; i < SELFTEST_MULTI_SESSION_COUNT - 1; i++) {
		pthread_join(threads[i], NULL);
		if (workers[i].failed) {
			fprintf(stderr, "multi-session worker failed index=%u\n",
				i);
			goto out;
		}
		if (verify_remote_string(session_fd, workers[i].remote_addr,
					 workers[i].final_payload) < 0)
			goto out;
	}

	printf("selftest multi-session conflict ok sessions=%u freeze_rounds=%u\n",
	       SELFTEST_MULTI_SESSION_COUNT,
	       SELFTEST_MULTI_SESSION_FREEZE_ROUNDS);
	ret = 0;
	goto out;

out_join:
	for (i = 0; i < SELFTEST_MULTI_SESSION_COUNT - 1; i++)
		pthread_join(threads[i], NULL);
out:
	for (i = 0; i < SELFTEST_MULTI_SESSION_COUNT - 1; i++) {
		if (aux_sessions[i] >= 0)
			close(aux_sessions[i]);
	}
	return ret;
}

static int verify_cross_page_permission_flip(int session_fd,
					     const struct child_info *info,
					     int cmd_fd, int resp_fd)
{
	unsigned char payload[SELFTEST_PERMISSION_SPAN];
	unsigned char readback[SELFTEST_PERMISSION_SPAN];
	unsigned char *original_pages = NULL;
	struct lkmdbg_page_entry page_entries[PAGE_QUERY_BATCH];
	struct lkmdbg_mem_op op;
	struct lkmdbg_mem_request req;
	struct page_query_buffer page_query_buf = {
		.entries = page_entries,
	};
	struct lkmdbg_page_query_request page_query_reply;
	uintptr_t page1;
	uintptr_t page2;
	size_t tracked_len;
	size_t prefix = SELFTEST_PERMISSION_SPAN / 2U;
	size_t i;
	uint32_t bytes_done = 0;
	int ret = -1;

	page1 = info->large_addr + (uintptr_t)(info->page_size * 20U);
	page2 = page1 + info->page_size;
	if (page2 + info->page_size > info->large_addr + info->large_len) {
		fprintf(stderr, "permission flip region out of range\n");
		return -1;
	}
	tracked_len = info->page_size * 2U;
	original_pages = malloc(tracked_len);
	if (!original_pages) {
		fprintf(stderr, "permission flip snapshot allocation failed\n");
		return -1;
	}
	if (read_target_memory_flags(session_fd, page1, original_pages, tracked_len,
				     LKMDBG_MEM_OP_FLAG_FORCE_ACCESS, &bytes_done,
				     0) < 0 ||
	    bytes_done != tracked_len) {
		fprintf(stderr,
			"permission flip snapshot read failed bytes_done=%u expected=%zu\n",
			bytes_done, tracked_len);
		goto out;
	}

	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (unsigned char)(0x60U + i);

	if (child_fill_remote_range(cmd_fd, resp_fd, page1, info->page_size * 2U,
				    0x11U) < 0)
		goto out;
	if (child_mprotect_range(cmd_fd, resp_fd, page2, info->page_size,
				 PROT_NONE) < 0)
		goto out;

	memset(&op, 0, sizeof(op));
	memset(&req, 0, sizeof(req));
	op.remote_addr = page2 - prefix;
	op.local_addr = (uintptr_t)payload;
	op.length = sizeof(payload);
	errno = 0;
	if ((xfer_target_memory(session_fd, &op, 1, 1, &req, 0) < 0 &&
	     errno != EFAULT) ||
	    op.bytes_done != prefix) {
		fprintf(stderr,
				"perm flip PROT_NONE write mismatch errno=%d bytes_done=%u expected=%zu\n",
				errno, op.bytes_done, prefix);
			goto out;
		}
	memset(readback, 0, sizeof(readback));
	if (read_target_memory_flags(session_fd, page2 - prefix, readback,
				     sizeof(readback),
				     LKMDBG_MEM_OP_FLAG_FORCE_ACCESS, NULL, 0) < 0)
		goto out;
	if (memcmp(readback, payload, prefix) != 0) {
		fprintf(stderr, "perm flip PROT_NONE first-page write missing\n");
		goto out;
	}
	for (i = prefix; i < sizeof(readback); i++) {
		if (readback[i] != 0x11U) {
			fprintf(stderr,
				"perm flip PROT_NONE leaked into protected page index=%zu val=0x%x\n",
				i, readback[i]);
			goto out;
		}
	}

	if (child_mprotect_range(cmd_fd, resp_fd, page2, info->page_size,
				 PROT_READ) < 0)
		goto out;
	memset(&op, 0, sizeof(op));
	memset(&req, 0, sizeof(req));
	op.remote_addr = page2 - prefix;
	op.local_addr = (uintptr_t)payload;
	op.length = sizeof(payload);
	errno = 0;
	if ((xfer_target_memory(session_fd, &op, 1, 1, &req, 0) < 0 &&
	     errno != EFAULT) ||
	    op.bytes_done != prefix) {
		fprintf(stderr,
				"perm flip PROT_READ write mismatch errno=%d bytes_done=%u expected=%zu\n",
				errno, op.bytes_done, prefix);
			goto out;
		}

	memset(readback, 0, sizeof(readback));
	if (read_target_memory_flags(session_fd, page2 - prefix, readback,
				     sizeof(readback),
				     LKMDBG_MEM_OP_FLAG_FORCE_ACCESS, NULL, 0) < 0 ||
	    memcmp(readback, payload, prefix) != 0) {
		fprintf(stderr,
			"perm flip PROT_READ readback mismatch\n");
		goto out;
	}
	for (i = prefix; i < sizeof(readback); i++) {
		if (readback[i] != 0x11U) {
			fprintf(stderr,
				"perm flip PROT_READ readback mismatch index=%zu val=0x%x\n",
				i, readback[i]);
			goto out;
		}
	}

	if (child_mprotect_range(cmd_fd, resp_fd, page2, info->page_size,
				 PROT_READ | PROT_WRITE) < 0)
		goto out;
	bytes_done = 0;
	if (write_target_memory(session_fd, page2 - prefix, payload, prefix,
				&bytes_done, 0) < 0 ||
	    bytes_done != prefix) {
		fprintf(stderr,
			"perm flip restore first-half mismatch bytes_done=%u expected=%zu\n",
			bytes_done, prefix);
		goto out;
	}
	bytes_done = 0;
	if (write_target_memory(session_fd, page2, payload + prefix,
				sizeof(payload) - prefix, &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(payload) - prefix) {
		uint32_t page_flags;

		memset(page_entries, 0, sizeof(page_entries));
		memset(&page_query_reply, 0, sizeof(page_query_reply));
		if (query_target_pages(session_fd, page2, info->page_size,
				       &page_query_buf, &page_query_reply) < 0)
			goto out;
		if (!page_query_reply.entries_filled ||
		    page_entries[0].page_addr != page2) {
			fprintf(stderr,
				"perm flip restore second-half query mismatch filled=%u addr=0x%" PRIx64 "\n",
				page_query_reply.entries_filled,
				page_query_reply.entries_filled ?
					(uint64_t)page_entries[0].page_addr : 0ULL);
			goto out;
		}
		page_flags = page_entries[0].flags;
		if (!(page_flags & LKMDBG_PAGE_FLAG_PROT_WRITE) ||
		    !(page_flags & LKMDBG_PAGE_FLAG_MAYWRITE) ||
		    !(page_flags & LKMDBG_PAGE_FLAG_FORCE_WRITE) ||
		    (page_flags & LKMDBG_PAGE_FLAG_NOFAULT_WRITE)) {
			fprintf(stderr,
				"perm flip restore second-half mismatch bytes_done=%u expected=%zu flags=0x%x\n",
				bytes_done, sizeof(payload) - prefix, page_flags);
			goto out;
		}
		bytes_done = 0;
		if (write_target_memory_flags(session_fd, page2, payload + prefix,
					      sizeof(payload) - prefix,
					      LKMDBG_MEM_OP_FLAG_FORCE_ACCESS,
					      &bytes_done, 0) < 0 ||
		    bytes_done != sizeof(payload) - prefix) {
			fprintf(stderr,
				"perm flip restore second-half force-write mismatch bytes_done=%u expected=%zu flags=0x%x\n",
				bytes_done, sizeof(payload) - prefix, page_flags);
			goto out;
		}
		printf("selftest cross-page permission flip fallback force-write flags=0x%x\n",
		       page_flags);
	}
	memset(readback, 0, sizeof(readback));
	if (read_target_memory_flags(session_fd, page2 - prefix, readback,
				     sizeof(readback),
				     LKMDBG_MEM_OP_FLAG_FORCE_ACCESS, NULL, 0) < 0 ||
	    memcmp(readback, payload, sizeof(payload)) != 0) {
		fprintf(stderr, "perm flip restore readback mismatch\n");
		goto out;
	}
	if (write_target_memory(session_fd, page1, original_pages, tracked_len,
				&bytes_done, 0) < 0 ||
	    bytes_done != tracked_len) {
		fprintf(stderr,
			"perm flip snapshot restore failed bytes_done=%u expected=%zu\n",
			bytes_done, tracked_len);
		goto out;
	}

	printf("selftest cross-page permission flip ok span=%u partial=%zu\n",
	       SELFTEST_PERMISSION_SPAN, prefix);
	ret = 0;

out:
	free(original_pages);
	return ret;
}

static int verify_randomized_mem_soak(int session_fd, pid_t child,
				      const struct child_info *info)
{
	uint64_t state = 0x13579bdf2468ace0ULL;
	uint64_t cookie = 0;
	uint64_t active_alloc_id = 0;
	uintptr_t shell_addr = info->large_addr + (uintptr_t)(info->page_size * 28U);
	unsigned int iter;
	int ret = -1;

	if (shell_addr + SELFTEST_SOAK_REMOTE_ALLOC_LEN >
	    info->large_addr + info->large_len) {
		fprintf(stderr, "soak shell range out of bounds\n");
		return -1;
	}

	for (iter = 0; iter < SELFTEST_SOAK_ITERS; iter++) {
		uint32_t bytes_done = 0;
		unsigned int op_sel;
		unsigned int slot;
		uintptr_t slot_addr;
		char payload[SELFTEST_SLOT_SIZE];
		char readback[SELFTEST_SLOT_SIZE];

		state = state * 6364136223846793005ULL + 1ULL;
		op_sel = (unsigned int)(state & 7U);
		slot = (unsigned int)((state >> 8) % info->slot_count);
		slot_addr = info->slots_addr + (uintptr_t)(slot * info->slot_size);

		switch (op_sel) {
		case 0:
			memset(readback, 0, sizeof(readback));
			if (read_target_memory(session_fd, slot_addr, readback,
					       sizeof(readback), &bytes_done,
					       0) < 0 || !bytes_done)
				goto out;
			break;
		case 1:
			snprintf(payload, sizeof(payload), "soak-%03u-%08x", iter,
				 (unsigned int)(state >> 16));
			if (write_target_memory(session_fd, slot_addr, payload,
						strlen(payload) + 1,
						&bytes_done, 0) < 0 ||
			    bytes_done != strlen(payload) + 1)
				goto out;
			break;
		case 2:
		{
			struct lkmdbg_mem_op ops[2];
			struct lkmdbg_mem_request req;

			memset(ops, 0, sizeof(ops));
			memset(&req, 0, sizeof(req));
			ops[0].remote_addr = info->large_addr;
			ops[0].local_addr = (uintptr_t)readback;
			ops[0].length = 16;
			ops[1].remote_addr = info->large_addr + 128U;
			ops[1].local_addr = (uintptr_t)(readback + 16);
			ops[1].length = 16;
			if (xfer_target_memory(session_fd, ops, 2, 0, &req, 0) <
				    0 ||
			    req.ops_done != 2)
				goto out;
			break;
		}
		case 3:
		{
			struct lkmdbg_mem_op ops[2];
			struct lkmdbg_mem_request req;

			snprintf(payload, sizeof(payload), "soakv-%03u", iter);
			memset(ops, 0, sizeof(ops));
			memset(&req, 0, sizeof(req));
			ops[0].remote_addr = slot_addr;
			ops[0].local_addr = (uintptr_t)payload;
			ops[0].length = (uint32_t)(strlen(payload) + 1);
			ops[1].remote_addr = slot_addr + 32U;
			ops[1].local_addr = (uintptr_t)payload;
			ops[1].length = 8;
			if (xfer_target_memory(session_fd, ops, 2, 1, &req, 0) <
				    0 ||
			    req.ops_done != 2)
				goto out;
			break;
		}
		case 4:
		{
			struct page_query_buffer page_buf = { 0 };
			struct lkmdbg_page_entry page_entry;

			page_buf.entries =
				calloc(PAGE_QUERY_BATCH, sizeof(*page_buf.entries));
			if (!page_buf.entries)
				goto out;
			if (lookup_target_page(session_fd, info->large_addr,
					       info->page_size, &page_buf,
					       &page_entry) < 0) {
				free(page_buf.entries);
				goto out;
			}
			free(page_buf.entries);
			break;
		}
		case 5:
		{
			struct vma_query_buffer vma_buf = { 0 };
			struct lkmdbg_vma_entry entry;
			char name_buf[256];

			vma_buf.entries =
				calloc(VMA_QUERY_BATCH, sizeof(*vma_buf.entries));
			vma_buf.names = calloc(1, VMA_QUERY_NAMES_SIZE);
			if (!vma_buf.entries || !vma_buf.names) {
				free(vma_buf.entries);
				free(vma_buf.names);
				goto out;
			}
			if (lookup_target_vma(session_fd, info->large_addr, &vma_buf,
					      &entry, name_buf,
					      sizeof(name_buf)) < 0) {
				free(vma_buf.entries);
				free(vma_buf.names);
				goto out;
			}
			free(vma_buf.entries);
			free(vma_buf.names);
			break;
		}
		case 6:
		{
			struct lkmdbg_freeze_request freeze_req;
			struct lkmdbg_stop_query_request stop_req;

			memset(&freeze_req, 0, sizeof(freeze_req));
			memset(&stop_req, 0, sizeof(stop_req));
			if (freeze_target_threads(session_fd, 2000, &freeze_req, 0) <
			    0)
				goto out;
			if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_FREEZE,
					      &stop_req) < 0)
				goto out;
			cookie = stop_req.stop.cookie;
			if (continue_target(session_fd, cookie, 2000, 0, NULL) < 0)
				goto out;
			cookie = 0;
			break;
		}
		case 7:
		{
			struct lkmdbg_remote_alloc_request alloc_reply;
			struct lkmdbg_remote_alloc_handle_request remove_reply;

			memset(&alloc_reply, 0, sizeof(alloc_reply));
			memset(&remove_reply, 0, sizeof(remove_reply));
			if (!active_alloc_id) {
				if (create_remote_alloc(
					    session_fd, shell_addr,
					    SELFTEST_SOAK_REMOTE_ALLOC_LEN,
					    LKMDBG_REMOTE_ALLOC_PROT_READ |
						    LKMDBG_REMOTE_ALLOC_PROT_WRITE,
					    &alloc_reply) < 0)
					goto out;
				active_alloc_id = alloc_reply.alloc_id;
			} else {
				if (remove_remote_alloc(session_fd, active_alloc_id,
							&remove_reply) < 0)
					goto out;
				active_alloc_id = 0;
			}
			break;
		}
		}
	}

	ret = 0;
	printf("selftest bounded randomized soak ok target=%d iters=%u\n", child,
	       SELFTEST_SOAK_ITERS);

out:
	if (cookie)
		continue_target(session_fd, cookie, 2000, 0, NULL);
	if (active_alloc_id) {
		struct lkmdbg_remote_alloc_handle_request remove_reply;

		memset(&remove_reply, 0, sizeof(remove_reply));
		remove_remote_alloc(session_fd, active_alloc_id, &remove_reply);
	}
	return ret;
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

static int find_first_parked_thread(int session_fd,
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
			if (!(entries[i].flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED))
				continue;

			if (entry_out)
				*entry_out = entries[i];
			return 0;
		}

		if (reply.done)
			break;
		cursor = reply.next_tid;
	}

	fprintf(stderr, "no parked frozen thread found in QUERY_THREADS\n");
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
	printf("features=0x%08x\n", req->regs.features);
	if (req->regs.features & LKMDBG_REGS_ARM64_FEATURE_FP) {
		printf("fpsr=0x%08x\n", req->regs.fpsr);
		printf("fpcr=0x%08x\n", req->regs.fpcr);
		for (i = 0; i < 32; i++) {
			printf("v%u_lo=0x%016" PRIx64 "\n", i,
			       (uint64_t)req->regs.vregs[i].lo);
			printf("v%u_hi=0x%016" PRIx64 "\n", i,
			       (uint64_t)req->regs.vregs[i].hi);
		}
	}
}

static int set_named_arm64_reg(struct lkmdbg_thread_regs_request *req,
			       const char *name, uint64_t value)
{
	unsigned long index;
	char *endp;
	char lane[8];

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

	if (strcmp(name, "fpsr") == 0) {
		req->regs.features |= LKMDBG_REGS_ARM64_FEATURE_FP;
		req->regs.fpsr = (uint32_t)value;
		return 0;
	}

	if (strcmp(name, "fpcr") == 0) {
		req->regs.features |= LKMDBG_REGS_ARM64_FEATURE_FP;
		req->regs.fpcr = (uint32_t)value;
		return 0;
	}

	if (name[0] == 'v') {
		index = strtoul(name + 1, &endp, 10);
		if (*endp != '_' || index >= 32)
			return -1;
		if (snprintf(lane, sizeof(lane), "%s", endp + 1) < 0)
			return -1;
		req->regs.features |= LKMDBG_REGS_ARM64_FEATURE_FP;
		if (strcmp(lane, "lo") == 0) {
			req->regs.vregs[index].lo = value;
			return 0;
		}
		if (strcmp(lane, "hi") == 0) {
			req->regs.vregs[index].hi = value;
			return 0;
		}
		return -1;
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

static const char *describe_syscall_trace_mode(uint32_t mode, char *buf,
					       size_t buf_size)
{
	buf[0] = '\0';

	if (mode & LKMDBG_SYSCALL_TRACE_MODE_EVENT)
		append_flag_name(buf, buf_size, "event");
	if (mode & LKMDBG_SYSCALL_TRACE_MODE_STOP)
		append_flag_name(buf, buf_size, "stop");
	if (mode & LKMDBG_SYSCALL_TRACE_MODE_CONTROL)
		append_flag_name(buf, buf_size, "control");
	if (!buf[0])
		snprintf(buf, buf_size, "off");

	return buf;
}

static const char *describe_syscall_trace_flags(uint32_t flags, char *buf,
						size_t buf_size)
{
	buf[0] = '\0';

	if (flags & LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_TRACEPOINT)
		append_flag_name(buf, buf_size, "tracepoint");
	if (flags & LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_ENTRY_HOOK)
		append_flag_name(buf, buf_size, "entry_hook");
	if (flags & LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_CONTROL)
		append_flag_name(buf, buf_size, "backend_control");
	if (!buf[0])
		snprintf(buf, buf_size, "none");

	return buf;
}

static const char *describe_syscall_trace_phases(uint32_t phases, char *buf,
						 size_t buf_size)
{
	buf[0] = '\0';

	if (phases & LKMDBG_SYSCALL_TRACE_PHASE_ENTER)
		append_flag_name(buf, buf_size, "enter");
	if (phases & LKMDBG_SYSCALL_TRACE_PHASE_EXIT)
		append_flag_name(buf, buf_size, "exit");
	if (!buf[0])
		snprintf(buf, buf_size, "none");

	return buf;
}

static const char *describe_syscall_resolve_action(uint32_t action, char *buf,
						   size_t buf_size)
{
	if (action == LKMDBG_SYSCALL_RESOLVE_ACTION_ALLOW)
		snprintf(buf, buf_size, "allow");
	else if (action == LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE)
		snprintf(buf, buf_size, "rewrite");
	else if (action == LKMDBG_SYSCALL_RESOLVE_ACTION_SKIP)
		snprintf(buf, buf_size, "skip");
	else
		snprintf(buf, buf_size, "unknown");

	return buf;
}

static const char *describe_syscall_resolve_flags(uint32_t flags, char *buf,
						  size_t buf_size)
{
	buf[0] = '\0';

	if (flags & LKMDBG_SYSCALL_RESOLVE_FLAG_NR_REWRITE_SUPPORTED)
		append_flag_name(buf, buf_size, "nr_rewrite");
	if (!buf[0])
		snprintf(buf, buf_size, "none");

	return buf;
}

static const char *describe_stealth_flags(uint32_t flags, char *buf,
					  size_t buf_size)
{
	buf[0] = '\0';

	if (flags & LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE)
		append_flag_name(buf, buf_size, "debugfs");
	if (flags & LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN)
		append_flag_name(buf, buf_size, "modulehide");
	if (flags & LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN)
		append_flag_name(buf, buf_size, "sysfshide");
	if (flags & LKMDBG_STEALTH_FLAG_OWNER_PROC_HIDDEN)
		append_flag_name(buf, buf_size, "ownerprochide");
	if (!buf[0])
		snprintf(buf, buf_size, "none");

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
	uint32_t saved_fpsr = 0;
	uint64_t saved_v0_lo = 0;
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
	if (!(regs_req.regs.features & LKMDBG_REGS_ARM64_FEATURE_FP)) {
		fprintf(stderr, "SET_REGS missing FP feature bit\n");
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}
	saved_fpsr = regs_req.regs.fpsr;
	saved_v0_lo = regs_req.regs.vregs[0].lo;
	regs_req.regs.regs[19] = SELFTEST_REG_SENTINEL;
	regs_req.regs.fpsr = saved_fpsr ^ 0x1U;
	regs_req.regs.vregs[0].lo = saved_v0_lo ^ 0x1ULL;
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
	if (!(regs_req.regs.features & LKMDBG_REGS_ARM64_FEATURE_FP) ||
	    regs_req.regs.fpsr != (saved_fpsr ^ 0x1U) ||
	    regs_req.regs.vregs[0].lo != (saved_v0_lo ^ 0x1ULL)) {
		fprintf(stderr,
			"SET_REGS FP values did not stick features=0x%x fpsr=0x%x v0_lo=0x%" PRIx64 "\n",
			regs_req.regs.features, regs_req.regs.fpsr,
			(uint64_t)regs_req.regs.vregs[0].lo);
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	regs_req.regs.regs[19] = saved_x19;
	regs_req.regs.fpsr = saved_fpsr;
	regs_req.regs.vregs[0].lo = saved_v0_lo;
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

static int parse_syscall_trace_mode(const char *arg, uint32_t *mode_out)
{
	char *copy;
	char *token;
	char *saveptr = NULL;
	char *endp = NULL;
	uint32_t mode = 0;

	if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
		*mode_out = LKMDBG_SYSCALL_TRACE_MODE_OFF;
		return 0;
	}

	copy = strdup(arg);
	if (!copy)
		return -1;

	for (token = strtok_r(copy, ",+|", &saveptr); token;
	     token = strtok_r(NULL, ",+|", &saveptr)) {
		if (strcmp(token, "event") == 0) {
			mode |= LKMDBG_SYSCALL_TRACE_MODE_EVENT;
			continue;
		}
		if (strcmp(token, "stop") == 0) {
			mode |= LKMDBG_SYSCALL_TRACE_MODE_STOP;
			continue;
		}
		if (strcmp(token, "control") == 0) {
			mode |= LKMDBG_SYSCALL_TRACE_MODE_CONTROL;
			continue;
		}
		if (strcmp(token, "both") == 0) {
			mode |= LKMDBG_SYSCALL_TRACE_MODE_EVENT |
				LKMDBG_SYSCALL_TRACE_MODE_STOP;
			continue;
		}
		free(copy);
		goto numeric;
	}

	free(copy);
	*mode_out = mode;
	return 0;

numeric:
	*mode_out = (uint32_t)strtoul(arg, &endp, 0);
	if (*endp != '\0')
		return -1;
	return 0;
}

static int parse_syscall_trace_phases(const char *arg, uint32_t *phases_out)
{
	if (strcmp(arg, "enter") == 0) {
		*phases_out = LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
		return 0;
	}
	if (strcmp(arg, "exit") == 0) {
		*phases_out = LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
		return 0;
	}
	if (strcmp(arg, "both") == 0 || strcmp(arg, "enter+exit") == 0 ||
	    strcmp(arg, "exit+enter") == 0) {
		*phases_out = LKMDBG_SYSCALL_TRACE_PHASE_ENTER |
			      LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
		return 0;
	}
	return -1;
}

static int parse_syscall_resolve_action(const char *arg, uint32_t *action_out)
{
	if (strcmp(arg, "allow") == 0) {
		*action_out = LKMDBG_SYSCALL_RESOLVE_ACTION_ALLOW;
		return 0;
	}
	if (strcmp(arg, "skip") == 0) {
		*action_out = LKMDBG_SYSCALL_RESOLVE_ACTION_SKIP;
		return 0;
	}
	if (strcmp(arg, "rewrite") == 0) {
		*action_out = LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE;
		return 0;
	}
	return -1;
}

static int parse_u64_value(const char *arg, uint64_t *value_out)
{
	char *endp;

	*value_out = strtoull(arg, &endp, 0);
	if (*endp != '\0')
		return -1;
	return 0;
}

static int parse_s64_value(const char *arg, int64_t *value_out)
{
	char *endp;

	*value_out = strtoll(arg, &endp, 0);
	if (*endp != '\0')
		return -1;
	return 0;
}

static int parse_stealth_flags(const char *arg, uint32_t *flags_out)
{
	char *copy;
	char *token;
	char *saveptr = NULL;
	char *endp = NULL;
	uint32_t flags = 0;

	if (strcmp(arg, "none") == 0 || strcmp(arg, "0") == 0) {
		*flags_out = 0;
		return 0;
	}

	copy = strdup(arg);
	if (!copy)
		return -1;

	for (token = strtok_r(copy, ",|", &saveptr); token;
	     token = strtok_r(NULL, ",|", &saveptr)) {
		if (strcmp(token, "debugfs") == 0 ||
		    strcmp(token, "debugfs_on") == 0) {
			flags |= LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE;
			continue;
		}
		if (strcmp(token, "modulehide") == 0 ||
		    strcmp(token, "module_hidden") == 0 ||
		    strcmp(token, "hide") == 0) {
			flags |= LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN;
			continue;
		}
		if (strcmp(token, "sysfshide") == 0 ||
		    strcmp(token, "sysfs_hidden") == 0) {
			flags |= LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN;
			continue;
		}
		if (strcmp(token, "ownerprochide") == 0 ||
		    strcmp(token, "owner_proc_hidden") == 0 ||
		    strcmp(token, "ownerhide") == 0) {
			flags |= LKMDBG_STEALTH_FLAG_OWNER_PROC_HIDDEN;
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
	if (*endp != '\0')
		return -1;
	return 0;
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

static int verify_runtime_events(int session_fd, int cmd_fd, int resp_fd,
				 pid_t child, const struct child_info *info)
{
	struct lkmdbg_hwpoint_request bp_req;
	struct lkmdbg_hwpoint_request mmu_req;
	struct lkmdbg_hwpoint_request wp_req;
	struct lkmdbg_signal_config_request signal_cfg;
	struct lkmdbg_event_record event;
	struct lkmdbg_stop_query_request stop_req;
	uint64_t signal_mask_words[2];
	uint64_t signal_siginfo_code = 0;
	uint32_t signal_event_flags = 0;
	const int event_timeout_ms = 5000;
	int signal_config_active = 0;
	int ret;

	memset(&bp_req, 0, sizeof(bp_req));
	memset(&mmu_req, 0, sizeof(mmu_req));
	memset(&wp_req, 0, sizeof(wp_req));
	memset(&signal_cfg, 0, sizeof(signal_cfg));
	memset(&stop_req, 0, sizeof(stop_req));
	memset(signal_mask_words, 0, sizeof(signal_mask_words));
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

	signal_mask_words[(SIGUSR1 - 1) / 64] = 1ULL << ((SIGUSR1 - 1) % 64);
	if (set_signal_config(session_fd, signal_mask_words,
			      LKMDBG_SIGNAL_CONFIG_STOP, &signal_cfg) < 0)
		return -1;
	signal_config_active = 1;
	if (signal_cfg.mask_words[0] != signal_mask_words[0] ||
	    signal_cfg.mask_words[1] != signal_mask_words[1] ||
	    signal_cfg.flags != LKMDBG_SIGNAL_CONFIG_STOP) {
		fprintf(stderr,
			"signal config set mismatch mask=%" PRIx64 ":%" PRIx64 " flags=0x%x\n",
			(uint64_t)signal_cfg.mask_words[1],
			(uint64_t)signal_cfg.mask_words[0], signal_cfg.flags);
		goto signal_fail;
	}
	memset(&signal_cfg, 0, sizeof(signal_cfg));
	if (get_signal_config(session_fd, &signal_cfg) < 0)
		goto signal_fail;
	if (signal_cfg.mask_words[0] != signal_mask_words[0] ||
	    signal_cfg.mask_words[1] != signal_mask_words[1] ||
	    signal_cfg.flags != LKMDBG_SIGNAL_CONFIG_STOP) {
		fprintf(stderr,
			"signal config get mismatch mask=%" PRIx64 ":%" PRIx64 " flags=0x%x\n",
			(uint64_t)signal_cfg.mask_words[1],
			(uint64_t)signal_cfg.mask_words[0], signal_cfg.flags);
		goto signal_fail;
	}

	printf("selftest runtime: waiting for signal event\n");
	if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_SIGNAL) < 0)
		goto signal_fail;
	ret = wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_SIGNAL,
				     SIGUSR1, event_timeout_ms, &event);
	if (ret == -ETIMEDOUT) {
		printf("selftest runtime: signal lifecycle event unavailable, continuing\n");
		goto signal_clear;
	} else if (ret < 0) {
		goto signal_fail;
	} else if (event.tgid != child || event.tid != child) {
		fprintf(stderr,
			"signal event task mismatch tgid=%d tid=%d expected leader=%d\n",
			event.tgid, event.tid, child);
		goto signal_fail;
	} else if (event.flags & ~LKMDBG_SIGNAL_EVENT_GROUP) {
		fprintf(stderr, "signal event flags mismatch flags=0x%x\n",
			event.flags);
		goto signal_fail;
	} else if ((int64_t)event.value1 != 0) {
		fprintf(stderr, "signal event delivery result mismatch=%" PRId64 "\n",
			(int64_t)event.value1);
		goto signal_fail;
	}
	signal_event_flags = event.flags;
	signal_siginfo_code = event.value0;

	if (wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_STOP,
				   LKMDBG_STOP_REASON_SIGNAL,
				   event_timeout_ms, &event) < 0) {
		fprintf(stderr, "signal stop event missing\n");
		goto signal_fail;
	}
	if (event.tgid != child || event.tid != child ||
	    event.value0 != SIGUSR1 || event.value1 != signal_siginfo_code ||
	    event.flags != signal_event_flags) {
		if (event.value1 != signal_siginfo_code) {
			fprintf(stderr,
				"signal stop event mismatch tgid=%d tid=%d sig=0x%" PRIx64 " code=0x%" PRIx64 " flags=0x%x expected_flags=0x%x\n",
				event.tgid, event.tid, (uint64_t)event.value0,
				(uint64_t)event.value1, event.flags,
				signal_event_flags);
		} else {
			fprintf(stderr,
				"signal stop event mismatch tgid=%d tid=%d sig=0x%" PRIx64 " flags=0x%x expected_flags=0x%x\n",
				event.tgid, event.tid, (uint64_t)event.value0,
				event.flags, signal_event_flags);
		}
		goto signal_fail;
	}
	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_SIGNAL,
			      &stop_req) < 0)
		goto signal_fail;
	if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
	    stop_req.stop.tgid != child || stop_req.stop.tid != child ||
	    stop_req.stop.value0 != SIGUSR1 ||
	    stop_req.stop.value1 != signal_siginfo_code ||
	    stop_req.stop.event_flags != signal_event_flags) {
		fprintf(stderr,
			"signal stop state mismatch flags=0x%x tgid=%d tid=%d sig=0x%" PRIx64 " code=0x%" PRIx64 " event_flags=0x%x\n",
			stop_req.stop.flags, stop_req.stop.tgid, stop_req.stop.tid,
			(uint64_t)stop_req.stop.value0,
			(uint64_t)stop_req.stop.value1,
			stop_req.stop.event_flags);
		goto signal_fail;
	}
	if (continue_target_and_wait_for_user_loop(session_fd,
						   stop_req.stop.cookie, cmd_fd,
						   resp_fd) < 0)
		goto signal_fail;

signal_clear:
	memset(signal_mask_words, 0, sizeof(signal_mask_words));
	memset(&signal_cfg, 0, sizeof(signal_cfg));
	if (signal_config_active) {
		if (set_signal_config(session_fd, signal_mask_words, 0,
				      &signal_cfg) < 0)
			return -1;
		signal_config_active = 0;
	}

	memset(&signal_cfg, 0, sizeof(signal_cfg));
	if (get_signal_config(session_fd, &signal_cfg) < 0)
		return -1;
	if (signal_cfg.mask_words[0] || signal_cfg.mask_words[1] ||
	    signal_cfg.flags) {
		fprintf(stderr,
			"signal config clear mismatch mask=%" PRIx64 ":%" PRIx64 " flags=0x%x\n",
			(uint64_t)signal_cfg.mask_words[1],
			(uint64_t)signal_cfg.mask_words[0], signal_cfg.flags);
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
	if (continue_target_and_wait_for_user_loop(session_fd,
						   stop_req.stop.cookie, cmd_fd,
						   resp_fd) < 0) {
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
	if (wait_for_stop_event_or_state(session_fd,
					 LKMDBG_STOP_REASON_BREAKPOINT,
					 event_timeout_ms, &event,
					 &stop_req) < 0) {
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
	if (continue_target_and_wait_for_user_loop(session_fd,
						   stop_req.stop.cookie, cmd_fd,
						   resp_fd) < 0) {
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
	if (wait_for_stop_event_or_state(session_fd,
					 LKMDBG_STOP_REASON_BREAKPOINT,
					 event_timeout_ms, &event,
					 &stop_req) < 0) {
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
	if (continue_target_and_wait_for_user_loop(session_fd,
						   stop_req.stop.cookie, cmd_fd,
						   resp_fd) < 0) {
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
		if (continue_target_and_wait_for_user_loop(session_fd,
							   stop_req.stop.cookie,
							   cmd_fd, resp_fd) < 0) {
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
		if (continue_target_and_wait_for_user_loop(session_fd,
							   stop_req.stop.cookie,
							   cmd_fd, resp_fd) < 0) {
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
	} else if (child_ping(cmd_fd, resp_fd) < 0) {
		remove_hwpoint(session_fd, wp_req.id);
		return -1;
	}
	if (remove_hwpoint(session_fd, wp_req.id) < 0)
		return -1;

	printf("selftest runtime events ok clone signal breakpoint mmu-breakpoint oneshot rearm watchpoint threshold actions\n");
	return 0;

signal_fail:
	if (signal_config_active) {
		memset(signal_mask_words, 0, sizeof(signal_mask_words));
		set_signal_config(session_fd, signal_mask_words, 0, NULL);
	}
	return -1;
}

static int verify_syscall_trace(int session_fd, int cmd_fd, int resp_fd,
				pid_t child)
{
	struct lkmdbg_syscall_trace_request trace_req;
	struct lkmdbg_syscall_resolve_request resolve_req;
	struct lkmdbg_syscall_rule_config_request rule_cfg;
	struct lkmdbg_syscall_rule_request rule_req;
	struct lkmdbg_syscall_rule_entry rule_entry;
	struct lkmdbg_event_record event;
	struct lkmdbg_stop_query_request stop_req;
	const uint32_t stop_phases[] = {
		LKMDBG_SYSCALL_TRACE_PHASE_ENTER,
		LKMDBG_SYSCALL_TRACE_PHASE_EXIT,
	};
	const uint32_t nr = SYS_getppid;
	const uint32_t nr_rewrite = SYS_getpgid;
	const uint32_t nr_rewrite_to = SYS_getpid;
	const pid_t parent_pid = getpid();
	const int event_timeout_ms = 5000;
	const int64_t exit_rewrite_retval = -7777;
	const int64_t exit_rewrite_retval_v3 = -8889;
	int64_t syscall_retval;
	uint32_t supported_phases;
	uint32_t control_phases;
	size_t stop_idx;
	int trace_active = 0;

	memset(&trace_req, 0, sizeof(trace_req));
	memset(&resolve_req, 0, sizeof(resolve_req));
	memset(&rule_cfg, 0, sizeof(rule_cfg));
	memset(&rule_req, 0, sizeof(rule_req));
	memset(&rule_entry, 0, sizeof(rule_entry));
	memset(&stop_req, 0, sizeof(stop_req));

	if (drain_session_events(session_fd) < 0)
		return -1;

	if (get_syscall_trace(session_fd, &trace_req) < 0)
		return -1;
	supported_phases = trace_req.supported_phases;
	if (!!supported_phases !=
	    !!(trace_req.flags &
	       (LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_TRACEPOINT |
		LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_ENTRY_HOOK))) {
		fprintf(stderr,
			"syscall trace backend flag mismatch flags=0x%x supported=0x%x\n",
			trace_req.flags, supported_phases);
		return -1;
	}
	if ((trace_req.flags &
	     (LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_TRACEPOINT |
	      LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_ENTRY_HOOK)) &&
	    !supported_phases) {
		fprintf(stderr,
			"syscall trace capability mismatch flags=0x%x supported=0x%x\n",
			trace_req.flags, supported_phases);
		return -1;
	}
	if (supported_phases &
	    ~(LKMDBG_SYSCALL_TRACE_PHASE_ENTER |
	      LKMDBG_SYSCALL_TRACE_PHASE_EXIT)) {
		fprintf(stderr,
			"syscall trace supported phases invalid: 0x%x\n",
			supported_phases);
		return -1;
	}
	if (!supported_phases) {
		if (expect_set_syscall_trace_errno(
			    session_fd, 0, (int)nr,
			    LKMDBG_SYSCALL_TRACE_MODE_EVENT,
			    LKMDBG_SYSCALL_TRACE_PHASE_ENTER, EOPNOTSUPP) < 0)
			return -1;
		printf("selftest syscall trace unavailable, skipping\n");
		return 0;
	}
	control_phases = LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
	if (supported_phases & LKMDBG_SYSCALL_TRACE_PHASE_EXIT)
		control_phases |= LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
	if (((LKMDBG_SYSCALL_TRACE_PHASE_ENTER |
	      LKMDBG_SYSCALL_TRACE_PHASE_EXIT) &
	     ~supported_phases) != 0) {
		if (expect_set_syscall_trace_errno(
			    session_fd, 0, (int)nr,
			    LKMDBG_SYSCALL_TRACE_MODE_EVENT,
			    (LKMDBG_SYSCALL_TRACE_PHASE_ENTER |
			     LKMDBG_SYSCALL_TRACE_PHASE_EXIT) &
				    ~supported_phases,
			    EOPNOTSUPP) < 0)
			return -1;
	}

	if (set_syscall_trace(session_fd, 0, (int)nr,
			      LKMDBG_SYSCALL_TRACE_MODE_EVENT,
			      supported_phases,
			      &trace_req) < 0)
		return -1;
	trace_active = 1;
	if (trace_req.tid != 0 || trace_req.syscall_nr != (int)nr ||
	    trace_req.mode != LKMDBG_SYSCALL_TRACE_MODE_EVENT ||
	    trace_req.phases != supported_phases ||
	    trace_req.supported_phases != supported_phases) {
		fprintf(stderr,
			"syscall trace set mismatch tid=%d nr=%d mode=0x%x phases=0x%x supported=0x%x\n",
			trace_req.tid, trace_req.syscall_nr, trace_req.mode,
			trace_req.phases, trace_req.supported_phases);
		goto fail;
	}

	memset(&trace_req, 0, sizeof(trace_req));
	if (get_syscall_trace(session_fd, &trace_req) < 0)
		goto fail;
	if (trace_req.tid != 0 || trace_req.syscall_nr != (int)nr ||
	    trace_req.mode != LKMDBG_SYSCALL_TRACE_MODE_EVENT ||
	    trace_req.phases != supported_phases ||
	    trace_req.supported_phases != supported_phases) {
		fprintf(stderr,
			"syscall trace get mismatch tid=%d nr=%d mode=0x%x phases=0x%x supported=0x%x\n",
			trace_req.tid, trace_req.syscall_nr, trace_req.mode,
			trace_req.phases, trace_req.supported_phases);
		goto fail;
	}

	if (child_ping(cmd_fd, resp_fd) < 0)
		goto fail;
	if (child_trigger_syscall(cmd_fd, resp_fd, &syscall_retval) < 0)
		goto fail;
	if (syscall_retval != parent_pid) {
		fprintf(stderr,
			"child syscall retval mismatch got=%" PRId64 " expected=%d\n",
			syscall_retval, parent_pid);
		goto fail;
	}

	if (supported_phases & LKMDBG_SYSCALL_TRACE_PHASE_ENTER) {
		if (wait_for_syscall_event(session_fd,
					   LKMDBG_SYSCALL_TRACE_PHASE_ENTER, nr,
					   event_timeout_ms, &event) < 0)
			goto fail;
		if (event.tgid != child || event.tid != child ||
		    event.value1 != 0) {
			fprintf(stderr,
				"syscall enter event mismatch tgid=%d tid=%d retval=%" PRId64 "\n",
				event.tgid, event.tid, (int64_t)event.value1);
			goto fail;
		}
	}
	if (supported_phases & LKMDBG_SYSCALL_TRACE_PHASE_EXIT) {
		if (wait_for_syscall_event(session_fd,
					   LKMDBG_SYSCALL_TRACE_PHASE_EXIT, nr,
					   event_timeout_ms, &event) < 0)
			goto fail;
		if (event.tgid != child || event.tid != child ||
		    (pid_t)(int64_t)event.value1 != parent_pid) {
			fprintf(stderr,
				"syscall exit event mismatch tgid=%d tid=%d retval=%" PRId64 " expected=%d\n",
				event.tgid, event.tid, (int64_t)event.value1,
				parent_pid);
			goto fail;
		}
	}

	for (stop_idx = 0;
	     stop_idx < sizeof(stop_phases) / sizeof(stop_phases[0]);
	     stop_idx++) {
		const uint32_t stop_phase = stop_phases[stop_idx];
		const uint64_t expected_retval =
			stop_phase == LKMDBG_SYSCALL_TRACE_PHASE_ENTER ? 0 :
							       (uint64_t)parent_pid;

		if (!(supported_phases & stop_phase))
			continue;

		if (set_syscall_trace(session_fd, child, (int)nr,
				      LKMDBG_SYSCALL_TRACE_MODE_STOP,
				      stop_phase,
				      &trace_req) < 0)
			goto fail;
		if (trace_req.tid != child || trace_req.syscall_nr != (int)nr ||
		    trace_req.mode != LKMDBG_SYSCALL_TRACE_MODE_STOP ||
		    trace_req.phases != stop_phase ||
		    trace_req.supported_phases != supported_phases) {
			fprintf(stderr,
				"syscall stop config mismatch tid=%d nr=%d mode=0x%x phases=0x%x supported=0x%x\n",
				trace_req.tid, trace_req.syscall_nr, trace_req.mode,
				trace_req.phases, trace_req.supported_phases);
			goto fail;
		}

		if (drain_session_events(session_fd) < 0)
			goto fail;
		if (child_ping(cmd_fd, resp_fd) < 0)
			goto fail;
		if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_SYSCALL) < 0)
			goto fail;
		if (wait_for_stop_event_or_state(session_fd,
						 LKMDBG_STOP_REASON_SYSCALL,
						 event_timeout_ms, &event,
						 &stop_req) < 0) {
			fprintf(stderr,
				"syscall stop event/state missing phase=0x%x\n",
				stop_phase);
			goto fail;
		}
		if (event.tgid != child || event.tid != child ||
		    event.flags != stop_phase || event.value0 != nr ||
		    event.value1 != expected_retval) {
			fprintf(stderr,
				"syscall stop event mismatch tgid=%d tid=%d flags=0x%x nr=%" PRIu64 " retval=%" PRId64 "\n",
				event.tgid, event.tid, event.flags,
				(uint64_t)event.value0, (int64_t)event.value1);
			goto fail;
		}
		if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
		    !(stop_req.stop.flags & LKMDBG_STOP_FLAG_REGS_VALID) ||
		    stop_req.stop.tgid != child || stop_req.stop.tid != child ||
		    stop_req.stop.event_flags != stop_phase ||
		    stop_req.stop.value0 != nr ||
		    stop_req.stop.value1 != expected_retval) {
			fprintf(stderr,
				"syscall stop state mismatch flags=0x%x tgid=%d tid=%d event_flags=0x%x nr=%" PRIu64 " retval=%" PRId64 "\n",
				stop_req.stop.flags, stop_req.stop.tgid,
				stop_req.stop.tid, stop_req.stop.event_flags,
				(uint64_t)stop_req.stop.value0,
				(int64_t)stop_req.stop.value1);
			goto fail;
		}
		if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
				    NULL) < 0)
			goto fail;
		if (child_read_syscall_result(resp_fd, &syscall_retval) < 0)
			goto fail;
		if (syscall_retval != parent_pid) {
			fprintf(stderr,
				"syscall stop completion mismatch got=%" PRId64 " expected=%d\n",
				syscall_retval, parent_pid);
			goto fail;
		}
	}

	if (trace_req.flags & LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_CONTROL) {
		if (set_syscall_trace(session_fd, child, (int)nr,
				      LKMDBG_SYSCALL_TRACE_MODE_CONTROL,
				      LKMDBG_SYSCALL_TRACE_PHASE_ENTER,
				      &trace_req) < 0)
			goto fail;

		if (drain_session_events(session_fd) < 0)
			goto fail;
		if (child_ping(cmd_fd, resp_fd) < 0)
			goto fail;
		if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_SYSCALL) < 0)
			goto fail;
		if (wait_for_stop_event_or_state(session_fd,
						 LKMDBG_STOP_REASON_SYSCALL,
						 event_timeout_ms, &event,
						 &stop_req) < 0)
			goto fail;
		if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_SYSCALL_CONTROL) ||
		    stop_req.stop.event_flags !=
			    LKMDBG_SYSCALL_TRACE_PHASE_ENTER ||
		    stop_req.stop.value0 != nr) {
			fprintf(stderr,
				"syscall control stop mismatch flags=0x%x event_flags=0x%x nr=%" PRIu64 "\n",
				stop_req.stop.flags, stop_req.stop.event_flags,
				(uint64_t)stop_req.stop.value0);
			goto fail;
		}
		if (resolve_syscall(session_fd, stop_req.stop.cookie,
				    LKMDBG_SYSCALL_RESOLVE_ACTION_SKIP, -1,
				    NULL, -1234, &resolve_req) < 0)
			goto fail;
		if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
				    NULL) < 0)
			goto fail;
		if (child_read_syscall_result(resp_fd, &syscall_retval) < 0)
			goto fail;
		if (syscall_retval != -1234) {
			fprintf(stderr,
				"syscall control skip retval mismatch got=%" PRId64 "\n",
				syscall_retval);
			goto fail;
		}

		if (resolve_req.backend_flags &
		    LKMDBG_SYSCALL_RESOLVE_FLAG_NR_REWRITE_SUPPORTED) {
			if (set_syscall_trace(session_fd, child, (int)nr,
					      LKMDBG_SYSCALL_TRACE_MODE_CONTROL,
					      LKMDBG_SYSCALL_TRACE_PHASE_ENTER,
					      &trace_req) < 0)
				goto fail;
			if (drain_session_events(session_fd) < 0)
				goto fail;
			if (child_ping(cmd_fd, resp_fd) < 0)
				goto fail;
			if (send_child_command(cmd_fd, CHILD_OP_TRIGGER_SYSCALL) <
			    0)
				goto fail;
			if (wait_for_stop_event_or_state(
				    session_fd, LKMDBG_STOP_REASON_SYSCALL,
				    event_timeout_ms, NULL, &stop_req) < 0)
				goto fail;
			if (resolve_syscall(
				    session_fd, stop_req.stop.cookie,
				    LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE,
				    SYS_getpid, NULL, 0, &resolve_req) < 0)
				goto fail;
			if (continue_target(session_fd, stop_req.stop.cookie, 2000,
					    0, NULL) < 0)
				goto fail;
			if (child_read_syscall_result(resp_fd, &syscall_retval) <
			    0)
				goto fail;
			if ((pid_t)syscall_retval != child) {
				fprintf(stderr,
					"syscall control rewrite retval mismatch got=%" PRId64 " expected=%d\n",
					syscall_retval, child);
				goto fail;
			}
		}

		if (set_syscall_trace(session_fd, child, -1,
				      LKMDBG_SYSCALL_TRACE_MODE_CONTROL,
				      control_phases, &trace_req) < 0)
			goto fail;
		if (set_syscall_rule_config(
			    session_fd, LKMDBG_SYSCALL_RULE_MODE_ENFORCE,
			    LKMDBG_SYSCALL_RULE_EVENT_RAW_AND_RULE,
			    &rule_cfg) < 0)
			goto fail;

		if (control_phases & LKMDBG_SYSCALL_TRACE_PHASE_EXIT) {
			memset(&rule_entry, 0, sizeof(rule_entry));
			rule_entry.tid = child;
			rule_entry.syscall_nr = (int32_t)nr;
			rule_entry.phases = LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
			rule_entry.actions =
				LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN;
			rule_entry.flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
			rule_entry.priority = 100;
			rule_entry.retval = exit_rewrite_retval;
			memset(&rule_req, 0, sizeof(rule_req));
			if (upsert_syscall_rule(session_fd, &rule_entry,
						&rule_req) < 0)
				goto fail;

			if (drain_session_events(session_fd) < 0)
				goto fail;
			if (child_trigger_syscall(cmd_fd, resp_fd,
						  &syscall_retval) < 0)
				goto fail;
			if (syscall_retval != exit_rewrite_retval) {
				fprintf(stderr,
					"syscall rule exit setret mismatch got=%" PRId64 " expected=%" PRId64 "\n",
					syscall_retval, exit_rewrite_retval);
				goto fail;
			}
			if (wait_for_session_event(
				    session_fd, LKMDBG_EVENT_TARGET_SYSCALL_RULE,
				    0, event_timeout_ms, &event) < 0)
				goto fail;
			if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_EXIT ||
			    event.value0 != nr || event.value1 != rule_req.rule.rule_id) {
				fprintf(stderr,
					"syscall rule exit event mismatch flags=0x%x nr=%" PRIu64 " rule=%" PRIu64 "\n",
					event.flags, (uint64_t)event.value0,
					(uint64_t)event.value1);
				goto fail;
			}
			if (wait_for_session_event(
				    session_fd,
				    LKMDBG_EVENT_TARGET_SYSCALL_RULE_DETAIL, 0,
				    event_timeout_ms, &event) < 0)
				goto fail;
			if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_EXIT ||
			    (event.code &
			     LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN) == 0 ||
			    (int64_t)event.value0 != parent_pid ||
			    (int64_t)event.value1 != exit_rewrite_retval) {
				fprintf(stderr,
					"syscall rule exit detail mismatch flags=0x%x code=0x%x old=%" PRId64 " new=%" PRId64 "\n",
					event.flags, event.code,
					(int64_t)event.value0,
					(int64_t)event.value1);
				goto fail;
			}
		}

		memset(&rule_entry, 0, sizeof(rule_entry));
		rule_entry.tid = child;
		rule_entry.syscall_nr = (int32_t)nr_rewrite;
		rule_entry.phases = LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
		rule_entry.actions = LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS;
		rule_entry.flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
		rule_entry.priority = 200;
		rule_entry.arg_match_mask = 0x1;
		rule_entry.arg_values[0] = UINT32_MAX;
		rule_entry.arg_value_masks[0] = UINT32_MAX;
		rule_entry.rewrite_mask = 0x1;
		rule_entry.rewrite_args[0] = 0;
		memset(&rule_req, 0, sizeof(rule_req));
		if (upsert_syscall_rule(session_fd, &rule_entry, &rule_req) < 0)
			goto fail;

		if (drain_session_events(session_fd) < 0)
			goto fail;
		if (child_trigger_syscall_ex(cmd_fd, resp_fd, nr_rewrite,
					     UINT64_MAX, 1,
					     &syscall_retval) < 0)
			goto fail;
		if (wait_for_session_event(session_fd,
					   LKMDBG_EVENT_TARGET_SYSCALL_RULE, 0,
					   event_timeout_ms, &event) < 0)
			goto fail;
		if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_ENTER ||
		    event.value0 != nr_rewrite ||
		    event.value1 != rule_req.rule.rule_id) {
			fprintf(stderr,
				"syscall rule rewrite event mismatch flags=0x%x nr=%" PRIu64 " rule=%" PRIu64 "\n",
				event.flags, (uint64_t)event.value0,
				(uint64_t)event.value1);
			goto fail;
		}
		if (wait_for_session_event(
			    session_fd, LKMDBG_EVENT_TARGET_SYSCALL_RULE_DETAIL, 0,
			    event_timeout_ms, &event) < 0)
			goto fail;
		if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_ENTER ||
		    (event.code & LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS) == 0 ||
		    event.value0 != nr_rewrite || event.value1 != nr_rewrite) {
			fprintf(stderr,
				"syscall rule rewrite detail mismatch flags=0x%x code=0x%x old=%" PRIu64 " new=%" PRIu64 "\n",
				event.flags, event.code, (uint64_t)event.value0,
				(uint64_t)event.value1);
			goto fail;
		}

		if (resolve_req.backend_flags &
		    LKMDBG_SYSCALL_RESOLVE_FLAG_NR_REWRITE_SUPPORTED) {
			memset(&rule_entry, 0, sizeof(rule_entry));
			rule_entry.tid = child;
			rule_entry.syscall_nr = (int32_t)nr;
			rule_entry.phases = LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
			rule_entry.actions = LKMDBG_SYSCALL_RULE_ACTION_REWRITE_NR;
			rule_entry.flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
			rule_entry.priority = 250;
			rule_entry.rewrite_syscall_nr = (int32_t)nr_rewrite_to;
			memset(&rule_req, 0, sizeof(rule_req));
			if (upsert_syscall_rule(session_fd, &rule_entry,
						&rule_req) < 0)
				goto fail;

			if (drain_session_events(session_fd) < 0)
				goto fail;
			if (child_trigger_syscall(cmd_fd, resp_fd,
						  &syscall_retval) < 0)
				goto fail;
			if ((pid_t)syscall_retval != child) {
				fprintf(stderr,
					"syscall rule rewrite-nr mismatch got=%" PRId64 " expected=%d\n",
					syscall_retval, child);
				goto fail;
			}
			if (wait_for_session_event(
				    session_fd, LKMDBG_EVENT_TARGET_SYSCALL_RULE,
				    0, event_timeout_ms, &event) < 0)
				goto fail;
			if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_ENTER ||
			    (event.code &
			     LKMDBG_SYSCALL_RULE_ACTION_REWRITE_NR) == 0 ||
			    event.value0 != nr_rewrite_to ||
			    event.value1 != rule_req.rule.rule_id) {
				fprintf(stderr,
					"syscall rule rewrite-nr event mismatch flags=0x%x code=0x%x nr=%" PRIu64 " rule=%" PRIu64 "\n",
					event.flags, event.code,
					(uint64_t)event.value0,
					(uint64_t)event.value1);
				goto fail;
			}
			if (wait_for_session_event(
				    session_fd,
				    LKMDBG_EVENT_TARGET_SYSCALL_RULE_DETAIL, 0,
				    event_timeout_ms, &event) < 0)
				goto fail;
			if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_ENTER ||
			    (event.code &
			     LKMDBG_SYSCALL_RULE_ACTION_REWRITE_NR) == 0 ||
			    event.value0 != nr ||
			    event.value1 != nr_rewrite_to) {
				fprintf(stderr,
					"syscall rule rewrite-nr detail mismatch flags=0x%x code=0x%x old=%" PRIu64 " new=%" PRIu64 "\n",
					event.flags, event.code,
					(uint64_t)event.value0,
					(uint64_t)event.value1);
				goto fail;
			}
		}

		if (control_phases & LKMDBG_SYSCALL_TRACE_PHASE_EXIT) {
			memset(&rule_entry, 0, sizeof(rule_entry));
			rule_entry.tid = child;
			rule_entry.syscall_nr = (int32_t)nr_rewrite_to;
			rule_entry.phases = LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
			rule_entry.actions =
				LKMDBG_SYSCALL_RULE_ACTION_REWRITE_RETVAL;
			rule_entry.flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
			rule_entry.priority = 260;
			rule_entry.retval = exit_rewrite_retval_v3;
			memset(&rule_req, 0, sizeof(rule_req));
			if (upsert_syscall_rule(session_fd, &rule_entry,
						&rule_req) < 0)
				goto fail;

			if (drain_session_events(session_fd) < 0)
				goto fail;
			if (child_trigger_syscall_ex(cmd_fd, resp_fd, nr_rewrite_to,
						     0, 0,
						     &syscall_retval) < 0)
				goto fail;
			if (syscall_retval != exit_rewrite_retval_v3) {
				fprintf(stderr,
					"syscall rule rewrite-retval mismatch got=%" PRId64 " expected=%" PRId64 "\n",
					syscall_retval,
					exit_rewrite_retval_v3);
				goto fail;
			}
			if (wait_for_session_event(
				    session_fd, LKMDBG_EVENT_TARGET_SYSCALL_RULE,
				    0, event_timeout_ms, &event) < 0)
				goto fail;
			if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_EXIT ||
			    (event.code &
			     LKMDBG_SYSCALL_RULE_ACTION_REWRITE_RETVAL) == 0 ||
			    event.value0 != nr_rewrite_to ||
			    event.value1 != rule_req.rule.rule_id) {
				fprintf(stderr,
					"syscall rule rewrite-retval event mismatch flags=0x%x code=0x%x nr=%" PRIu64 " rule=%" PRIu64 "\n",
					event.flags, event.code,
					(uint64_t)event.value0,
					(uint64_t)event.value1);
				goto fail;
			}
			if (wait_for_session_event(
				    session_fd,
				    LKMDBG_EVENT_TARGET_SYSCALL_RULE_DETAIL, 0,
				    event_timeout_ms, &event) < 0)
				goto fail;
			if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_EXIT ||
			    (event.code &
			     LKMDBG_SYSCALL_RULE_ACTION_REWRITE_RETVAL) == 0 ||
			    (pid_t)(int64_t)event.value0 != child ||
			    (int64_t)event.value1 != exit_rewrite_retval_v3) {
				fprintf(stderr,
					"syscall rule rewrite-retval detail mismatch flags=0x%x code=0x%x old=%" PRId64 " new=%" PRId64 "\n",
					event.flags, event.code,
					(int64_t)event.value0,
					(int64_t)event.value1);
				goto fail;
			}
		}
	}

	memset(&trace_req, 0, sizeof(trace_req));
	if (set_syscall_trace(session_fd, 0, -1, LKMDBG_SYSCALL_TRACE_MODE_OFF, 0,
			      &trace_req) < 0)
		return -1;
	trace_active = 0;
	memset(&trace_req, 0, sizeof(trace_req));
	if (get_syscall_trace(session_fd, &trace_req) < 0)
		return -1;
	if (trace_req.tid || trace_req.syscall_nr != -1 || trace_req.mode ||
	    trace_req.phases) {
		fprintf(stderr,
			"syscall trace clear mismatch tid=%d nr=%d mode=0x%x phases=0x%x\n",
			trace_req.tid, trace_req.syscall_nr, trace_req.mode,
			trace_req.phases);
		return -1;
	}

	printf("selftest syscall trace ok nr=%u parent=%d supported=0x%x\n", nr,
	       parent_pid, supported_phases);
	return 0;

fail:
	if (trace_active)
		set_syscall_trace(session_fd, 0, -1, LKMDBG_SYSCALL_TRACE_MODE_OFF,
				  0, NULL);
	return -1;
}

static int verify_stealth_control(int session_fd)
{
	struct lkmdbg_stealth_request stealth_req;
	struct lkmdbg_status_reply status_reply;
	uint32_t original_flags;
	int present;
	int aux_session_fd = -1;

	memset(&stealth_req, 0, sizeof(stealth_req));
	memset(&status_reply, 0, sizeof(status_reply));

	if (get_stealth(session_fd, &stealth_req) < 0)
		return -1;
	if (get_status(session_fd, &status_reply) < 0)
		return -1;
	if (status_reply.stealth_flags != stealth_req.flags ||
	    status_reply.stealth_supported_flags != stealth_req.supported_flags) {
		fprintf(stderr,
			"stealth status mismatch flags=0x%x/0x%x supported=0x%x/0x%x\n",
			status_reply.stealth_flags, stealth_req.flags,
			status_reply.stealth_supported_flags,
			stealth_req.supported_flags);
		return -1;
	}

	if (!(stealth_req.supported_flags & LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN)) {
		printf("selftest stealth: module list hide unavailable, skipping\n");
		return 0;
	}

	original_flags = stealth_req.flags;
	present = proc_modules_has_name("lkmdbg ");
	if (present < 0)
		return -1;
	if (!present) {
		fprintf(stderr, "module missing from /proc/modules before hide\n");
		return -1;
	}

	if (set_stealth(session_fd,
			original_flags | LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN,
			&stealth_req) < 0)
		return -1;
	if (!(stealth_req.flags & LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN)) {
		fprintf(stderr, "module list hide did not stick flags=0x%x\n",
			stealth_req.flags);
		goto fail;
	}

	present = proc_modules_has_name("lkmdbg ");
	if (present < 0)
		goto fail;
	if (present) {
		fprintf(stderr, "module still visible in /proc/modules after hide\n");
		goto fail;
	}

	aux_session_fd = open_session_fd();
	if (aux_session_fd < 0)
		goto fail;
	close(aux_session_fd);
	aux_session_fd = -1;

	if (set_stealth(session_fd, original_flags, &stealth_req) < 0)
		return -1;
	if (stealth_req.flags != original_flags) {
		fprintf(stderr, "stealth restore mismatch flags=0x%x expected=0x%x\n",
			stealth_req.flags, original_flags);
		return -1;
	}

	present = proc_modules_has_name("lkmdbg ");
	if (present < 0)
		return -1;
	if (!present) {
		fprintf(stderr, "module not restored in /proc/modules\n");
		return -1;
	}

	printf("selftest stealth control ok original=0x%x restored=0x%x\n",
	       original_flags, stealth_req.flags);
	return 0;

fail:
	if (aux_session_fd >= 0)
		close(aux_session_fd);
	set_stealth(session_fd, original_flags, NULL);
	return -1;
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

static uint64_t remote_call_expected_value(const uint64_t *args)
{
	return (args[0] ^ (args[1] << 1)) + (args[2] * 3U) +
	       (args[3] ^ 0x13579BDF2468ACE0ULL);
}

static int verify_remote_call(int session_fd, const struct child_info *info)
{
	struct lkmdbg_freeze_request freeze_req;
	struct lkmdbg_remote_call_request call_req;
	struct lkmdbg_remote_call_request x8_call_req;
	struct lkmdbg_thread_entry parked_entry;
	struct lkmdbg_thread_regs_request regs_req;
	struct lkmdbg_stop_query_request stop_req;
	struct lkmdbg_event_record event;
	pid_t tids[SELFTEST_FREEZE_THREADS];
	uint64_t args[4] = {
		0x0123456789abcdefULL,
		0x0fedcba987654321ULL,
		0x1111111111111111ULL,
		0x2222222222222222ULL,
	};
	uint64_t expected = remote_call_expected_value(args);
	uint64_t x8_marker = 0x5a5aa5a5c3c33c3cULL;
	uint64_t x8_value = 0x1122334455667788ULL;
	uint64_t x8_expected = x8_value ^ x8_marker;
	unsigned int i;
	unsigned int parked_index = UINT32_MAX;

	memset(&freeze_req, 0, sizeof(freeze_req));
	memset(&call_req, 0, sizeof(call_req));
	memset(&x8_call_req, 0, sizeof(x8_call_req));
	memset(&parked_entry, 0, sizeof(parked_entry));
	memset(&regs_req, 0, sizeof(regs_req));
	memset(&stop_req, 0, sizeof(stop_req));
	memset(tids, 0, sizeof(tids));

	if (wait_for_freeze_thread_tids(session_fd, info, tids) < 0)
		return -1;
	if (freeze_target_threads(session_fd, 2000, &freeze_req, 0) < 0)
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
		fprintf(stderr, "no parked thread available for remote call\n");
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (drain_session_events(session_fd) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (remote_call_thread(session_fd, tids[parked_index],
			       info->remote_call_addr, args, 4, &call_req) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0)
		return -1;

	if (wait_for_stop_event_or_state(session_fd,
					 LKMDBG_STOP_REASON_REMOTE_CALL,
					 5000, &event, &stop_req) < 0)
		return -1;

	if (event.tid != tids[parked_index]) {
		fprintf(stderr, "remote call event tid mismatch got=%d expected=%d\n",
			event.tid, tids[parked_index]);
		return -1;
	}
	if (stop_req.stop.value0 != expected) {
		fprintf(stderr,
			"remote call return mismatch got=0x%" PRIx64 " expected=0x%" PRIx64 "\n",
			(uint64_t)stop_req.stop.value0, (uint64_t)expected);
		return -1;
	}
	if (stop_req.stop.value1 != call_req.call_id) {
		fprintf(stderr,
			"remote call id mismatch stop=0x%" PRIx64 " req=0x%" PRIx64 "\n",
			(uint64_t)stop_req.stop.value1,
			(uint64_t)call_req.call_id);
		return -1;
	}
	if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
	    !(stop_req.stop.flags & LKMDBG_STOP_FLAG_REGS_VALID)) {
		fprintf(stderr, "remote call stop flags mismatch flags=0x%x\n",
			stop_req.stop.flags);
		return -1;
	}
	if (stop_req.stop.regs.pc != call_req.return_pc) {
		fprintf(stderr,
			"remote call stop pc mismatch got=0x%" PRIx64 " expected=0x%" PRIx64 "\n",
			(uint64_t)stop_req.stop.regs.pc,
			(uint64_t)call_req.return_pc);
		return -1;
	}

	if (find_target_thread_entry(session_fd, tids[parked_index], &parked_entry) <
	    0)
		return -1;
	if (!(parked_entry.flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED)) {
		fprintf(stderr, "remote call parked thread no longer reported parked\n");
		return -1;
	}

	if (get_target_regs(session_fd, tids[parked_index], &regs_req) < 0)
		return -1;
	if (regs_req.regs.pc != call_req.return_pc) {
		fprintf(stderr,
			"remote call parked regs pc mismatch got=0x%" PRIx64 " expected=0x%" PRIx64 "\n",
			(uint64_t)regs_req.regs.pc, (uint64_t)call_req.return_pc);
		return -1;
	}

	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0)
		return -1;

	memset(&freeze_req, 0, sizeof(freeze_req));
	memset(&stop_req, 0, sizeof(stop_req));
	memset(&parked_entry, 0, sizeof(parked_entry));
	parked_index = UINT32_MAX;

	if (freeze_target_threads(session_fd, 2000, &freeze_req, 0) < 0)
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
		fprintf(stderr,
			"no parked thread available for second remote call\n");
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (drain_session_events(session_fd) < 0) {
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (remote_call_thread_ex(session_fd, tids[parked_index],
				  info->remote_call_x8_addr, &x8_marker, 1,
				  LKMDBG_REMOTE_CALL_FLAG_SET_X8, 0, 0,
				  x8_value, &x8_call_req) < 0)
		return -1;

	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0)
		return -1;

	if (wait_for_stop_event_or_state(session_fd,
					 LKMDBG_STOP_REASON_REMOTE_CALL,
					 5000, &event, &stop_req) < 0)
		return -1;
	if (stop_req.stop.value0 != x8_expected) {
		fprintf(stderr,
			"remote call x8 return mismatch got=0x%" PRIx64 " expected=0x%" PRIx64 "\n",
			(uint64_t)stop_req.stop.value0, (uint64_t)x8_expected);
		return -1;
	}
	if (stop_req.stop.value1 != x8_call_req.call_id) {
		fprintf(stderr,
			"remote call x8 id mismatch stop=0x%" PRIx64 " req=0x%" PRIx64 "\n",
			(uint64_t)stop_req.stop.value1,
			(uint64_t)x8_call_req.call_id);
		return -1;
	}

	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0)
		return -1;

	printf("selftest remote call ok tid=%d call_id=%" PRIu64 " retval=0x%" PRIx64 " x8=0x%" PRIx64 "\n",
	       tids[parked_index], (uint64_t)call_req.call_id,
	       (uint64_t)expected, (uint64_t)x8_expected);
	return 0;
}

static int verify_remote_thread_create(int session_fd,
				       const struct child_info *info)
{
	struct lkmdbg_freeze_request freeze_req;
	struct lkmdbg_remote_thread_create_request create_req;
	struct lkmdbg_thread_entry parked_entry;
	struct lkmdbg_stop_query_request stop_req;
	pid_t tids[SELFTEST_FREEZE_THREADS];
	uint64_t zero = 0;
	uint64_t arg_value = 0xabcdef0012345678ULL;
	uint64_t remote_arg = 0;
	uint64_t remote_counter = 0;
	uint32_t bytes_done = 0;
	unsigned int i;
	unsigned int parked_index = UINT32_MAX;

	memset(&freeze_req, 0, sizeof(freeze_req));
	memset(&create_req, 0, sizeof(create_req));
	memset(&parked_entry, 0, sizeof(parked_entry));
	memset(&stop_req, 0, sizeof(stop_req));
	memset(tids, 0, sizeof(tids));

	if (write_target_memory(session_fd, info->remote_thread_tid_addr, &zero,
				sizeof(zero), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(zero))
		return -1;
	if (write_target_memory(session_fd, info->remote_thread_arg_addr, &zero,
				sizeof(zero), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(zero))
		return -1;
	if (write_target_memory(session_fd, info->remote_thread_counter_addr, &zero,
				sizeof(zero), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(zero))
		return -1;

	if (wait_for_freeze_thread_tids(session_fd, info, tids) < 0)
		return -1;
	if (freeze_target_threads(session_fd, 2000, &freeze_req, 0) < 0)
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
		thaw_target_threads(session_fd, 2000, NULL, 0);
		return -1;
	}

	if (remote_thread_create(session_fd, tids[parked_index],
				 info->remote_thread_launcher_addr,
				 info->remote_thread_start_addr, arg_value, 0, 0,
				 0, 5000, &create_req) < 0)
		return -1;
	if (create_req.created_tid <= 0 || create_req.result <= 0 ||
	    !create_req.call_id || !create_req.stop_cookie) {
		fprintf(stderr,
			"remote thread create reply mismatch tid=%d result=%" PRId64 " call_id=%" PRIu64 " cookie=%" PRIu64 "\n",
			create_req.created_tid, (int64_t)create_req.result,
			(uint64_t)create_req.call_id, (uint64_t)create_req.stop_cookie);
		return -1;
	}

	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_REMOTE_CALL,
			      &stop_req) < 0)
		return -1;
	if (stop_req.stop.cookie != create_req.stop_cookie ||
	    stop_req.stop.value0 != (uint64_t)create_req.result ||
	    stop_req.stop.value1 != create_req.call_id) {
		fprintf(stderr,
			"remote thread create stop mismatch cookie=%" PRIu64 " value0=%" PRIu64 " value1=%" PRIu64 "\n",
			(uint64_t)stop_req.stop.cookie, (uint64_t)stop_req.stop.value0,
			(uint64_t)stop_req.stop.value1);
		return -1;
	}

	if (read_target_memory(session_fd, info->remote_thread_tid_addr, &remote_arg,
			       sizeof(remote_arg), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(remote_arg))
		return -1;
	if ((pid_t)remote_arg != create_req.created_tid) {
		fprintf(stderr,
			"remote thread tid mismatch remote=%" PRIu64 " created=%d\n",
			remote_arg, create_req.created_tid);
		return -1;
	}
	if (read_target_memory(session_fd, info->remote_thread_arg_addr, &remote_arg,
			       sizeof(remote_arg), &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(remote_arg))
		return -1;
	if (remote_arg != arg_value) {
		fprintf(stderr,
			"remote thread arg mismatch got=0x%" PRIx64 " expected=0x%" PRIx64 "\n",
			remote_arg, arg_value);
		return -1;
	}
	if (read_target_memory(session_fd, info->remote_thread_counter_addr,
			       &remote_counter, sizeof(remote_counter),
			       &bytes_done, 0) < 0 ||
	    bytes_done != sizeof(remote_counter))
		return -1;
	if (remote_counter == 0) {
		fprintf(stderr, "remote thread counter did not advance\n");
		return -1;
	}

	if (continue_target(session_fd, create_req.stop_cookie, 2000, 0, NULL) <
	    0)
		return -1;

	printf("selftest remote thread create ok parked=%d created=%d arg=0x%" PRIx64 "\n",
	       tids[parked_index], create_req.created_tid, arg_value);
	return 0;
}

static int run_remote_call_workflow(int session_fd, uint64_t target_pc,
				    const uint64_t *args, uint32_t arg_count,
				    uint32_t flags, uint64_t stack_ptr,
				    uint64_t return_pc, uint64_t x8)
{
	struct lkmdbg_freeze_request freeze_req;
	struct lkmdbg_stop_query_request stop_req;
	struct lkmdbg_event_record event;
	struct lkmdbg_thread_entry parked_entry;
	struct lkmdbg_remote_call_request call_req;
	uint64_t freeze_cookie = 0;
	int remote_stop_active = 0;
	int ret = -1;

	memset(&freeze_req, 0, sizeof(freeze_req));
	memset(&stop_req, 0, sizeof(stop_req));
	memset(&event, 0, sizeof(event));
	memset(&parked_entry, 0, sizeof(parked_entry));
	memset(&call_req, 0, sizeof(call_req));

	if (freeze_target_threads(session_fd, 2000, &freeze_req, 0) < 0)
		goto out;
	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_FREEZE, &stop_req) <
	    0)
		goto out;
	freeze_cookie = stop_req.stop.cookie;

	if (find_first_parked_thread(session_fd, &parked_entry) < 0)
		goto out;
	if (drain_session_events(session_fd) < 0)
		goto out;

	if (remote_call_thread_ex(session_fd, parked_entry.tid, target_pc, args,
				  arg_count, flags, stack_ptr, return_pc, x8,
				  &call_req) < 0)
		goto out;
	if (continue_target(session_fd, freeze_cookie, 2000, 0, NULL) < 0)
		goto out;
	freeze_cookie = 0;

	if (wait_for_stop_event_or_state(session_fd,
					 LKMDBG_STOP_REASON_REMOTE_CALL,
					 5000, &event, &stop_req) < 0)
		goto out;
	remote_stop_active = 1;

	printf("remote_call.tid=%d\n", parked_entry.tid);
	printf("remote_call.call_id=%" PRIu64 "\n", (uint64_t)call_req.call_id);
	printf("remote_call.target_pc=0x%" PRIx64 "\n",
	       (uint64_t)target_pc);
	printf("remote_call.retval=0x%" PRIx64 "\n",
	       (uint64_t)stop_req.stop.value0);
	printf("remote_call.stop_cookie=%" PRIu64 "\n",
	       (uint64_t)stop_req.stop.cookie);
	printf("remote_call.stop_pc=0x%" PRIx64 "\n",
	       (uint64_t)stop_req.stop.regs.pc);
	printf("remote_call.stop_value1=0x%" PRIx64 "\n",
	       (uint64_t)stop_req.stop.value1);

	if (continue_target(session_fd, stop_req.stop.cookie, 2000, 0, NULL) < 0)
		goto out;

	remote_stop_active = 0;
	ret = 0;

out:
	if (ret < 0) {
		if (remote_stop_active)
			continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
					NULL);
		else if (freeze_cookie)
			continue_target(session_fd, freeze_cookie, 2000, 0, NULL);
	}
	return ret;
}

static int run_remote_thread_create_workflow(int session_fd,
					     uint64_t launcher_pc,
					     uint64_t start_pc,
					     uint64_t start_arg,
					     uint64_t stack_top,
					     uint64_t tls,
					     uint32_t flags)
{
	struct lkmdbg_freeze_request freeze_req;
	struct lkmdbg_stop_query_request stop_req;
	struct lkmdbg_event_record event;
	struct lkmdbg_thread_entry parked_entry;
	struct lkmdbg_remote_thread_create_request create_req;
	uint64_t freeze_cookie = 0;
	int remote_stop_active = 0;
	int ret = -1;

	memset(&freeze_req, 0, sizeof(freeze_req));
	memset(&stop_req, 0, sizeof(stop_req));
	memset(&event, 0, sizeof(event));
	memset(&parked_entry, 0, sizeof(parked_entry));
	memset(&create_req, 0, sizeof(create_req));

	if (freeze_target_threads(session_fd, 5000, &freeze_req, 0) < 0)
		goto out;
	if (expect_stop_state(session_fd, LKMDBG_STOP_REASON_FREEZE, &stop_req) <
	    0)
		goto out;
	freeze_cookie = stop_req.stop.cookie;

	if (find_first_parked_thread(session_fd, &parked_entry) < 0)
		goto out;

	if (remote_thread_create(session_fd, parked_entry.tid, launcher_pc,
				 start_pc, start_arg, stack_top, tls, flags,
				 5000, &create_req) < 0)
		goto out;

	freeze_cookie = 0;
	if (wait_for_stop_event_or_state(session_fd,
					 LKMDBG_STOP_REASON_REMOTE_CALL,
					 5000, &event, &stop_req) < 0)
		goto out;
	remote_stop_active = 1;

	printf("remote_thread.tid=%d\n", parked_entry.tid);
	printf("remote_thread.created_tid=%d\n", create_req.created_tid);
	printf("remote_thread.result=%" PRId64 "\n",
	       (int64_t)create_req.result);
	printf("remote_thread.call_id=%" PRIu64 "\n",
	       (uint64_t)create_req.call_id);
	printf("remote_thread.stop_cookie=%" PRIu64 "\n",
	       (uint64_t)create_req.stop_cookie);

	if (continue_target(session_fd, create_req.stop_cookie, 2000, 0, NULL) <
	    0)
		goto out;

	remote_stop_active = 0;
	ret = 0;

out:
	if (ret < 0) {
		if (remote_stop_active)
			continue_target(session_fd, stop_req.stop.cookie, 2000, 0,
					NULL);
		else if (freeze_cookie)
			continue_target(session_fd, freeze_cookie, 2000, 0, NULL);
	}
	return ret;
}

static int verify_inflight_exit_race(void)
{
	static const useconds_t delays_us[] = { 5000U, 2000U, 500U, 100U };
	unsigned int attempt;

	for (attempt = 0; attempt < sizeof(delays_us) / sizeof(delays_us[0]);
	     attempt++) {
		struct delayed_signal_ctx kill_ctx;
		struct child_info info;
		struct lkmdbg_mem_op ops[SELFTEST_RACE_OPS];
		struct lkmdbg_mem_request req;
		unsigned char *buf = NULL;
		pthread_t killer_thread;
		size_t total_len;
		pid_t child;
		int info_fd = -1;
		int cmd_fd = -1;
		int resp_fd = -1;
		int session_fd = -1;
		int status;
		int xfer_ret;
		unsigned int i;
		uint32_t race_ops;

		memset(&info, 0, sizeof(info));
		memset(&kill_ctx, 0, sizeof(kill_ctx));
		memset(ops, 0, sizeof(ops));
		memset(&req, 0, sizeof(req));
		if (start_selftest_child(&child, &info_fd, &cmd_fd, &resp_fd,
					 &info) < 0)
			return -1;
		session_fd = open_target_session(child);
		if (session_fd < 0) {
			close(info_fd);
			stop_selftest_child(child, cmd_fd, resp_fd, 1);
			return -1;
		}

		race_ops = SELFTEST_RACE_MAX_BATCH_BYTES / info.large_len;
		if (!race_ops)
			race_ops = 1;
		if (race_ops > SELFTEST_RACE_OPS)
			race_ops = SELFTEST_RACE_OPS;

		total_len = (size_t)info.large_len * race_ops;
		buf = malloc(total_len);
		if (!buf) {
			fprintf(stderr, "race buffer allocation failed\n");
			close(session_fd);
			close(info_fd);
			stop_selftest_child(child, cmd_fd, resp_fd, 1);
			return -1;
		}
		fill_pattern(buf, total_len, attempt + 41U);
		for (i = 0; i < race_ops; i++) {
			size_t segment_off = (size_t)i * info.large_len;

			ops[i].remote_addr = info.large_addr;
			ops[i].local_addr = (uintptr_t)(buf + segment_off);
			ops[i].length = info.large_len;
		}

		kill_ctx.pid = child;
		kill_ctx.signo = SIGKILL;
		kill_ctx.delay_us = delays_us[attempt];
		if (pthread_create(&killer_thread, NULL, delayed_signal_thread_main,
				   &kill_ctx) != 0) {
			fprintf(stderr, "race killer thread create failed\n");
			free(buf);
			close(session_fd);
			close(info_fd);
			stop_selftest_child(child, cmd_fd, resp_fd, 1);
			return -1;
		}

		errno = 0;
		xfer_ret = xfer_target_memory(session_fd, ops, race_ops, 1, &req,
					      0);
		pthread_join(killer_thread, NULL);
		if (kill_ctx.ret < 0 && kill_ctx.err != ESRCH) {
			fprintf(stderr, "race killer signal failed err=%d\n",
				kill_ctx.err);
			free(buf);
			close(session_fd);
			close(info_fd);
			close(cmd_fd);
			close(resp_fd);
			waitpid(child, NULL, 0);
			return -1;
		}
		if (wait_for_target_exit_event(session_fd, child, NULL) < 0) {
			free(buf);
			close(session_fd);
			close(info_fd);
			close(cmd_fd);
			close(resp_fd);
			waitpid(child, NULL, 0);
			return -1;
		}
		if (wait_for_child_exit(child, &status) < 0) {
			free(buf);
			close(session_fd);
			close(info_fd);
			close(cmd_fd);
			close(resp_fd);
			return -1;
		}
		close(cmd_fd);
		close(resp_fd);
		close(info_fd);

		if (WIFSIGNALED(status) && WTERMSIG(status) != SIGKILL) {
			fprintf(stderr, "race child died with signal=%d\n",
				WTERMSIG(status));
			free(buf);
			close(session_fd);
			return -1;
		}

		if (expect_dead_mem_accesses_fail(session_fd, &info) < 0) {
			free(buf);
			close(session_fd);
			return -1;
		}
		printf("selftest in-flight exit race ok attempt=%u delay_us=%u interrupted=%u ret=%d errno=%d ops_done=%u bytes_done=%" PRIu64 "\n",
		       attempt + 1U, delays_us[attempt],
		       (xfer_ret < 0 && errno == ESRCH) ||
			       req.ops_done < race_ops ||
			       req.bytes_done < total_len,
		       xfer_ret, errno, req.ops_done, (uint64_t)req.bytes_done);
		free(buf);
		close(session_fd);
		return 0;
	}

	fprintf(stderr, "in-flight exit race attempts exhausted\n");
	return -1;
}

static int verify_target_exit_cleanup_and_rebind(int session_fd, pid_t child,
						 const struct child_info *info,
						 int cmd_fd, int resp_fd)
{
	struct lkmdbg_remote_map_request map_reply;
	struct lkmdbg_remote_map_handle_request map_remove_reply;
	struct lkmdbg_remote_map_query_request map_query_reply;
	struct remote_map_query_buffer map_query_buf = { 0 };
	struct lkmdbg_remote_alloc_request alloc_reply;
	struct lkmdbg_remote_alloc_handle_request alloc_remove_reply;
	struct lkmdbg_remote_alloc_query_request alloc_query_reply;
	struct remote_alloc_query_buffer alloc_query_buf = { 0 };
	struct lkmdbg_pte_patch_request pte_apply_reply;
	struct lkmdbg_pte_patch_request pte_remove_reply;
	struct lkmdbg_pte_patch_entry pte_entries[4];
	struct lkmdbg_pte_patch_query_request pte_query_reply;
	struct child_info replacement_info;
	struct lkmdbg_event_record exit_event;
	char replacement_readback[32];
	char rebind_payload[32] = "rebind-target-ok";
	char initial_payload[] = "child-buffer-initial";
	uintptr_t shell_addr = info->large_addr + (uintptr_t)(info->page_size * 32U);
	pid_t replacement_child = -1;
	int replacement_info_fd = -1;
	int replacement_cmd_fd = -1;
	int replacement_resp_fd = -1;
	int status;
	int ret = -1;

	memset(&map_reply, 0, sizeof(map_reply));
	memset(&map_remove_reply, 0, sizeof(map_remove_reply));
	memset(&map_query_reply, 0, sizeof(map_query_reply));
	memset(&alloc_reply, 0, sizeof(alloc_reply));
	memset(&alloc_remove_reply, 0, sizeof(alloc_remove_reply));
	memset(&alloc_query_reply, 0, sizeof(alloc_query_reply));
	memset(&pte_apply_reply, 0, sizeof(pte_apply_reply));
	memset(&pte_remove_reply, 0, sizeof(pte_remove_reply));
	memset(&pte_query_reply, 0, sizeof(pte_query_reply));
	memset(&replacement_info, 0, sizeof(replacement_info));
	memset(&exit_event, 0, sizeof(exit_event));
	(void)resp_fd;

	map_query_buf.entries =
		calloc(REMOTE_MAP_QUERY_BATCH, sizeof(*map_query_buf.entries));
	alloc_query_buf.entries = calloc(REMOTE_ALLOC_QUERY_BATCH,
					 sizeof(*alloc_query_buf.entries));
	if (!map_query_buf.entries || !alloc_query_buf.entries) {
		fprintf(stderr, "exit cleanup query allocation failed\n");
		goto out;
	}
	if (shell_addr + SELFTEST_SOAK_REMOTE_ALLOC_LEN >
	    info->large_addr + info->large_len) {
		fprintf(stderr, "exit cleanup shell range out of bounds\n");
		goto out;
	}

	if (create_remote_map(session_fd, info->large_addr, 0, info->page_size,
			      LKMDBG_REMOTE_MAP_PROT_READ, 0,
			      &map_reply) < 0)
		goto out;
	if (map_reply.map_fd >= 0) {
		close(map_reply.map_fd);
		map_reply.map_fd = -1;
	}
	if (create_remote_alloc(session_fd, shell_addr,
				SELFTEST_SOAK_REMOTE_ALLOC_LEN,
				LKMDBG_REMOTE_ALLOC_PROT_READ |
					LKMDBG_REMOTE_ALLOC_PROT_WRITE,
				&alloc_reply) < 0)
		goto out;
	if (apply_pte_patch(session_fd, info->basic_addr,
			    LKMDBG_PTE_MODE_PROTNONE, 0, 0,
			    &pte_apply_reply) < 0)
		goto out;
	printf("selftest target exit cleanup stage=armed resources\n");
	fflush(stdout);

	if (request_child_exit(cmd_fd) < 0)
		goto out;
	printf("selftest target exit cleanup stage=exit requested\n");
	fflush(stdout);
	if (wait_for_target_exit_event(session_fd, child, &exit_event) < 0)
		goto out;
	printf("selftest target exit cleanup stage=exit event received tid=%d code=%" PRIu64 "\n",
	       exit_event.tid, (uint64_t)exit_event.value0);
	fflush(stdout);
	if (wait_for_child_exit(child, &status) < 0)
		goto out;
	printf("selftest target exit cleanup stage=child reaped status=%d\n", status);
	fflush(stdout);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "target exit child status=%d\n", status);
		goto out;
	}

	if (expect_dead_mem_accesses_fail(session_fd, info) < 0)
		goto out;
	printf("selftest target exit cleanup stage=dead access verified\n");

	if (query_remote_maps(session_fd, 0, &map_query_buf, &map_query_reply) < 0)
		goto out;
	if (map_query_reply.entries_filled != 1 ||
	    map_query_buf.entries[0].map_id != map_reply.map_id) {
		fprintf(stderr,
			"dead target remote map query mismatch filled=%u id=%" PRIu64 "\n",
			map_query_reply.entries_filled,
			map_query_reply.entries_filled ?
				(uint64_t)map_query_buf.entries[0].map_id :
				0ULL);
		goto out;
	}
	if (query_remote_allocs(session_fd, 0, &alloc_query_buf,
				&alloc_query_reply) < 0)
		goto out;
	if (alloc_query_reply.entries_filled != 1 ||
	    alloc_query_buf.entries[0].alloc_id != alloc_reply.alloc_id) {
		fprintf(stderr,
			"dead target remote alloc query mismatch filled=%u id=%" PRIu64 "\n",
			alloc_query_reply.entries_filled,
			alloc_query_reply.entries_filled ?
				(uint64_t)alloc_query_buf.entries[0].alloc_id :
				0ULL);
		goto out;
	}
	if (query_pte_patches(session_fd, 0, pte_entries,
			      (uint32_t)(sizeof(pte_entries) /
					 sizeof(pte_entries[0])),
			      &pte_query_reply) < 0)
		goto out;
	if (pte_query_reply.entries_filled != 1 ||
	    pte_entries[0].id != pte_apply_reply.id ||
	    !(pte_entries[0].state & LKMDBG_PTE_PATCH_STATE_LOST)) {
		fprintf(stderr,
			"dead target pte query mismatch filled=%u id=%" PRIu64 " state=0x%x\n",
			pte_query_reply.entries_filled,
			pte_query_reply.entries_filled ?
				(uint64_t)pte_entries[0].id :
				0ULL,
			pte_query_reply.entries_filled ? pte_entries[0].state : 0U);
		goto out;
	}

	if (remove_remote_map(session_fd, map_reply.map_id, &map_remove_reply) < 0)
		goto out;
	if (remove_remote_alloc(session_fd, alloc_reply.alloc_id,
				&alloc_remove_reply) < 0)
		goto out;
	if (remove_pte_patch(session_fd, pte_apply_reply.id, &pte_remove_reply) <
	    0)
		goto out;
	if (!(pte_remove_reply.state & LKMDBG_PTE_PATCH_STATE_LOST)) {
		fprintf(stderr, "dead target pte remove state mismatch=0x%x\n",
			pte_remove_reply.state);
		goto out;
	}
	printf("selftest target exit cleanup stage=dead resources cleaned\n");

	errno = 0;
	if (create_remote_map(session_fd, info->large_addr, 0, info->page_size,
			      LKMDBG_REMOTE_MAP_PROT_READ, 0,
			      &map_reply) == 0 || errno != ESRCH) {
		fprintf(stderr,
			"dead target create remote map mismatch errno=%d\n",
			errno);
		goto out;
	}
	errno = 0;
	if (create_remote_alloc(session_fd, shell_addr,
				SELFTEST_SOAK_REMOTE_ALLOC_LEN,
				LKMDBG_REMOTE_ALLOC_PROT_READ,
				&alloc_reply) == 0 || errno != ESRCH) {
		fprintf(stderr,
			"dead target create remote alloc mismatch errno=%d\n",
			errno);
		goto out;
	}

	if (start_selftest_child(&replacement_child, &replacement_info_fd,
				 &replacement_cmd_fd, &replacement_resp_fd,
				 &replacement_info) < 0)
		goto out;
	printf("selftest target exit cleanup stage=replacement started pid=%d\n",
	       replacement_child);
	if (child_read_remote_range(replacement_cmd_fd, replacement_resp_fd,
				    replacement_info.basic_addr,
				    replacement_readback,
				    sizeof(initial_payload)) < 0)
		goto out;
	if (memcmp(replacement_readback, initial_payload,
		   sizeof(initial_payload)) != 0) {
		fprintf(stderr, "replacement child initial payload mismatch\n");
		goto out;
	}
	memset(replacement_readback, 0, sizeof(replacement_readback));
	errno = 0;
	if (write_target_memory(session_fd, replacement_info.basic_addr,
				"stale-session", sizeof("stale-session"), NULL,
				0) == 0 || errno != ESRCH) {
		fprintf(stderr, "stale session write mismatch errno=%d\n", errno);
		goto out;
	}
	if (child_read_remote_range(replacement_cmd_fd, replacement_resp_fd,
				    replacement_info.basic_addr,
				    replacement_readback,
				    sizeof(initial_payload)) < 0)
		goto out;
	if (memcmp(replacement_readback, initial_payload,
		   sizeof(initial_payload)) != 0) {
		fprintf(stderr,
			"replacement child changed before rebind unexpectedly\n");
		goto out;
	}

	if (set_target(session_fd, replacement_child) < 0)
		goto out;
	printf("selftest target exit cleanup stage=rebound pid=%d\n",
	       replacement_child);
	if (write_target_memory(session_fd, replacement_info.basic_addr,
				rebind_payload, strlen(rebind_payload) + 1, NULL,
				0) < 0)
		goto out;
	if (verify_remote_string(session_fd, replacement_info.basic_addr,
				 rebind_payload) < 0)
		goto out;
	if (child_read_remote_range(replacement_cmd_fd, replacement_resp_fd,
				    replacement_info.basic_addr,
				    replacement_readback,
				    strlen(rebind_payload) + 1) < 0)
		goto out;
	if (strcmp(replacement_readback, rebind_payload) != 0) {
		fprintf(stderr, "replacement child rebind writeback mismatch\n");
		goto out;
	}

	printf("selftest target exit cleanup ok exit_code=%" PRIu64 " rebind_pid=%d\n",
	       (uint64_t)exit_event.value0, replacement_child);
	ret = 0;

out:
	if (replacement_info_fd >= 0)
		close(replacement_info_fd);
	if (replacement_cmd_fd >= 0 && replacement_resp_fd >= 0)
		stop_selftest_child(replacement_child, replacement_cmd_fd,
				    replacement_resp_fd, 1);
	free(map_query_buf.entries);
	free(alloc_query_buf.entries);
	return ret;
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
		"  %s setreg <pid> <tid> <reg> <value_hex>  (reg: xN|sp|pc|pstate|fpsr|fpcr|vN_lo|vN_hi)\n"
		"  %s hwadd <pid> <tid> <r|w|rw|x|rx|wx|rwx> <addr_hex> <len> [stop|counter|mmu|flags] [after_hits] [oneshot|autocont|actions]\n"
		"  %s hwdel <pid> <id>\n"
		"  %s hwrearm <pid> <id>\n"
		"  %s hwlist <pid>\n"
		"  %s stop <pid>\n"
		"  %s cont <pid> [timeout_ms] [rearm|norearm]\n"
		"  %s step <pid> <tid>\n"
		"  %s freeze <pid> [timeout_ms]\n"
		"  %s thaw <pid> [timeout_ms]\n"
		"  %s sysset <pid> <off|event|stop|control|event+stop|event+control> <enter|exit|both> [tid] [syscall_nr]\n"
		"  %s sysget <pid>\n"
		"  %s sysresolve <pid> <allow|skip|rewrite> [retval|new_syscall_nr] [arg0 ... arg5]\n"
		"  %s rcall <pid> <target_pc_hex> [arg0 ... arg7]\n"
		"  %s rcallx8 <pid> <target_pc_hex> <x8_hex> [arg0 ... arg7]\n"
		"  %s rthread <pid> <launcher_pc_hex> <start_pc_hex> <arg_hex> <stack_top_hex> [tls_hex]\n"
		"  %s stealthset <pid> <none|debugfs|modulehide|sysfshide|ownerprochide|debugfs,modulehide,sysfshide,ownerprochide>\n"
		"  %s stealthget <pid>\n"
		"  %s events <pid> [max_events] [timeout_ms]\n"
		"  %s vmas <pid>\n"
		"  %s pages <pid> <remote_addr_hex> <length>\n"
		"  %s ptset <pid> <remote_addr_hex> <ro|rw|rx|rwx|protnone|execonly|raw:<pte_hex>>\n"
		"  %s ptdel <pid> <id>\n"
		"  %s ptlist <pid>\n"
		"  %s ralloc <pid> <remote_addr_hex> <length> <r|w|rw|x|rx|wx|rwx>\n"
		"  %s radel <pid> <id>\n"
		"  %s ralist <pid>\n"
		"  %s physread <phys_addr_hex> <length>\n"
		"  %s physwrite <phys_addr_hex> <ascii_data>\n"
		"  %s physreadv <pid> <remote_addr_hex> <length>\n"
		"  %s physwritev <pid> <remote_addr_hex> <ascii_data>\n"
		"  %s write <pid> <remote_addr_hex> <ascii_data>\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog, prog, prog);
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

	if (verify_remote_map(session_fd, child, &info, cmd_pipe[1],
			      resp_pipe[0]) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_remote_alloc(session_fd, child, &info, cmd_pipe[1],
				resp_pipe[0]) < 0) {
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

	if (verify_view_external_read(session_fd, child, &info, cmd_pipe[1],
				      resp_pipe[0]) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_view_wxshadow(session_fd, child, &info, cmd_pipe[1],
				 resp_pipe[0]) < 0) {
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

	if (verify_runtime_events(session_fd, cmd_pipe[1], resp_pipe[0], child,
				  &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_syscall_trace(session_fd, cmd_pipe[1], resp_pipe[0], child) <
	    0) {
		if (errno == EPIPE || errno == ECONNRESET) {
			printf("selftest syscall trace skipped: child channel unavailable errno=%d\n",
			       errno);
		} else {
			close(session_fd);
			close(info_pipe[0]);
			close(cmd_pipe[1]);
			close(resp_pipe[0]);
			kill(child, SIGKILL);
			waitpid(child, NULL, 0);
			return 1;
		}
	}

	if (verify_stealth_control(session_fd) < 0) {
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

	if (verify_remote_call(session_fd, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_remote_thread_create(session_fd, &info) < 0) {
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

	if (verify_multi_session_conflict(session_fd, child, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_cross_page_permission_flip(session_fd, &info, cmd_pipe[1],
					      resp_pipe[0]) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	if (verify_randomized_mem_soak(session_fd, child, &info) < 0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

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

	if (verify_inflight_exit_race() < 0) {
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

	if (verify_target_exit_cleanup_and_rebind(session_fd, child, &info,
						  cmd_pipe[1], resp_pipe[0]) <
	    0) {
		close(session_fd);
		close(info_pipe[0]);
		close(cmd_pipe[1]);
		close(resp_pipe[0]);
		free(large_buf);
		free(batch_a);
		free(batch_b);
		free(batch_c);
		free(batch_d);
		return 1;
	}

	close(session_fd);
	close(info_pipe[0]);
	close(cmd_pipe[1]);
	close(resp_pipe[0]);

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
	uint64_t remote_alloc_id = 0;
	size_t remote_alloc_len = 0;
	uint32_t remote_alloc_prot = 0;
	uint32_t continue_flags = 0;
	int syscall_trace_nr = -1;
	uint32_t syscall_trace_mode = 0;
	uint32_t syscall_trace_phases = 0;
	uint32_t syscall_resolve_action = 0;
	int syscall_resolve_have_action = 0;
	int syscall_resolve_nr = -1;
	int64_t syscall_resolve_retval = 0;
	uint64_t syscall_resolve_args[6] = { 0 };
	uint32_t syscall_resolve_arg_count = 0;
	uint32_t stealth_flags = 0;
	uint64_t remote_call_pc = 0;
	uint64_t remote_call_x8 = 0;
	uint64_t remote_call_args[LKMDBG_REMOTE_CALL_MAX_ARGS] = { 0 };
	uint32_t remote_call_arg_count = 0;
	uint32_t remote_call_flags = 0;
	uint64_t remote_thread_launcher = 0;
	uint64_t remote_thread_start = 0;
	uint64_t remote_thread_arg = 0;
	uint64_t remote_thread_stack = 0;
	uint64_t remote_thread_tls = 0;
	uint32_t remote_thread_flags = 0;
	unsigned int max_events = EVENT_READ_BATCH;

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		fprintf(stderr, "failed to ignore SIGPIPE\n");
		return 1;
	}

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
			    strcmp(argv[1], "ralloc") == 0 ||
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

	if (strcmp(argv[1], "sysset") == 0) {
		if (argc < 5) {
			usage(argv[0]);
			return 1;
		}
		if (parse_syscall_trace_mode(argv[3], &syscall_trace_mode) < 0) {
			fprintf(stderr, "invalid syscall trace mode: %s\n",
				argv[3]);
			return 1;
		}
		if (parse_syscall_trace_phases(argv[4], &syscall_trace_phases) < 0) {
			fprintf(stderr, "invalid syscall trace phases: %s\n",
				argv[4]);
			return 1;
		}
		if (argc >= 6) {
			tid = (pid_t)strtol(argv[5], &endp, 10);
			if (*endp != '\0' || tid < 0) {
				fprintf(stderr, "invalid tid: %s\n", argv[5]);
				return 1;
			}
		}
		if (argc >= 7) {
			syscall_trace_nr = (int)strtol(argv[6], &endp, 0);
			if (*endp != '\0' || syscall_trace_nr < -1) {
				fprintf(stderr, "invalid syscall_nr: %s\n",
					argv[6]);
				return 1;
			}
		}
		if (!syscall_trace_mode) {
			syscall_trace_phases = 0;
			syscall_trace_nr = -1;
			tid = 0;
		}
	}

	if (strcmp(argv[1], "sysresolve") == 0) {
		uint32_t i;
		int argi = 4;

		if (argc < 4) {
			usage(argv[0]);
			return 1;
		}
		if (parse_syscall_resolve_action(argv[3], &syscall_resolve_action) <
		    0) {
			fprintf(stderr, "invalid syscall resolve action: %s\n",
				argv[3]);
			return 1;
		}
		syscall_resolve_have_action = 1;

		if (syscall_resolve_action == LKMDBG_SYSCALL_RESOLVE_ACTION_SKIP) {
			if (argc < 5) {
				usage(argv[0]);
				return 1;
			}
			if (parse_s64_value(argv[4], &syscall_resolve_retval) < 0) {
				fprintf(stderr, "invalid skip retval: %s\n",
					argv[4]);
				return 1;
			}
			argi = 5;
		} else if (syscall_resolve_action ==
			   LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE) {
			if (argc < 5) {
				usage(argv[0]);
				return 1;
			}
			syscall_resolve_nr = (int)strtol(argv[4], &endp, 0);
			if (*endp != '\0' || syscall_resolve_nr < 0) {
				fprintf(stderr, "invalid rewrite syscall nr: %s\n",
					argv[4]);
				return 1;
			}
			argi = 5;
			syscall_resolve_arg_count = (uint32_t)(argc - argi);
			if (syscall_resolve_arg_count > 6) {
				fprintf(stderr,
					"too many syscall args: %u (max 6)\n",
					syscall_resolve_arg_count);
				return 1;
			}
			for (i = 0; i < syscall_resolve_arg_count; i++) {
				if (parse_u64_value(argv[argi + (int)i],
						    &syscall_resolve_args[i]) <
				    0) {
					fprintf(stderr,
						"invalid syscall arg: %s\n",
						argv[argi + (int)i]);
					return 1;
				}
			}
		}
	}

	if (strcmp(argv[1], "rcall") == 0 || strcmp(argv[1], "rcallx8") == 0) {
		uint32_t i;
		int argi;

		if ((strcmp(argv[1], "rcall") == 0 && argc < 4) ||
		    (strcmp(argv[1], "rcallx8") == 0 && argc < 5)) {
			usage(argv[0]);
			return 1;
		}
		if (parse_u64_value(argv[3], &remote_call_pc) < 0) {
			fprintf(stderr, "invalid remote call pc: %s\n", argv[3]);
			return 1;
		}
		argi = 4;
		if (strcmp(argv[1], "rcallx8") == 0) {
			if (parse_u64_value(argv[4], &remote_call_x8) < 0) {
				fprintf(stderr, "invalid remote call x8: %s\n",
					argv[4]);
				return 1;
			}
			remote_call_flags |= LKMDBG_REMOTE_CALL_FLAG_SET_X8;
			argi = 5;
		}
		remote_call_arg_count = (uint32_t)(argc - argi);
		if (remote_call_arg_count > LKMDBG_REMOTE_CALL_MAX_ARGS) {
			fprintf(stderr, "too many remote call args: %u (max %u)\n",
				remote_call_arg_count,
				LKMDBG_REMOTE_CALL_MAX_ARGS);
			return 1;
		}
		for (i = 0; i < remote_call_arg_count; i++) {
			if (parse_u64_value(argv[argi + (int)i],
					    &remote_call_args[i]) < 0) {
				fprintf(stderr, "invalid remote call arg: %s\n",
					argv[argi + (int)i]);
				return 1;
			}
		}
	}

	if (strcmp(argv[1], "rthread") == 0) {
		if (argc < 7) {
			usage(argv[0]);
			return 1;
		}
		if (parse_u64_value(argv[3], &remote_thread_launcher) < 0 ||
		    parse_u64_value(argv[4], &remote_thread_start) < 0 ||
		    parse_u64_value(argv[5], &remote_thread_arg) < 0 ||
		    parse_u64_value(argv[6], &remote_thread_stack) < 0) {
			fprintf(stderr, "invalid remote thread arguments\n");
			return 1;
		}
		if (argc >= 8) {
			if (parse_u64_value(argv[7], &remote_thread_tls) < 0) {
				fprintf(stderr, "invalid remote thread tls: %s\n",
					argv[7]);
				return 1;
			}
			remote_thread_flags |=
				LKMDBG_REMOTE_THREAD_CREATE_FLAG_SET_TLS;
		}
	}

	if (strcmp(argv[1], "stealthset") == 0) {
		if (argc < 4) {
			usage(argv[0]);
			return 1;
		}
		if (parse_stealth_flags(argv[3], &stealth_flags) < 0) {
			fprintf(stderr, "invalid stealth flags: %s\n", argv[3]);
			return 1;
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

	if (strcmp(argv[1], "ralloc") == 0) {
		if (argc < 6) {
			usage(argv[0]);
			return 1;
		}

		remote_alloc_len = (size_t)strtoull(argv[4], &endp, 0);
		if (*endp != '\0' || !remote_alloc_len) {
			fprintf(stderr, "invalid remote alloc length: %s\n",
				argv[4]);
			return 1;
		}

		if (parse_hwpoint_type(argv[5], &remote_alloc_prot) < 0) {
			fprintf(stderr, "invalid remote alloc prot: %s\n",
				argv[5]);
			return 1;
		}
	}

	if (strcmp(argv[1], "radel") == 0) {
		if (argc < 4) {
			usage(argv[0]);
			return 1;
		}

		remote_alloc_id = strtoull(argv[3], &endp, 0);
		if (*endp != '\0' || !remote_alloc_id) {
			fprintf(stderr, "invalid remote alloc id: %s\n",
				argv[3]);
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
	} else if (strcmp(argv[1], "sysset") == 0) {
		struct lkmdbg_syscall_trace_request reply;
		char flags_buf[32];
		char mode_buf[32];
		char phase_buf[32];

		memset(&reply, 0, sizeof(reply));
		if (set_syscall_trace(session_fd, tid, syscall_trace_nr,
				      syscall_trace_mode, syscall_trace_phases,
				      &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("syscall_trace.tid=%d\n", reply.tid);
		printf("syscall_trace.nr=%d\n", reply.syscall_nr);
		printf("syscall_trace.mode=0x%x(%s)\n", reply.mode,
		       describe_syscall_trace_mode(reply.mode, mode_buf,
						   sizeof(mode_buf)));
		printf("syscall_trace.phases=0x%x(%s)\n", reply.phases,
		       describe_syscall_trace_phases(reply.phases, phase_buf,
						     sizeof(phase_buf)));
		printf("syscall_trace.flags=0x%x(%s)\n", reply.flags,
		       describe_syscall_trace_flags(reply.flags, flags_buf,
						    sizeof(flags_buf)));
		printf("syscall_trace.supported_phases=0x%x(%s)\n",
		       reply.supported_phases,
		       describe_syscall_trace_phases(reply.supported_phases,
						     phase_buf,
						     sizeof(phase_buf)));
	} else if (strcmp(argv[1], "sysget") == 0) {
		struct lkmdbg_syscall_trace_request reply;
		char flags_buf[32];
		char mode_buf[32];
		char phase_buf[32];

		memset(&reply, 0, sizeof(reply));
		if (get_syscall_trace(session_fd, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("syscall_trace.tid=%d\n", reply.tid);
		printf("syscall_trace.nr=%d\n", reply.syscall_nr);
		printf("syscall_trace.mode=0x%x(%s)\n", reply.mode,
		       describe_syscall_trace_mode(reply.mode, mode_buf,
						   sizeof(mode_buf)));
		printf("syscall_trace.phases=0x%x(%s)\n", reply.phases,
		       describe_syscall_trace_phases(reply.phases, phase_buf,
						     sizeof(phase_buf)));
		printf("syscall_trace.flags=0x%x(%s)\n", reply.flags,
		       describe_syscall_trace_flags(reply.flags, flags_buf,
						    sizeof(flags_buf)));
		printf("syscall_trace.supported_phases=0x%x(%s)\n",
		       reply.supported_phases,
		       describe_syscall_trace_phases(reply.supported_phases,
						     phase_buf,
						     sizeof(phase_buf)));
	} else if (strcmp(argv[1], "sysresolve") == 0) {
		struct lkmdbg_stop_query_request stop_req;
		struct lkmdbg_syscall_resolve_request reply;
		uint64_t args[6] = { 0 };
		char action_buf[16];
		char flags_buf[32];
		uint32_t i;

		memset(&stop_req, 0, sizeof(stop_req));
		memset(&reply, 0, sizeof(reply));
		if (!syscall_resolve_have_action) {
			close(session_fd);
			return 1;
		}
		if (get_stop_state(session_fd, &stop_req) < 0) {
			close(session_fd);
			return 1;
		}
		if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_ACTIVE) ||
		    stop_req.stop.reason != LKMDBG_STOP_REASON_SYSCALL ||
		    !(stop_req.stop.flags & LKMDBG_STOP_FLAG_SYSCALL_CONTROL)) {
			fprintf(stderr,
				"current stop is not a controllable syscall stop\n");
			close(session_fd);
			return 1;
		}

		if (syscall_resolve_action ==
		    LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE) {
			if (!(stop_req.stop.flags & LKMDBG_STOP_FLAG_REGS_VALID)) {
				fprintf(stderr,
					"rewrite requires valid stop registers\n");
				close(session_fd);
				return 1;
			}
			for (i = 0; i < 6; i++)
				args[i] = stop_req.stop.regs.regs[i];
			for (i = 0; i < syscall_resolve_arg_count; i++)
				args[i] = syscall_resolve_args[i];
		}

		if (resolve_syscall(session_fd, stop_req.stop.cookie,
				    syscall_resolve_action, syscall_resolve_nr,
				    syscall_resolve_action ==
						    LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE
					    ? args
					    : NULL,
				    syscall_resolve_retval, &reply) < 0) {
			close(session_fd);
			return 1;
		}

		printf("syscall_resolve.stop_cookie=%" PRIu64 "\n",
		       (uint64_t)reply.stop_cookie);
		printf("syscall_resolve.action=%u(%s)\n", reply.action,
		       describe_syscall_resolve_action(reply.action, action_buf,
						       sizeof(action_buf)));
		printf("syscall_resolve.syscall_nr=%d\n", reply.syscall_nr);
		printf("syscall_resolve.retval=%" PRId64 "\n",
		       (int64_t)reply.retval);
		printf("syscall_resolve.backend_flags=0x%x(%s)\n",
		       reply.backend_flags,
		       describe_syscall_resolve_flags(reply.backend_flags,
						      flags_buf,
						      sizeof(flags_buf)));
	} else if (strcmp(argv[1], "rcall") == 0 ||
		   strcmp(argv[1], "rcallx8") == 0) {
		if (run_remote_call_workflow(session_fd, remote_call_pc,
					     remote_call_args,
					     remote_call_arg_count,
					     remote_call_flags, 0, 0,
					     remote_call_x8) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "rthread") == 0) {
		if (run_remote_thread_create_workflow(session_fd,
						      remote_thread_launcher,
						      remote_thread_start,
						      remote_thread_arg,
						      remote_thread_stack,
						      remote_thread_tls,
						      remote_thread_flags) < 0) {
			close(session_fd);
			return 1;
		}
	} else if (strcmp(argv[1], "stealthset") == 0) {
		struct lkmdbg_stealth_request reply;
		char flags_buf[48];
		char supported_buf[48];

		memset(&reply, 0, sizeof(reply));
		if (set_stealth(session_fd, stealth_flags, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("stealth.flags=0x%x(%s)\n", reply.flags,
		       describe_stealth_flags(reply.flags, flags_buf,
					      sizeof(flags_buf)));
		printf("stealth.supported=0x%x(%s)\n", reply.supported_flags,
		       describe_stealth_flags(reply.supported_flags,
					      supported_buf,
					      sizeof(supported_buf)));
	} else if (strcmp(argv[1], "stealthget") == 0) {
		struct lkmdbg_stealth_request reply;
		char flags_buf[48];
		char supported_buf[48];

		memset(&reply, 0, sizeof(reply));
		if (get_stealth(session_fd, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("stealth.flags=0x%x(%s)\n", reply.flags,
		       describe_stealth_flags(reply.flags, flags_buf,
					      sizeof(flags_buf)));
		printf("stealth.supported=0x%x(%s)\n", reply.supported_flags,
		       describe_stealth_flags(reply.supported_flags,
					      supported_buf,
					      sizeof(supported_buf)));
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
	} else if (strcmp(argv[1], "ralloc") == 0) {
		struct lkmdbg_remote_alloc_request reply;

		memset(&reply, 0, sizeof(reply));
		if (create_remote_alloc(session_fd, remote_addr, remote_alloc_len,
					remote_alloc_prot, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("remote_alloc.id=%" PRIu64 "\n",
		       (uint64_t)reply.alloc_id);
		printf("remote_alloc.addr=0x%" PRIx64 "\n",
		       (uint64_t)reply.remote_addr);
		printf("remote_alloc.len=%" PRIu64 "\n",
		       (uint64_t)reply.mapped_length);
		printf("remote_alloc.prot=0x%x\n", reply.prot);
	} else if (strcmp(argv[1], "radel") == 0) {
		struct lkmdbg_remote_alloc_handle_request reply;

		memset(&reply, 0, sizeof(reply));
		if (remove_remote_alloc(session_fd, remote_alloc_id, &reply) < 0) {
			close(session_fd);
			return 1;
		}
		printf("remote_alloc.id=%" PRIu64 "\n",
		       (uint64_t)reply.alloc_id);
		printf("remote_alloc.tgid=%d\n", reply.target_tgid);
		printf("remote_alloc.addr=0x%" PRIx64 "\n",
		       (uint64_t)reply.remote_addr);
		printf("remote_alloc.len=%" PRIu64 "\n",
		       (uint64_t)reply.mapped_length);
		printf("remote_alloc.prot=0x%x\n", reply.prot);
	} else if (strcmp(argv[1], "ralist") == 0) {
		struct lkmdbg_remote_alloc_entry entries[16];
		uint64_t cursor = 0;

		for (;;) {
			struct remote_alloc_query_buffer buf = {
				.entries = entries,
			};
			struct lkmdbg_remote_alloc_query_request reply;
			uint32_t i;

			if (query_remote_allocs(session_fd, cursor, &buf, &reply) <
			    0) {
				close(session_fd);
				return 1;
			}

			for (i = 0; i < reply.entries_filled; i++) {
				printf("id=%" PRIu64 " tgid=%d addr=0x%" PRIx64 " len=%" PRIu64 " prot=0x%x flags=0x%x\n",
				       (uint64_t)entries[i].alloc_id,
				       entries[i].target_tgid,
				       (uint64_t)entries[i].remote_addr,
				       (uint64_t)entries[i].mapped_length,
				       entries[i].prot, entries[i].flags);
			}

			if (reply.done)
				break;
			cursor = reply.next_id;
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
