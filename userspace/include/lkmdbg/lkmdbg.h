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

int lkmdbg_event_read(struct lkmdbg_session *session,
		      struct lkmdbg_event_record *events, size_t capacity,
		      size_t *count_out, int timeout_ms);

int lkmdbg_threads_query(struct lkmdbg_session *session, int32_t start_tid,
			 struct lkmdbg_thread_entry *entries, uint32_t capacity,
			 struct lkmdbg_thread_query_request *result_out);

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

int lkmdbg_raw_ioctl(struct lkmdbg_session *session, unsigned long command,
		     void *argument);

#ifdef __cplusplus
}
#endif

#endif
