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

int lkmdbg_remote_map_create(
	struct lkmdbg_session *session,
	const struct lkmdbg_remote_map_options *options,
	struct lkmdbg_remote_mapping **mapping_out);
void *lkmdbg_remote_map_data(const struct lkmdbg_remote_mapping *mapping);
uint64_t lkmdbg_remote_map_length(const struct lkmdbg_remote_mapping *mapping);
uint64_t lkmdbg_remote_map_id(const struct lkmdbg_remote_mapping *mapping);
int lkmdbg_remote_map_fd(const struct lkmdbg_remote_mapping *mapping);
int lkmdbg_remote_map_destroy(struct lkmdbg_remote_mapping *mapping);

int lkmdbg_raw_ioctl(struct lkmdbg_session *session, unsigned long command,
		     void *argument);

#ifdef __cplusplus
}
#endif

#endif
