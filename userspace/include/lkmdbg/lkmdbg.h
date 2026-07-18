#ifndef LKMDBG_USERSPACE_H
#define LKMDBG_USERSPACE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "lkmdbg_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LKMDBG_SDK_VERSION_MAJOR 0U
#define LKMDBG_SDK_VERSION_MINOR 1U
#define LKMDBG_SDK_VERSION_PATCH 0U

struct lkmdbg_session;
struct lkmdbg_remote_mapping;
struct lkmdbg_remote_allocation;
struct lkmdbg_view_region;

struct lkmdbg_transfer_result {
	uint32_t operations_done;
	uint64_t bytes_done;
};

struct lkmdbg_remote_map_options {
	uintptr_t remote_address;
	uintptr_t local_address;
	uint64_t length;
	uint32_t protection;
	uint32_t flags;
	uint32_t timeout_ms;
};

struct lkmdbg_remote_alloc_options {
	uintptr_t remote_address;
	uint64_t length;
	uint32_t protection;
	uint32_t flags;
};

struct lkmdbg_vma_query_options {
	uint64_t start_address;
	uint32_t flags;
	uint32_t match_flags_mask;
	uint32_t match_flags_value;
	uint32_t match_protection_mask;
	uint32_t match_protection_value;
};

struct lkmdbg_page_query_options {
	uint64_t start_address;
	uint64_t length;
	uint32_t flags;
};

struct lkmdbg_view_region_options {
	uintptr_t base_address;
	uint64_t length;
	uint32_t access_mask;
	uint32_t flags;
	uint32_t backend;
	uint32_t fault_policy;
	uint32_t sync_policy;
	uint32_t writeback_policy;
};

int lkmdbg_session_open(struct lkmdbg_session **session_out);
int lkmdbg_session_adopt_fd(int fd, int take_ownership,
			    struct lkmdbg_session **session_out);
void lkmdbg_session_close(struct lkmdbg_session *session);
int lkmdbg_session_fd(const struct lkmdbg_session *session);
int lkmdbg_session_set_target(struct lkmdbg_session *session, pid_t tgid,
			      pid_t tid);
int lkmdbg_session_get_status(struct lkmdbg_session *session,
			      struct lkmdbg_status_reply *status_out);
int lkmdbg_session_reset(struct lkmdbg_session *session);

int lkmdbg_memory_read(struct lkmdbg_session *session, uintptr_t remote_address,
		       void *buffer, size_t length, uint32_t flags,
		       struct lkmdbg_transfer_result *result_out);
int lkmdbg_memory_write(struct lkmdbg_session *session,
			uintptr_t remote_address, const void *buffer,
			size_t length, uint32_t flags,
			struct lkmdbg_transfer_result *result_out);
int lkmdbg_memory_readv(struct lkmdbg_session *session,
			struct lkmdbg_mem_op *operations, uint32_t operation_count,
			struct lkmdbg_transfer_result *result_out);
int lkmdbg_memory_writev(struct lkmdbg_session *session,
			 struct lkmdbg_mem_op *operations,
			 uint32_t operation_count,
			 struct lkmdbg_transfer_result *result_out);
int lkmdbg_physical_readv(struct lkmdbg_session *session,
			  struct lkmdbg_phys_op *operations,
			  uint32_t operation_count,
			  struct lkmdbg_transfer_result *result_out);
int lkmdbg_physical_writev(struct lkmdbg_session *session,
			   struct lkmdbg_phys_op *operations,
			   uint32_t operation_count,
			   struct lkmdbg_transfer_result *result_out);
int lkmdbg_physical_read(struct lkmdbg_session *session,
			 uint64_t physical_address, void *buffer, size_t length,
			 uint32_t flags,
			 struct lkmdbg_transfer_result *result_out);
int lkmdbg_physical_write(struct lkmdbg_session *session,
			  uint64_t physical_address, const void *buffer,
			  size_t length, uint32_t flags,
			  struct lkmdbg_transfer_result *result_out);
int lkmdbg_virtual_to_physical(struct lkmdbg_session *session,
			       uintptr_t virtual_address, size_t length,
			       struct lkmdbg_phys_op *translation_out);

int lkmdbg_event_read(struct lkmdbg_session *session,
		      struct lkmdbg_event_record *events, size_t capacity,
		      size_t *count_out, int timeout_ms);

