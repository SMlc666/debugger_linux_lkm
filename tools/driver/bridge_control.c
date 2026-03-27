#include "bridge_control.h"
#include "common.hpp"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

int query_target_threads(int session_fd, int32_t start_tid,
			 struct lkmdbg_thread_entry *entries,
			 uint32_t max_entries,
			 struct lkmdbg_thread_query_request *reply_out)
{
	struct lkmdbg_thread_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_tid = start_tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_THREADS, &req) < 0) {
		lkmdbg_log_errnof("QUERY_THREADS");
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int get_target_regs(int session_fd, pid_t tid,
		    struct lkmdbg_thread_regs_request *reply_out)
{
	struct lkmdbg_thread_regs_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_REGS, &req) < 0) {
		lkmdbg_log_errnof("GET_REGS");
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int set_target_regs(int session_fd,
		    const struct lkmdbg_thread_regs_request *req_in)
{
	struct lkmdbg_thread_regs_request req = *req_in;

	if (ioctl(session_fd, LKMDBG_IOC_SET_REGS, &req) < 0) {
		lkmdbg_log_errnof("SET_REGS");
		return -1;
	}

	return 0;
}

int add_hwpoint_ex(int session_fd, pid_t tid, uint64_t addr, uint32_t type,
		   uint32_t len, uint32_t flags, uint64_t trigger_hit_count,
		   uint32_t action_flags,
		   struct lkmdbg_hwpoint_request *reply_out)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.tid = tid,
		.type = type,
		.len = len,
		.flags = flags,
		.trigger_hit_count = trigger_hit_count,
		.action_flags = action_flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_ADD_HWPOINT, &req) < 0) {
		lkmdbg_log_errorf(
			"ADD_HWPOINT failed: %s tid=%d addr=0x%" PRIx64 " type=0x%x len=%u flags=0x%x trigger=%" PRIu64 " actions=0x%x",
			strerror(errno), tid, addr, type, len, flags,
			(uint64_t)trigger_hit_count, action_flags);
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int add_hwpoint(int session_fd, pid_t tid, uint64_t addr, uint32_t type,
		uint32_t len, uint32_t flags,
		struct lkmdbg_hwpoint_request *reply_out)
{
	return add_hwpoint_ex(session_fd, tid, addr, type, len, flags, 1, 0,
			      reply_out);
}

int add_hwpoint_expect_errno_ex(int session_fd, pid_t tid, uint64_t addr,
				uint32_t type, uint32_t len, uint32_t flags,
				uint64_t trigger_hit_count,
				uint32_t action_flags, int expected_errno)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.tid = tid,
		.type = type,
		.len = len,
		.flags = flags,
		.trigger_hit_count = trigger_hit_count,
		.action_flags = action_flags,
	};

	errno = 0;
	if (ioctl(session_fd, LKMDBG_IOC_ADD_HWPOINT, &req) == 0) {
		lkmdbg_log_errorf(
			"ADD_HWPOINT unexpectedly succeeded addr=0x%" PRIx64 " flags=0x%x",
			(uint64_t)addr, flags);
		return -1;
	}

	if (errno != expected_errno) {
		lkmdbg_log_errorf(
			"ADD_HWPOINT errno=%d expected=%d addr=0x%" PRIx64 " flags=0x%x",
			errno, expected_errno, (uint64_t)addr, flags);
		return -1;
	}

	return 0;
}

int add_hwpoint_expect_errno(int session_fd, pid_t tid, uint64_t addr,
			     uint32_t type, uint32_t len, uint32_t flags,
			     int expected_errno)
{
	return add_hwpoint_expect_errno_ex(session_fd, tid, addr, type, len,
					   flags, 1, 0, expected_errno);
}

int remove_hwpoint(int session_fd, uint64_t id)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_HWPOINT, &req) < 0) {
		lkmdbg_log_errnof("REMOVE_HWPOINT");
		return -1;
	}

	return 0;
}

