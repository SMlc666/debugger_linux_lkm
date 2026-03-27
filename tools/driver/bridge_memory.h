#ifndef LKMDBG_DRIVER_BRIDGE_MEMORY_H
#define LKMDBG_DRIVER_BRIDGE_MEMORY_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/lkmdbg_ioctl.h"

int xfer_target_memory(int session_fd, struct lkmdbg_mem_op *ops,
		       uint32_t op_count, int write,
		       struct lkmdbg_mem_request *reply_out, int verbose);
int xfer_physical_memory(int session_fd, struct lkmdbg_phys_op *ops,
			 uint32_t op_count, int write,
			 struct lkmdbg_phys_request *reply_out, int verbose);

int read_target_memory(int session_fd, uintptr_t remote_addr, void *buf,
		       size_t len, uint32_t *bytes_done_out, int verbose);
int read_target_memory_flags(int session_fd, uintptr_t remote_addr, void *buf,
			     size_t len, uint32_t op_flags,
			     uint32_t *bytes_done_out, int verbose);
int write_target_memory(int session_fd, uintptr_t remote_addr, const void *buf,
			size_t len, uint32_t *bytes_done_out, int verbose);
int write_target_memory_flags(int session_fd, uintptr_t remote_addr,
			      const void *buf, size_t len,
			      uint32_t op_flags, uint32_t *bytes_done_out,
			      int verbose);
int read_target_memoryv(int session_fd, struct lkmdbg_mem_op *ops,
			uint32_t op_count, uint32_t *ops_done_out,
			uint64_t *bytes_done_out, int verbose);
int write_target_memoryv(int session_fd, struct lkmdbg_mem_op *ops,
			 uint32_t op_count, uint32_t *ops_done_out,
			 uint64_t *bytes_done_out, int verbose);

int read_physical_memory(int session_fd, uint64_t phys_addr, void *buf,
			 size_t len, uint32_t *bytes_done_out, int verbose);
int read_physical_memory_flags(int session_fd, uint64_t phys_addr, void *buf,
			       size_t len, uint32_t op_flags,
			       uint32_t *bytes_done_out, int verbose);
int write_physical_memory(int session_fd, uint64_t phys_addr, const void *buf,
			  size_t len, uint32_t *bytes_done_out, int verbose);
int write_physical_memory_flags(int session_fd, uint64_t phys_addr,
				const void *buf, size_t len,
				uint32_t op_flags, uint32_t *bytes_done_out,
				int verbose);

#endif
