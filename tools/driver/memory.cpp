#include "memory.hpp"
#include "common.hpp"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

namespace lkmdbg {
namespace driver {
namespace memory {

int xfer_memory(int session_fd, bool write, struct lkmdbg_mem_op *ops,
		uint32_t op_count, struct lkmdbg_mem_request *reply_out)
{
	struct lkmdbg_mem_request req;
	unsigned int cmd = write ? LKMDBG_IOC_WRITE_MEM : LKMDBG_IOC_READ_MEM;

	memset(&req, 0, sizeof(req));
	req.version = LKMDBG_PROTO_VERSION;
	req.size = sizeof(req);
	req.ops_addr = (uintptr_t)ops;
	req.op_count = op_count;

	if (ioctl(session_fd, cmd, &req) < 0) {
		lkmdbg_log_errorf("%s failed: %s",
				  write ? "WRITE_MEM" : "READ_MEM",
				  strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;

	return 0;
}

int read_memory(int session_fd, uintptr_t remote_addr, void *buf, size_t len,
		uint32_t flags)
{
	struct lkmdbg_mem_op op;

	if (len > UINT32_MAX) {
		errno = EINVAL;
		return -1;
	}

	memset(&op, 0, sizeof(op));
	op.remote_addr = remote_addr;
	op.local_addr = (uintptr_t)buf;
	op.length = (uint32_t)len;
	op.flags = flags;

	return xfer_memory(session_fd, false, &op, 1, NULL);
}

int write_memory(int session_fd, uintptr_t remote_addr, const void *buf,
		 size_t len, uint32_t flags)
{
	struct lkmdbg_mem_op op;

	if (len > UINT32_MAX) {
		errno = EINVAL;
		return -1;
	}

	memset(&op, 0, sizeof(op));
	op.remote_addr = remote_addr;
	op.local_addr = (uintptr_t)buf;
	op.length = (uint32_t)len;
	op.flags = flags;

	return xfer_memory(session_fd, true, &op, 1, NULL);
}

} // namespace memory
} // namespace driver
} // namespace lkmdbg
