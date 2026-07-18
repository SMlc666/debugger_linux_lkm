#define _GNU_SOURCE

#include "lkmdbg/lkmdbg.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct lkmdbg_session {
	int fd;
	bool owns_fd;
};

struct lkmdbg_remote_mapping {
	int session_fd;
	int map_fd;
	uint64_t map_id;
	uint64_t length;
	void *data;
};

struct lkmdbg_remote_allocation {
	int session_fd;
	uint64_t alloc_id;
	uintptr_t remote_address;
	uint64_t length;
};

static int lkmdbg_validate_session(const struct lkmdbg_session *session)
{
	if (!session || session->fd < 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int lkmdbg_session_adopt_fd(int fd, int take_ownership,
			    struct lkmdbg_session **session_out)
{
	struct lkmdbg_session *session;

	if (fd < 0 || !session_out) {
		errno = EINVAL;
		return -1;
	}
	*session_out = NULL;
	session = calloc(1, sizeof(*session));
	if (!session)
		return -1;
	session->fd = fd;
	session->owns_fd = take_ownership != 0;
	*session_out = session;
	return 0;
}

int lkmdbg_session_open(struct lkmdbg_session **session_out)
{
	struct lkmdbg_open_session_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};
	int proc_fd;
	int session_fd;

	if (!session_out) {
		errno = EINVAL;
		return -1;
	}
	*session_out = NULL;
	proc_fd = open("/proc/version", O_RDONLY | O_CLOEXEC);
	if (proc_fd < 0)
		return -1;
	session_fd = ioctl(proc_fd, LKMDBG_IOC_OPEN_SESSION, &req);
	close(proc_fd);
	if (session_fd < 0)
		return -1;
	if (lkmdbg_session_adopt_fd(session_fd, 1, session_out) < 0) {
		close(session_fd);
		return -1;
	}
	return 0;
}

void lkmdbg_session_close(struct lkmdbg_session *session)
{
	if (!session)
		return;
	if (session->owns_fd && session->fd >= 0)
		close(session->fd);
	session->fd = -1;
	free(session);
}

int lkmdbg_session_fd(const struct lkmdbg_session *session)
{
	if (lkmdbg_validate_session(session) < 0)
		return -1;
	return session->fd;
}

int lkmdbg_session_set_target(struct lkmdbg_session *session, pid_t tgid,
			      pid_t tid)
{
	struct lkmdbg_target_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tgid = tgid,
		.tid = tid,
	};

	if (lkmdbg_validate_session(session) < 0)
		return -1;
	if (tgid <= 0 || tid < 0) {
		errno = EINVAL;
		return -1;
	}
	return ioctl(session->fd, LKMDBG_IOC_SET_TARGET, &req);
}

int lkmdbg_session_get_status(struct lkmdbg_session *session,
			      struct lkmdbg_status_reply *status_out)
{
	if (lkmdbg_validate_session(session) < 0 || !status_out) {
		errno = EINVAL;
		return -1;
	}
	*status_out = (struct lkmdbg_status_reply) {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(*status_out),
	};
	return ioctl(session->fd, LKMDBG_IOC_GET_STATUS, status_out);
}

int lkmdbg_session_reset(struct lkmdbg_session *session)
{
	if (lkmdbg_validate_session(session) < 0)
		return -1;
	return ioctl(session->fd, LKMDBG_IOC_RESET_SESSION);
}

static int lkmdbg_memory_xferv(struct lkmdbg_session *session,
			       struct lkmdbg_mem_op *operations,
			       uint32_t operation_count, bool write,
			       struct lkmdbg_transfer_result *result_out)
{
	struct lkmdbg_mem_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.ops_addr = (uintptr_t)operations,
		.op_count = operation_count,
	};
	int ret;

	if (result_out)
		*result_out = (struct lkmdbg_transfer_result) { 0 };
	if (lkmdbg_validate_session(session) < 0 || !operations ||
	    operation_count == 0) {
		errno = EINVAL;
		return -1;
	}
	ret = ioctl(session->fd,
		    write ? LKMDBG_IOC_WRITE_MEM : LKMDBG_IOC_READ_MEM, &req);
	if (result_out) {
		result_out->operations_done = req.ops_done;
		result_out->bytes_done = req.bytes_done;
	}
	return ret;
}

