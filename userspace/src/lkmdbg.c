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

int lkmdbg_raw_ioctl(struct lkmdbg_session *session, unsigned long command,
		     void *argument)
{
	if (lkmdbg_validate_session(session) < 0)
		return -1;
	return ioctl(session->fd, command, argument);
}
