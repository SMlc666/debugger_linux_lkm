#ifndef LKMDBG_DRIVER_BRIDGE_CONTROL_H
#define LKMDBG_DRIVER_BRIDGE_CONTROL_H

#include <inttypes.h>
#include <stdint.h>
#include <sys/types.h>

#include "../../include/lkmdbg_ioctl.h"

int query_target_threads(int session_fd, int32_t start_tid,
			 struct lkmdbg_thread_entry *entries,
			 uint32_t max_entries,
			 struct lkmdbg_thread_query_request *reply_out);
int get_target_regs(int session_fd, pid_t tid,
		    struct lkmdbg_thread_regs_request *reply_out);
int set_target_regs(int session_fd,
		    const struct lkmdbg_thread_regs_request *req_in);

int add_hwpoint_ex(int session_fd, pid_t tid, uint64_t addr, uint32_t type,
		   uint32_t len, uint32_t flags, uint64_t trigger_hit_count,
		   uint32_t action_flags,
		   struct lkmdbg_hwpoint_request *reply_out);
int add_hwpoint(int session_fd, pid_t tid, uint64_t addr, uint32_t type,
		uint32_t len, uint32_t flags,
		struct lkmdbg_hwpoint_request *reply_out);
int add_hwpoint_expect_errno_ex(int session_fd, pid_t tid, uint64_t addr,
				uint32_t type, uint32_t len, uint32_t flags,
				uint64_t trigger_hit_count,
				uint32_t action_flags, int expected_errno);
int add_hwpoint_expect_errno(int session_fd, pid_t tid, uint64_t addr,
			     uint32_t type, uint32_t len, uint32_t flags,
			     int expected_errno);
int remove_hwpoint(int session_fd, uint64_t id);
int rearm_hwpoint(int session_fd, uint64_t id,
		  struct lkmdbg_hwpoint_request *reply_out);
int query_hwpoints(int session_fd, uint64_t start_id,
		   struct lkmdbg_hwpoint_entry *entries, uint32_t max_entries,
		   struct lkmdbg_hwpoint_query_request *reply_out);
int single_step_thread(int session_fd, pid_t tid);

int remote_call_thread(int session_fd, pid_t tid, uint64_t target_pc,
		       const uint64_t *args, uint32_t arg_count,
		       struct lkmdbg_remote_call_request *reply_out);
int remote_call_thread_ex(int session_fd, pid_t tid, uint64_t target_pc,
			  const uint64_t *args, uint32_t arg_count,
			  uint32_t flags, uint64_t stack_ptr,
			  uint64_t return_pc, uint64_t x8,
			  struct lkmdbg_remote_call_request *reply_out);
int remote_thread_create(int session_fd, pid_t tid, uint64_t launcher_pc,
			 uint64_t start_pc, uint64_t start_arg,
			 uint64_t stack_top, uint64_t tls, uint32_t flags,
			 uint32_t timeout_ms,
			 struct lkmdbg_remote_thread_create_request *reply_out);

int get_stop_state(int session_fd,
		   struct lkmdbg_stop_query_request *reply_out);
int set_signal_config(int session_fd, const uint64_t mask_words[2],
		      uint32_t flags,
		      struct lkmdbg_signal_config_request *reply_out);
int get_signal_config(int session_fd,
		      struct lkmdbg_signal_config_request *reply_out);
int continue_target(int session_fd, uint64_t stop_cookie, uint32_t timeout_ms,
		    uint32_t flags,
		    struct lkmdbg_continue_request *reply_out);
int control_target_threads(int session_fd, int thaw, uint32_t timeout_ms,
			   struct lkmdbg_freeze_request *reply_out,
			   int verbose);
int freeze_target_threads(int session_fd, uint32_t timeout_ms,
			  struct lkmdbg_freeze_request *reply_out,
			  int verbose);
int thaw_target_threads(int session_fd, uint32_t timeout_ms,
			struct lkmdbg_freeze_request *reply_out,
			int verbose);

int set_syscall_trace(int session_fd, pid_t tid, int syscall_nr, uint32_t mode,
		      uint32_t phases,
		      struct lkmdbg_syscall_trace_request *reply_out);
int get_syscall_trace(int session_fd,
		      struct lkmdbg_syscall_trace_request *reply_out);
int set_syscall_rule_config(int session_fd, uint32_t mode, uint32_t event_policy,
			    struct lkmdbg_syscall_rule_config_request *reply_out);
int get_syscall_rule_config(int session_fd,
			    struct lkmdbg_syscall_rule_config_request *reply_out);
int upsert_syscall_rule(int session_fd,
			const struct lkmdbg_syscall_rule_entry *entry_in,
			struct lkmdbg_syscall_rule_request *reply_out);
int remove_syscall_rule(int session_fd, uint64_t rule_id);
int query_syscall_rules(int session_fd, uint64_t start_id,
			struct lkmdbg_syscall_rule_entry *entries,
			uint32_t max_entries,
			struct lkmdbg_syscall_rule_query_request *reply_out);
int resolve_syscall(int session_fd, uint64_t stop_cookie, uint32_t action,
		    int syscall_nr, const uint64_t *args, int64_t retval,
		    struct lkmdbg_syscall_resolve_request *reply_out);

int set_stealth(int session_fd, uint32_t flags,
		struct lkmdbg_stealth_request *reply_out);
int get_stealth(int session_fd, struct lkmdbg_stealth_request *reply_out);

#endif