int rearm_hwpoint(int session_fd, uint64_t id,
		  struct lkmdbg_hwpoint_request *reply_out)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REARM_HWPOINT, &req) < 0) {
		lkmdbg_log_errnof("REARM_HWPOINT");
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int query_hwpoints(int session_fd, uint64_t start_id,
		   struct lkmdbg_hwpoint_entry *entries, uint32_t max_entries,
		   struct lkmdbg_hwpoint_query_request *reply_out)
{
	struct lkmdbg_hwpoint_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_id = start_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_HWPOINTS, &req) < 0) {
		lkmdbg_log_errnof("QUERY_HWPOINTS");
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int single_step_thread(int session_fd, pid_t tid)
{
	struct lkmdbg_single_step_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SINGLE_STEP, &req) < 0) {
		lkmdbg_log_errnof("SINGLE_STEP");
		return -1;
	}

	return 0;
}

int remote_call_thread(int session_fd, pid_t tid, uint64_t target_pc,
		       const uint64_t *args, uint32_t arg_count,
		       struct lkmdbg_remote_call_request *reply_out)
{
	struct lkmdbg_remote_call_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
		.target_pc = target_pc,
		.arg_count = arg_count,
	};

	if (arg_count > LKMDBG_REMOTE_CALL_MAX_ARGS) {
		lkmdbg_log_errorf("REMOTE_CALL arg_count too large: %u", arg_count);
		return -1;
	}

	if (args && arg_count)
		memcpy(req.args, args, arg_count * sizeof(args[0]));

	if (ioctl(session_fd, LKMDBG_IOC_REMOTE_CALL, &req) < 0) {
		lkmdbg_log_errnof("REMOTE_CALL");
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int remote_call_thread_ex(int session_fd, pid_t tid, uint64_t target_pc,
			  const uint64_t *args, uint32_t arg_count,
			  uint32_t flags, uint64_t stack_ptr,
			  uint64_t return_pc, uint64_t x8,
			  struct lkmdbg_remote_call_request *reply_out)
{
	struct lkmdbg_remote_call_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
		.flags = flags,
		.target_pc = target_pc,
		.arg_count = arg_count,
		.stack_ptr = stack_ptr,
		.return_pc = return_pc,
		.x8 = x8,
	};

	if (arg_count > LKMDBG_REMOTE_CALL_MAX_ARGS) {
		lkmdbg_log_errorf("REMOTE_CALL arg_count too large: %u", arg_count);
		return -1;
	}

	if (args && arg_count)
		memcpy(req.args, args, arg_count * sizeof(args[0]));

	if (ioctl(session_fd, LKMDBG_IOC_REMOTE_CALL, &req) < 0) {
		lkmdbg_log_errnof("REMOTE_CALL_EX");
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int remote_thread_create(int session_fd, pid_t tid, uint64_t launcher_pc,
			 uint64_t start_pc, uint64_t start_arg,
			 uint64_t stack_top, uint64_t tls, uint32_t flags,
			 uint32_t timeout_ms,
			 struct lkmdbg_remote_thread_create_request *reply_out)
{
	struct lkmdbg_remote_thread_create_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
		.flags = flags,
		.timeout_ms = timeout_ms,
		.launcher_pc = launcher_pc,
		.start_pc = start_pc,
		.start_arg = start_arg,
		.stack_top = stack_top,
		.tls = tls,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOTE_THREAD_CREATE, &req) < 0) {
		lkmdbg_log_errorf("REMOTE_THREAD_CREATE failed: %s",
				  strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int get_stop_state(int session_fd,
		   struct lkmdbg_stop_query_request *reply_out)
{
	struct lkmdbg_stop_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_STOP_STATE, &req) < 0) {
		lkmdbg_log_errnof("GET_STOP_STATE");
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int set_signal_config(int session_fd, const uint64_t mask_words[2],
		      uint32_t flags,
		      struct lkmdbg_signal_config_request *reply_out)
{
	struct lkmdbg_signal_config_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.flags = flags,
	};

	if (mask_words) {
		req.mask_words[0] = mask_words[0];
		req.mask_words[1] = mask_words[1];
	}

	if (ioctl(session_fd, LKMDBG_IOC_SET_SIGNAL_CONFIG, &req) < 0) {
		lkmdbg_log_errorf("SET_SIGNAL_CONFIG failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int get_signal_config(int session_fd,
		      struct lkmdbg_signal_config_request *reply_out)
{
	struct lkmdbg_signal_config_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_SIGNAL_CONFIG, &req) < 0) {
		lkmdbg_log_errorf("GET_SIGNAL_CONFIG failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int continue_target(int session_fd, uint64_t stop_cookie, uint32_t timeout_ms,
		    uint32_t flags,
		    struct lkmdbg_continue_request *reply_out)
{
	struct lkmdbg_continue_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.flags = flags,
		.timeout_ms = timeout_ms,
		.stop_cookie = stop_cookie,
	};

	if (ioctl(session_fd, LKMDBG_IOC_CONTINUE_TARGET, &req) < 0) {
		lkmdbg_log_errnof("CONTINUE_TARGET");
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int control_target_threads(int session_fd, int thaw, uint32_t timeout_ms,
			   struct lkmdbg_freeze_request *reply_out,
			   int verbose)
{
	struct lkmdbg_freeze_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.timeout_ms = timeout_ms,
	};
	unsigned long cmd =
		thaw ? LKMDBG_IOC_THAW_THREADS : LKMDBG_IOC_FREEZE_THREADS;

	if (ioctl(session_fd, cmd, &req) < 0) {
		lkmdbg_log_errorf("%s failed: %s",
				  thaw ? "THAW_THREADS" : "FREEZE_THREADS",
				  strerror(errno));
		return -1;
	}

	if (verbose) {
		printf("%s total=%u settled=%u parked=%u\n",
		       thaw ? "THAW_THREADS" : "FREEZE_THREADS",
		       req.threads_total, req.threads_settled,
		       req.threads_parked);
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int freeze_target_threads(int session_fd, uint32_t timeout_ms,
			  struct lkmdbg_freeze_request *reply_out,
			  int verbose)
{
	return control_target_threads(session_fd, 0, timeout_ms, reply_out,
				      verbose);
}

int thaw_target_threads(int session_fd, uint32_t timeout_ms,
			struct lkmdbg_freeze_request *reply_out,
			int verbose)
{
	return control_target_threads(session_fd, 1, timeout_ms, reply_out,
				      verbose);
}

int set_syscall_trace(int session_fd, pid_t tid, int syscall_nr, uint32_t mode,
		      uint32_t phases,
		      struct lkmdbg_syscall_trace_request *reply_out)
{
	struct lkmdbg_syscall_trace_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
		.syscall_nr = syscall_nr,
		.mode = mode,
		.phases = phases,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_SYSCALL_TRACE, &req) < 0) {
		lkmdbg_log_errorf("SET_SYSCALL_TRACE failed: %s",
				  strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;

	return 0;
}

int get_syscall_trace(int session_fd,
		      struct lkmdbg_syscall_trace_request *reply_out)
{
	struct lkmdbg_syscall_trace_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.syscall_nr = -1,
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_SYSCALL_TRACE, &req) < 0) {
		lkmdbg_log_errorf("GET_SYSCALL_TRACE failed: %s",
				  strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;

	return 0;
}

int resolve_syscall(int session_fd, uint64_t stop_cookie, uint32_t action,
		    int syscall_nr, const uint64_t *args, int64_t retval,
		    struct lkmdbg_syscall_resolve_request *reply_out)
{
	struct lkmdbg_syscall_resolve_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.stop_cookie = stop_cookie,
		.action = action,
		.syscall_nr = syscall_nr,
		.retval = retval,
	};

	if (args)
		memcpy(req.args, args, sizeof(req.args));

	if (ioctl(session_fd, LKMDBG_IOC_RESOLVE_SYSCALL, &req) < 0) {
		lkmdbg_log_errnof("RESOLVE_SYSCALL");
		return -1;
	}

	if (reply_out)
		*reply_out = req;

	return 0;
}

int set_stealth(int session_fd, uint32_t flags,
		struct lkmdbg_stealth_request *reply_out)
{
	struct lkmdbg_stealth_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.flags = flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_STEALTH, &req) < 0) {
		lkmdbg_log_errnof("SET_STEALTH");
		return -1;
	}

	if (reply_out)
		*reply_out = req;

	return 0;
}

int get_stealth(int session_fd, struct lkmdbg_stealth_request *reply_out)
{
	struct lkmdbg_stealth_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_STEALTH, &req) < 0) {
		lkmdbg_log_errnof("GET_STEALTH");
		return -1;
	}

	if (reply_out)
		*reply_out = req;

	return 0;
}
