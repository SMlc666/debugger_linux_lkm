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

int create_view_region(int session_fd, uintptr_t base_addr, uint64_t length,
		       uint32_t access_mask, uint32_t backend,
		       uint32_t fault_policy, uint32_t sync_policy,
		       uint32_t writeback_policy,
		       struct lkmdbg_view_region_request *reply_out);
int set_view_region_read_backing(int session_fd, uint64_t region_id,
				 const void *buf, uint64_t length,
				 uint32_t backing_type,
				 struct lkmdbg_view_backing_request *reply_out);
int set_view_region_policy(int session_fd, uint64_t region_id, uint32_t backend,
			   uint32_t fault_policy, uint32_t sync_policy,
			   uint32_t writeback_policy,
			   struct lkmdbg_view_policy_request *reply_out);
int remove_view_region(int session_fd, uint64_t region_id,
		       struct lkmdbg_view_region_handle_request *reply_out);
int query_view_regions(int session_fd, uint64_t start_id,
		       struct lkmdbg_view_region_entry *entries,
		       uint32_t max_entries,
		       struct lkmdbg_view_region_query_request *reply_out);

#endif