int lkmdbg_memory_readv(struct lkmdbg_session *session,
			struct lkmdbg_mem_op *operations, uint32_t operation_count,
			struct lkmdbg_transfer_result *result_out)
{
	return lkmdbg_memory_xferv(session, operations, operation_count, false,
				   result_out);
}

int lkmdbg_memory_writev(struct lkmdbg_session *session,
			 struct lkmdbg_mem_op *operations,
			 uint32_t operation_count,
			 struct lkmdbg_transfer_result *result_out)
{
	return lkmdbg_memory_xferv(session, operations, operation_count, true,
				   result_out);
}

static int lkmdbg_memory_xfer(struct lkmdbg_session *session,
			      uintptr_t remote_address, void *buffer,
			      size_t length, uint32_t flags, bool write,
			      struct lkmdbg_transfer_result *result_out)
{
	struct lkmdbg_mem_op op;

	if (!buffer || length == 0 || length > UINT32_MAX) {
		errno = EINVAL;
		return -1;
	}
	op = (struct lkmdbg_mem_op) {
		.remote_addr = remote_address,
		.local_addr = (uintptr_t)buffer,
		.length = (uint32_t)length,
		.flags = flags,
	};
	return lkmdbg_memory_xferv(session, &op, 1, write, result_out);
}

int lkmdbg_memory_read(struct lkmdbg_session *session, uintptr_t remote_address,
		       void *buffer, size_t length, uint32_t flags,
		       struct lkmdbg_transfer_result *result_out)
{
	return lkmdbg_memory_xfer(session, remote_address, buffer, length, flags,
				  false, result_out);
}

int lkmdbg_memory_write(struct lkmdbg_session *session,
			uintptr_t remote_address, const void *buffer,
			size_t length, uint32_t flags,
			struct lkmdbg_transfer_result *result_out)
{
	return lkmdbg_memory_xfer(session, remote_address, (void *)buffer, length,
				  flags, true, result_out);
}

int lkmdbg_event_read(struct lkmdbg_session *session,
		      struct lkmdbg_event_record *events, size_t capacity,
		      size_t *count_out, int timeout_ms)
{
	struct pollfd pfd;
	ssize_t nr;

	if (count_out)
		*count_out = 0;
	if (lkmdbg_validate_session(session) < 0 || !events || capacity == 0 ||
	    capacity > SIZE_MAX / sizeof(*events) || timeout_ms < -1) {
		errno = EINVAL;
		return -1;
	}
	pfd = (struct pollfd) { .fd = session->fd, .events = POLLIN };
	for (;;) {
		int ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0) {
			if (ret == 0)
				errno = ETIMEDOUT;
			return -1;
		}
		break;
	}
	nr = read(session->fd, events, capacity * sizeof(*events));
	if (nr < 0)
		return -1;
	if ((size_t)nr % sizeof(*events) != 0) {
		errno = EPROTO;
		return -1;
	}
	if (count_out)
		*count_out = (size_t)nr / sizeof(*events);
	return 0;
}

int lkmdbg_threads_query(struct lkmdbg_session *session, int32_t start_tid,
			 struct lkmdbg_thread_entry *entries, uint32_t capacity,
			 struct lkmdbg_thread_query_request *result_out)
{
	struct lkmdbg_thread_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = capacity,
		.start_tid = start_tid,
	};
	int ret;

	if (result_out)
		*result_out = (struct lkmdbg_thread_query_request) { 0 };
	if (lkmdbg_validate_session(session) < 0 || !entries || capacity == 0 ||
	    start_tid < 0) {
		errno = EINVAL;
		return -1;
	}
	ret = ioctl(session->fd, LKMDBG_IOC_QUERY_THREADS, &req);
	if (result_out)
		*result_out = req;
	return ret;
}

static int lkmdbg_threads_control(struct lkmdbg_session *session,
				  uint32_t timeout_ms, bool thaw,
				  struct lkmdbg_freeze_request *result_out)
{
	struct lkmdbg_freeze_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.timeout_ms = timeout_ms,
	};
	int ret;

	if (result_out)
		*result_out = (struct lkmdbg_freeze_request) { 0 };
	if (lkmdbg_validate_session(session) < 0)
		return -1;
	ret = ioctl(session->fd,
		    thaw ? LKMDBG_IOC_THAW_THREADS : LKMDBG_IOC_FREEZE_THREADS,
		    &req);
	if (result_out)
		*result_out = req;
	return ret;
}

