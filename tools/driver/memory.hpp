#ifndef LKMDBG_DRIVER_MEMORY_HPP
#define LKMDBG_DRIVER_MEMORY_HPP

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/lkmdbg_ioctl.h"

namespace lkmdbg {
namespace driver {
namespace memory {

int xfer_memory(int session_fd, bool write, struct lkmdbg_mem_op *ops,
		uint32_t op_count, struct lkmdbg_mem_request *reply_out);
int read_memory(int session_fd, uintptr_t remote_addr, void *buf, size_t len,
		uint32_t flags);
int write_memory(int session_fd, uintptr_t remote_addr, const void *buf,
		 size_t len, uint32_t flags);

} // namespace memory
} // namespace driver
} // namespace lkmdbg

#endif
