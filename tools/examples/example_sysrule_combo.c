#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_control.h"
#include "../driver/bridge_events.h"

static ssize_t read_full_timeout(int fd, void *buf, size_t len, int timeout_ms)
{
	size_t done = 0;

	while (done < len) {
		struct pollfd pfd = {
			.fd = fd,
			.events = POLLIN,
		};
		ssize_t nr;

		if (poll(&pfd, 1, timeout_ms) <= 0)
			return -1;
		nr = read(fd, (char *)buf + done, len - done);
		if (nr < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (nr == 0)
			return -1;
		done += (size_t)nr;
	}

	return (ssize_t)done;
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t nw = write(fd, (const char *)buf + done, len - done);

		if (nw < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)nw;
	}

	return (ssize_t)done;
}

static int run_child(int cmd_fd, int reply_fd)
{
	char cmd;
	int64_t rv;

	for (;;) {
		if (read_full_timeout(cmd_fd, &cmd, sizeof(cmd), 5000) !=
		    (ssize_t)sizeof(cmd))
			return 1;
		if (cmd == 'q')
			return 0;
		if (cmd == 'p') {
			rv = (int64_t)syscall(SYS_getppid);
		} else if (cmd == 'g') {
			rv = (int64_t)syscall(SYS_getpgid, (pid_t)-1);
		} else {
			return 1;
		}
		if (write_full(reply_fd, &rv, sizeof(rv)) != (ssize_t)sizeof(rv))
			return 1;
	}
}

