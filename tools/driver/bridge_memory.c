#include "bridge_memory.h"
#include "common.hpp"

#include <errno.h>
#include <sys/ioctl.h>

int xfer_target_memory(int session_fd, struct lkmdbg_mem_op *ops,
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
		lkmdbg_log_errorf("%s failed: %s",
				  write ? "WRITE_MEM" : "READ_MEM",
				  strerror(errno));
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

int xfer_physical_memory(int session_fd, struct lkmdbg_phys_op *ops,
			 uint32_t op_count, int write,
			 struct lkmdbg_phys_request *reply_out, int verbose)
{
	struct lkmdbg_phys_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.ops_addr = (uintptr_t)ops,
		.op_count = op_count,
	};
	unsigned long cmd = write ? LKMDBG_IOC_WRITE_PHYS : LKMDBG_IOC_READ_PHYS;

	if (ioctl(session_fd, cmd, &req) < 0) {
		lkmdbg_log_errorf("%s failed: %s",
				  write ? "WRITE_PHYS" : "READ_PHYS",
				  strerror(errno));
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

int read_target_memory(int session_fd, uintptr_t remote_addr, void *buf,
		       size_t len, uint32_t *bytes_done_out, int verbose)
{
	return read_target_memory_flags(session_fd, remote_addr, buf, len, 0,
					bytes_done_out, verbose);
}

int read_target_memory_flags(int session_fd, uintptr_t remote_addr, void *buf,
			     size_t len, uint32_t op_flags,
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

int write_target_memory(int session_fd, uintptr_t remote_addr, const void *buf,
			size_t len, uint32_t *bytes_done_out, int verbose)
{
	return write_target_memory_flags(session_fd, remote_addr, buf, len, 0,
					 bytes_done_out, verbose);
}

int write_target_memory_flags(int session_fd, uintptr_t remote_addr,
			      const void *buf, size_t len,
			      uint32_t op_flags, uint32_t *bytes_done_out,
			      int verbose)
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

int read_target_memoryv(int session_fd, struct lkmdbg_mem_op *ops,
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

int write_target_memoryv(int session_fd, struct lkmdbg_mem_op *ops,
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

int read_physical_memory(int session_fd, uint64_t phys_addr, void *buf,
			 size_t len, uint32_t *bytes_done_out, int verbose)
{
	return read_physical_memory_flags(session_fd, phys_addr, buf, len, 0,
					  bytes_done_out, verbose);
}

int read_physical_memory_flags(int session_fd, uint64_t phys_addr, void *buf,
			       size_t len, uint32_t op_flags,
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

int write_physical_memory(int session_fd, uint64_t phys_addr, const void *buf,
			  size_t len, uint32_t *bytes_done_out, int verbose)
{
	return write_physical_memory_flags(session_fd, phys_addr, buf, len, 0,
					   bytes_done_out, verbose);
}

int write_physical_memory_flags(int session_fd, uint64_t phys_addr,
				const void *buf, size_t len,
				uint32_t op_flags, uint32_t *bytes_done_out,
				int verbose)
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