int lkmdbg_threads_freeze(struct lkmdbg_session *session, uint32_t timeout_ms,
			  struct lkmdbg_freeze_request *result_out)
{
	return lkmdbg_threads_control(session, timeout_ms, false, result_out);
}

int lkmdbg_threads_thaw(struct lkmdbg_session *session, uint32_t timeout_ms,
			struct lkmdbg_freeze_request *result_out)
{
	return lkmdbg_threads_control(session, timeout_ms, true, result_out);
}

int lkmdbg_registers_get(struct lkmdbg_session *session, pid_t tid,
			 struct lkmdbg_regs_arm64 *registers_out)
{
	struct lkmdbg_thread_regs_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
	};

	if (lkmdbg_validate_session(session) < 0 || tid <= 0 || !registers_out) {
		errno = EINVAL;
		return -1;
	}
	if (ioctl(session->fd, LKMDBG_IOC_GET_REGS, &req) < 0)
		return -1;
	*registers_out = req.regs;
	return 0;
}

int lkmdbg_registers_set(struct lkmdbg_session *session, pid_t tid,
			 const struct lkmdbg_regs_arm64 *registers)
{
	struct lkmdbg_thread_regs_request req;

	if (lkmdbg_validate_session(session) < 0 || tid <= 0 || !registers) {
		errno = EINVAL;
		return -1;
	}
	req = (struct lkmdbg_thread_regs_request) {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
		.regs = *registers,
	};
	return ioctl(session->fd, LKMDBG_IOC_SET_REGS, &req);
}

int lkmdbg_vmas_query(struct lkmdbg_session *session,
		      const struct lkmdbg_vma_query_options *options,
		      struct lkmdbg_vma_entry *entries, uint32_t capacity,
		      char *names, uint32_t names_capacity,
		      struct lkmdbg_vma_query_request *result_out)
{
	struct lkmdbg_vma_query_request req;
	int ret;

	if (result_out)
		*result_out = (struct lkmdbg_vma_query_request) { 0 };
	if (lkmdbg_validate_session(session) < 0 || !options || !entries ||
	    capacity == 0 || (!names && names_capacity != 0)) {
		errno = EINVAL;
		return -1;
	}
	req = (struct lkmdbg_vma_query_request) {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.start_addr = options->start_address,
		.entries_addr = (uintptr_t)entries,
		.max_entries = capacity,
		.flags = options->flags,
		.match_flags_mask = options->match_flags_mask,
		.match_flags_value = options->match_flags_value,
		.match_prot_mask = options->match_protection_mask,
		.match_prot_value = options->match_protection_value,
		.names_addr = (uintptr_t)names,
		.names_size = names_capacity,
	};
	ret = ioctl(session->fd, LKMDBG_IOC_QUERY_VMAS, &req);
	if (result_out)
		*result_out = req;
	return ret;
}

int lkmdbg_pages_query(struct lkmdbg_session *session,
		       const struct lkmdbg_page_query_options *options,
		       struct lkmdbg_page_entry *entries, uint32_t capacity,
		       struct lkmdbg_page_query_request *result_out)
{
	struct lkmdbg_page_query_request req;
	int ret;

	if (result_out)
		*result_out = (struct lkmdbg_page_query_request) { 0 };
	if (lkmdbg_validate_session(session) < 0 || !options || !entries ||
	    capacity == 0 || options->length == 0) {
		errno = EINVAL;
		return -1;
	}
	req = (struct lkmdbg_page_query_request) {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.start_addr = options->start_address,
		.length = options->length,
		.entries_addr = (uintptr_t)entries,
		.max_entries = capacity,
		.flags = options->flags,
	};
	ret = ioctl(session->fd, LKMDBG_IOC_QUERY_PAGES, &req);
	if (result_out)
		*result_out = req;
	return ret;
}

static int lkmdbg_stealth_control(struct lkmdbg_session *session,
				  uint32_t flags, bool set,
				  struct lkmdbg_stealth_request *result_out)
{
	struct lkmdbg_stealth_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.flags = flags,
	};
	int ret;

	if (result_out)
		*result_out = (struct lkmdbg_stealth_request) { 0 };
	if (lkmdbg_validate_session(session) < 0)
		return -1;
	ret = ioctl(session->fd,
		    set ? LKMDBG_IOC_SET_STEALTH : LKMDBG_IOC_GET_STEALTH, &req);
	if (result_out)
		*result_out = req;
	return ret;
}