static int child_do_syscall(int cmd_fd, int reply_fd, char cmd, int64_t *rv_out)
{
	if (write_full(cmd_fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
		return -1;
	if (read_full_timeout(reply_fd, rv_out, sizeof(*rv_out), 5000) !=
	    (ssize_t)sizeof(*rv_out))
		return -1;
	return 0;
}

int main(void)
{
	const int64_t forced_ret = -4242;
	int cmd_pipe[2];
	int reply_pipe[2];
	struct lkmdbg_syscall_trace_request trace_req;
	struct lkmdbg_syscall_rule_config_request cfg_reply;
	struct lkmdbg_syscall_rule_entry entry;
	struct lkmdbg_syscall_rule_request rule_reply;
	struct lkmdbg_event_record event;
	pid_t child;
	int session_fd = -1;
	uint32_t control_phases;
	int64_t rv = 0;
	char quit_cmd = 'q';
	int status = 1;

	memset(&trace_req, 0, sizeof(trace_req));
	memset(&cfg_reply, 0, sizeof(cfg_reply));
	memset(&entry, 0, sizeof(entry));
	memset(&rule_reply, 0, sizeof(rule_reply));
	memset(&event, 0, sizeof(event));

	if (pipe(cmd_pipe) != 0 || pipe(reply_pipe) != 0) {
		fprintf(stderr, "example_sysrule_combo: pipe failed errno=%d\n",
			errno);
		return 1;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_sysrule_combo: fork failed errno=%d\n",
			errno);
		return 1;
	}
	if (child == 0) {
		close(cmd_pipe[1]);
		close(reply_pipe[0]);
		_exit(run_child(cmd_pipe[0], reply_pipe[1]));
	}

	close(cmd_pipe[0]);
	close(reply_pipe[1]);

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;
	if (get_syscall_trace(session_fd, &trace_req) < 0)
		goto out;

	if (!(trace_req.flags & LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_CONTROL) ||
	    !(trace_req.supported_phases & LKMDBG_SYSCALL_TRACE_PHASE_ENTER)) {
		printf("example_sysrule_combo: skip backend_control_missing flags=0x%x supported=0x%x\n",
		       trace_req.flags, trace_req.supported_phases);
		status = 0;
		goto out;
	}

	control_phases = LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
	if (trace_req.supported_phases & LKMDBG_SYSCALL_TRACE_PHASE_EXIT)
		control_phases |= LKMDBG_SYSCALL_TRACE_PHASE_EXIT;

	if (set_syscall_rule_config(session_fd, LKMDBG_SYSCALL_RULE_MODE_ENFORCE,
				    LKMDBG_SYSCALL_RULE_EVENT_RAW_AND_RULE,
				    &cfg_reply) < 0)
		goto out;

	if (set_syscall_trace(session_fd, child, SYS_getppid,
			      LKMDBG_SYSCALL_TRACE_MODE_CONTROL,
			      control_phases, &trace_req) < 0)
		goto out;

	if (control_phases & LKMDBG_SYSCALL_TRACE_PHASE_EXIT) {
		memset(&entry, 0, sizeof(entry));
		memset(&rule_reply, 0, sizeof(rule_reply));
		entry.tid = child;
		entry.syscall_nr = SYS_getppid;
		entry.phases = LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
		entry.actions = LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN;
		entry.flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
		entry.priority = 100;
		entry.retval = forced_ret;
		if (upsert_syscall_rule(session_fd, &entry, &rule_reply) < 0)
			goto out;
		if (drain_session_events(session_fd) < 0)
			goto out;
		if (child_do_syscall(cmd_pipe[1], reply_pipe[0], 'p', &rv) < 0)
			goto out;
		if (rv != forced_ret) {
			fprintf(stderr,
				"example_sysrule_combo: setret mismatch got=%lld expected=%lld\n",
				(long long)rv, (long long)forced_ret);
			goto out;
		}
		if (wait_for_session_event(
			    session_fd, LKMDBG_EVENT_TARGET_SYSCALL_RULE, 0,
			    2000, &event) < 0)
			goto out;
		if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_EXIT ||
		    event.value0 != SYS_getppid ||
		    event.value1 != rule_reply.rule.rule_id) {
			fprintf(stderr,
				"example_sysrule_combo: exit event mismatch flags=0x%x nr=%llu rule=%llu\n",
				event.flags, (unsigned long long)event.value0,
				(unsigned long long)event.value1);
			goto out;
		}
	}

	if (set_syscall_trace(session_fd, child, SYS_getpgid,
			      LKMDBG_SYSCALL_TRACE_MODE_CONTROL,
			      LKMDBG_SYSCALL_TRACE_PHASE_ENTER, &trace_req) < 0)
		goto out;

	memset(&entry, 0, sizeof(entry));
	memset(&rule_reply, 0, sizeof(rule_reply));
	entry.tid = child;
	entry.syscall_nr = SYS_getpgid;
	entry.phases = LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
	entry.actions = LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS;
	entry.flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
	entry.priority = 200;
	entry.arg_match_mask = 0x1;
	entry.arg_values[0] = UINT32_MAX;
	entry.arg_value_masks[0] = UINT32_MAX;
	entry.rewrite_mask = 0x1;
	entry.rewrite_args[0] = 0;
	if (upsert_syscall_rule(session_fd, &entry, &rule_reply) < 0)
		goto out;

	if (drain_session_events(session_fd) < 0)
		goto out;
	if (child_do_syscall(cmd_pipe[1], reply_pipe[0], 'g', &rv) < 0)
		goto out;
	if (wait_for_session_event(session_fd, LKMDBG_EVENT_TARGET_SYSCALL_RULE, 0,
				   2000, &event) < 0)
		goto out;
	if (event.flags != LKMDBG_SYSCALL_TRACE_PHASE_ENTER ||
	    event.value0 != SYS_getpgid ||
	    event.value1 != rule_reply.rule.rule_id) {
		fprintf(stderr,
			"example_sysrule_combo: rewrite event mismatch flags=0x%x nr=%llu rule=%llu\n",
			event.flags, (unsigned long long)event.value0,
			(unsigned long long)event.value1);
		goto out;
	}

	status = 0;
	printf("example_sysrule_combo: ok control_phases=0x%x\n",
	       control_phases);

out:
	if (session_fd >= 0) {
		(void)set_syscall_trace(session_fd, 0, -1,
					LKMDBG_SYSCALL_TRACE_MODE_OFF, 0, NULL);
		close(session_fd);
	}
	(void)write_full(cmd_pipe[1], &quit_cmd, sizeof(quit_cmd));
	close(cmd_pipe[1]);
	close(reply_pipe[0]);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