int lkmdbg_threads_query(struct lkmdbg_session *session, int32_t start_tid,
			 struct lkmdbg_thread_entry *entries, uint32_t capacity,
			 struct lkmdbg_thread_query_request *result_out);
int lkmdbg_threads_freeze(struct lkmdbg_session *session, uint32_t timeout_ms,
			  struct lkmdbg_freeze_request *result_out);
int lkmdbg_threads_thaw(struct lkmdbg_session *session, uint32_t timeout_ms,
			struct lkmdbg_freeze_request *result_out);
int lkmdbg_registers_get(struct lkmdbg_session *session, pid_t tid,
			 struct lkmdbg_regs_arm64 *registers_out);
int lkmdbg_registers_set(struct lkmdbg_session *session, pid_t tid,
			 const struct lkmdbg_regs_arm64 *registers);
int lkmdbg_vmas_query(struct lkmdbg_session *session,
		      const struct lkmdbg_vma_query_options *options,
		      struct lkmdbg_vma_entry *entries, uint32_t capacity,
		      char *names, uint32_t names_capacity,
		      struct lkmdbg_vma_query_request *result_out);
int lkmdbg_pages_query(struct lkmdbg_session *session,
		       const struct lkmdbg_page_query_options *options,
		       struct lkmdbg_page_entry *entries, uint32_t capacity,
		       struct lkmdbg_page_query_request *result_out);
int lkmdbg_stealth_get(struct lkmdbg_session *session,
		       struct lkmdbg_stealth_request *result_out);
int lkmdbg_stealth_set(struct lkmdbg_session *session, uint32_t flags,
		       struct lkmdbg_stealth_request *result_out);

int lkmdbg_remote_map_create(
	struct lkmdbg_session *session,
	const struct lkmdbg_remote_map_options *options,
	struct lkmdbg_remote_mapping **mapping_out);
void *lkmdbg_remote_map_data(const struct lkmdbg_remote_mapping *mapping);
uint64_t lkmdbg_remote_map_length(const struct lkmdbg_remote_mapping *mapping);
uint64_t lkmdbg_remote_map_id(const struct lkmdbg_remote_mapping *mapping);
int lkmdbg_remote_map_fd(const struct lkmdbg_remote_mapping *mapping);
int lkmdbg_remote_map_destroy(struct lkmdbg_remote_mapping *mapping);

int lkmdbg_remote_alloc_create(
	struct lkmdbg_session *session,
	const struct lkmdbg_remote_alloc_options *options,
	struct lkmdbg_remote_allocation **allocation_out);
uint64_t lkmdbg_remote_alloc_id(
	const struct lkmdbg_remote_allocation *allocation);
uintptr_t lkmdbg_remote_alloc_address(
	const struct lkmdbg_remote_allocation *allocation);
uint64_t lkmdbg_remote_alloc_length(
	const struct lkmdbg_remote_allocation *allocation);
int lkmdbg_remote_alloc_destroy(struct lkmdbg_remote_allocation *allocation);
int lkmdbg_remote_alloc_query(
	struct lkmdbg_session *session, uint64_t start_id,
	struct lkmdbg_remote_alloc_entry *entries, uint32_t capacity,
	struct lkmdbg_remote_alloc_query_request *result_out);

int lkmdbg_view_region_create(
	struct lkmdbg_session *session,
	const struct lkmdbg_view_region_options *options,
	struct lkmdbg_view_region **region_out);
uint64_t lkmdbg_view_region_id(const struct lkmdbg_view_region *region);
int lkmdbg_view_region_set_backing(
	struct lkmdbg_view_region *region, uint32_t view_kind,
	const void *source, uint64_t source_length, uint32_t backing_type,
	struct lkmdbg_view_backing_request *result_out);
int lkmdbg_view_regions_query(
	struct lkmdbg_session *session, uint64_t start_id,
	struct lkmdbg_view_region_entry *entries, uint32_t capacity,
	struct lkmdbg_view_region_query_request *result_out);
int lkmdbg_view_region_destroy(struct lkmdbg_view_region *region);

int lkmdbg_raw_ioctl(struct lkmdbg_session *session, unsigned long command,
		     void *argument);

#ifdef __cplusplus
}
#endif

#endif