int lkmdbg_stealth_get(struct lkmdbg_session *session,
		       struct lkmdbg_stealth_request *result_out)
{
	if (!result_out) {
		errno = EINVAL;
		return -1;
	}
	return lkmdbg_stealth_control(session, 0, false, result_out);
}

int lkmdbg_stealth_set(struct lkmdbg_session *session, uint32_t flags,
		       struct lkmdbg_stealth_request *result_out)
{
	return lkmdbg_stealth_control(session, flags, true, result_out);
}

int lkmdbg_remote_map_create(
	struct lkmdbg_session *session,
	const struct lkmdbg_remote_map_options *options,
	struct lkmdbg_remote_mapping **mapping_out)
{
	struct lkmdbg_remote_map_request req;
	struct lkmdbg_remote_mapping *mapping;
	int prot = 0;

	if (lkmdbg_validate_session(session) < 0 || !options || !mapping_out ||
	    options->length == 0) {
		errno = EINVAL;
		return -1;
	}
	*mapping_out = NULL;
	if (options->protection & LKMDBG_REMOTE_MAP_PROT_READ)
		prot |= PROT_READ;
	if (options->protection & LKMDBG_REMOTE_MAP_PROT_WRITE)
		prot |= PROT_WRITE;
	if (options->protection & LKMDBG_REMOTE_MAP_PROT_EXEC)
		prot |= PROT_EXEC;
	req = (struct lkmdbg_remote_map_request) {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.remote_addr = options->remote_address,
		.local_addr = options->local_address,
		.length = options->length,
		.prot = options->protection,
		.flags = options->flags,
		.timeout_ms = options->timeout_ms,
		.map_fd = -1,
	};
	if (ioctl(session->fd, LKMDBG_IOC_CREATE_REMOTE_MAP, &req) < 0)
		return -1;
	mapping = calloc(1, sizeof(*mapping));
	if (!mapping)
		goto fail_remove;
	mapping->session_fd = -1;
	mapping->map_fd = req.map_fd;
	mapping->session_fd = dup(session->fd);
	mapping->map_id = req.map_id;
	mapping->length = req.mapped_length;
	if (mapping->session_fd < 0)
		goto fail_mapping;
	mapping->data = mmap(NULL, mapping->length, prot, MAP_SHARED,
			     mapping->map_fd, 0);
	if (mapping->data == MAP_FAILED)
		goto fail_mapping;
	*mapping_out = mapping;
	return 0;

fail_mapping:
	{
		int saved_errno = errno;
		if (mapping->session_fd >= 0)
			close(mapping->session_fd);
		free(mapping);
		errno = saved_errno;
	}
fail_remove:
	{
		int saved_errno = errno;
		struct lkmdbg_remote_map_handle_request remove_req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(remove_req),
			.map_id = req.map_id,
		};
		if (req.map_id != 0)
			(void)ioctl(session->fd, LKMDBG_IOC_REMOVE_REMOTE_MAP,
				    &remove_req);
		if (req.map_fd >= 0)
			close(req.map_fd);
		errno = saved_errno;
	}
	return -1;
}

void *lkmdbg_remote_map_data(const struct lkmdbg_remote_mapping *mapping)
{
	return mapping ? mapping->data : NULL;
}

uint64_t lkmdbg_remote_map_length(const struct lkmdbg_remote_mapping *mapping)
{
	return mapping ? mapping->length : 0;
}

uint64_t lkmdbg_remote_map_id(const struct lkmdbg_remote_mapping *mapping)
{
	return mapping ? mapping->map_id : 0;
}

int lkmdbg_remote_map_fd(const struct lkmdbg_remote_mapping *mapping)
{
	if (!mapping) {
		errno = EINVAL;
		return -1;
	}
	return mapping->map_fd;
}

int lkmdbg_remote_map_destroy(struct lkmdbg_remote_mapping *mapping)
{
	struct lkmdbg_remote_map_handle_request req;
	int ret = 0;
	int saved_errno = 0;

	if (!mapping) {
		errno = EINVAL;
		return -1;
	}
	req = (struct lkmdbg_remote_map_handle_request) {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.map_id = mapping->map_id,
	};
	if (mapping->map_id != 0 &&
	    ioctl(mapping->session_fd, LKMDBG_IOC_REMOVE_REMOTE_MAP, &req) < 0 &&
	    errno != ENOENT && errno != ESRCH) {
		ret = -1;
		saved_errno = errno;
	}
	if (mapping->data && mapping->data != MAP_FAILED)
		munmap(mapping->data, mapping->length);
	if (mapping->map_fd >= 0)
		close(mapping->map_fd);
	if (mapping->session_fd >= 0)
		close(mapping->session_fd);
	free(mapping);
	if (ret < 0)
		errno = saved_errno;
	return ret;
}

int lkmdbg_remote_alloc_create(
	struct lkmdbg_session *session,
	const struct lkmdbg_remote_alloc_options *options,
	struct lkmdbg_remote_allocation **allocation_out)
{
	struct lkmdbg_remote_alloc_request req;
	struct lkmdbg_remote_allocation *allocation;

	if (lkmdbg_validate_session(session) < 0 || !options || !allocation_out ||
	    options->length == 0) {
		errno = EINVAL;
		return -1;
	}
	*allocation_out = NULL;
	req = (struct lkmdbg_remote_alloc_request) {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.remote_addr = options->remote_address,
		.length = options->length,
		.prot = options->protection,
		.flags = options->flags,
	};
	if (ioctl(session->fd, LKMDBG_IOC_CREATE_REMOTE_ALLOC, &req) < 0)
		return -1;
	allocation = calloc(1, sizeof(*allocation));
	if (!allocation)
		goto fail_remove;
	allocation->session_fd = dup(session->fd);
	allocation->alloc_id = req.alloc_id;
	allocation->remote_address = req.remote_addr;
	allocation->length = req.mapped_length;
	if (allocation->session_fd < 0) {
		int saved_errno = errno;
		free(allocation);
		errno = saved_errno;
		goto fail_remove;
	}
	*allocation_out = allocation;
	return 0;

fail_remove:
	{
		int saved_errno = errno;
		struct lkmdbg_remote_alloc_handle_request remove_req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(remove_req),
			.alloc_id = req.alloc_id,
		};
		(void)ioctl(session->fd, LKMDBG_IOC_REMOVE_REMOTE_ALLOC,
			    &remove_req);
		errno = saved_errno;
	}
	return -1;
}

uint64_t lkmdbg_remote_alloc_id(
	const struct lkmdbg_remote_allocation *allocation)
{
	return allocation ? allocation->alloc_id : 0;
}

uintptr_t lkmdbg_remote_alloc_address(
	const struct lkmdbg_remote_allocation *allocation)
{
	return allocation ? allocation->remote_address : 0;
}

uint64_t lkmdbg_remote_alloc_length(
	const struct lkmdbg_remote_allocation *allocation)
{
	return allocation ? allocation->length : 0;
}

int lkmdbg_remote_alloc_destroy(struct lkmdbg_remote_allocation *allocation)
{
	struct lkmdbg_remote_alloc_handle_request req;
	int ret;
	int saved_errno;

	if (!allocation) {
		errno = EINVAL;
		return -1;
	}
	req = (struct lkmdbg_remote_alloc_handle_request) {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.alloc_id = allocation->alloc_id,
	};
	ret = ioctl(allocation->session_fd, LKMDBG_IOC_REMOVE_REMOTE_ALLOC, &req);
	saved_errno = errno;
	close(allocation->session_fd);
	free(allocation);
	if (ret < 0 && saved_errno != ENOENT && saved_errno != ESRCH) {
		errno = saved_errno;
		return -1;
	}
	return 0;
}

int lkmdbg_remote_alloc_query(
	struct lkmdbg_session *session, uint64_t start_id,
	struct lkmdbg_remote_alloc_entry *entries, uint32_t capacity,
	struct lkmdbg_remote_alloc_query_request *result_out)
{
	struct lkmdbg_remote_alloc_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = capacity,
		.start_id = start_id,
	};
	int ret;

	if (result_out)
		*result_out = (struct lkmdbg_remote_alloc_query_request) { 0 };
	if (lkmdbg_validate_session(session) < 0 || !entries || capacity == 0) {
		errno = EINVAL;
		return -1;
	}
	ret = ioctl(session->fd, LKMDBG_IOC_QUERY_REMOTE_ALLOCS, &req);
	if (result_out)
		*result_out = req;
	return ret;
}

int lkmdbg_raw_ioctl(struct lkmdbg_session *session, unsigned long command,
		     void *argument)
{
	if (lkmdbg_validate_session(session) < 0)
		return -1;
	return ioctl(session->fd, command, argument);
}
