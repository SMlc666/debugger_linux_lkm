#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/reboot.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"

#define STATUS_PATH "/sys/kernel/debug/lkmdbg/status"
#define HOOKS_PATH "/sys/kernel/debug/lkmdbg/hooks"
#define MODULE_PATH "/lkmdbg.ko"
#define MODULE_NAME "lkmdbg"
#define OPEN_SESSION_TOOL "/lkmdbg_open_session"
#define STEALTH_CTL_TOOL "/lkmdbg_stealth_ctl"
#define MEM_TEST_TOOL "/lkmdbg_mem_test"
#define MMU_TEST_TOOL "/lkmdbg_mmu_test"
#define WATCHPOINT_CTRL_TOOL "/qemu_watchpoint_control"
#define INPUT_QUERY_BATCH 16U
#define QEMU_HOOK_SELFTEST_STRESS_REPEATS 5U
#define QEMU_PROC_VERSION_REPEATS 5U
#define QEMU_SEQ_READ_REPEATS 3U
#define QEMU_HOOK_SOAK_PROC_VERSION_REPEATS 50U
#define QEMU_HOOK_SOAK_SEQ_READ_REPEATS 50U
#define QEMU_INPUT_HOST_TIMEOUT_MS 15000
#define QEMU_INPUT_IDLE_TIMEOUT_MS 250
#define QEMU_INPUT_INJECT_TIMEOUT_MS 1000
#define QEMU_INPUT_DEVICES_PATH "/proc/bus/input/devices"

static void qemu_poweroff(void)
{
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
}

static void qemu_fail(const char *fmt, ...)
{
	va_list ap;

	printf("LKMDBG_QEMU_SMOKE_FAIL:");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	qemu_poweroff();
	_exit(1);
}

static void qemu_check(bool cond, const char *fmt, ...)
{
	va_list ap;

	if (cond)
		return;

	printf("LKMDBG_QEMU_SMOKE_FAIL:");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	qemu_poweroff();
	_exit(1);
}

static void qemu_mount_or_fail(const char *source, const char *target,
			       const char *fstype)
{
	if (mount(source, target, fstype, 0, NULL) == 0)
		return;

	qemu_fail("mount_%s errno=%d", target, errno);
}

static void qemu_read_file(const char *path, char *buf, size_t size)
{
	ssize_t nr;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		qemu_fail("open_%s errno=%d", path, errno);

	nr = read(fd, buf, size - 1);
	if (nr < 0) {
		close(fd);
		qemu_fail("read_%s errno=%d", path, errno);
	}

	buf[nr] = '\0';
	close(fd);
}

static const char *qemu_cmdline(void)
{
	static char buf[4096];
	static bool loaded;

	if (!loaded) {
		qemu_read_file("/proc/cmdline", buf, sizeof(buf));
		loaded = true;
	}

	return buf;
}

static bool qemu_cmdline_has_flag(const char *flag)
{
	const char *cmdline = qemu_cmdline();
	size_t flag_len = strlen(flag);
	const char *pos = cmdline;

	while ((pos = strstr(pos, flag)) != NULL) {
		if ((pos == cmdline || pos[-1] == ' ') &&
		    (pos[flag_len] == '\0' || pos[flag_len] == ' ' ||
		     pos[flag_len] == '\n'))
			return true;
		pos += flag_len;
	}

	return false;
}

static unsigned int qemu_cmdline_get_u32(const char *key,
					 unsigned int default_value)
{
	char needle[64];
	const char *cmdline = qemu_cmdline();
	const char *pos = cmdline;
	char *endptr;
	unsigned long value;
	int len;

	len = snprintf(needle, sizeof(needle), "%s=", key);
	qemu_check(len > 0 && (size_t)len < sizeof(needle),
		   "cmdline_key_too_long_%s", key);

	while ((pos = strstr(pos, needle)) != NULL) {
		if (pos == cmdline || pos[-1] == ' ') {
			pos += len;
			errno = 0;
			value = strtoul(pos, &endptr, 0);
			qemu_check(errno == 0 && endptr != pos &&
				   (*endptr == '\0' || *endptr == ' ' ||
				    *endptr == '\n'),
				   "bad_cmdline_u32_%s", key);
			qemu_check(value <= UINT_MAX, "cmdline_u32_overflow_%s", key);
			return (unsigned int)value;
		}
		pos += len;
	}

	return default_value;
}

static void qemu_expect_status_line(const char *needle)
{
	char buf[8192];

	qemu_read_file(STATUS_PATH, buf, sizeof(buf));
	qemu_check(strstr(buf, needle) != NULL, "missing_status_%s", needle);
}

static unsigned long long qemu_read_status_u64(const char *key)
{
	char buf[8192];
	char *line;
	char *endptr;
	unsigned long long value;

	qemu_read_file(STATUS_PATH, buf, sizeof(buf));
	line = strstr(buf, key);
	qemu_check(line != NULL, "missing_status_key_%s", key);

	line += strlen(key);
	errno = 0;
	value = strtoull(line, &endptr, 0);
	qemu_check(errno == 0 && endptr != line, "bad_status_u64_%s", key);
	return value;
}

static void qemu_expect_status_u64_at_least(const char *key,
					    unsigned long long minimum)
{
	unsigned long long value;

	value = qemu_read_status_u64(key);
	qemu_check(value >= minimum, "status_%s_lt_%llu_got_%llu", key, minimum,
		   value);
}

static void qemu_run_tool(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		qemu_fail("fork_%s errno=%d", argv[0], errno);

	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0)
		qemu_fail("waitpid_%s errno=%d", argv[0], errno);

	qemu_check(WIFEXITED(status), "tool_signal_%s status=%d", argv[0], status);
	qemu_check(WEXITSTATUS(status) == 0, "tool_exit_%s status=%d", argv[0],
		   WEXITSTATUS(status));
}

static int qemu_run_tool_status(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		qemu_fail("fork_%s errno=%d", argv[0], errno);

	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0)
		qemu_fail("waitpid_%s errno=%d", argv[0], errno);

	qemu_check(WIFEXITED(status), "tool_signal_%s status=%d", argv[0], status);
	return WEXITSTATUS(status);
}

static void qemu_run_tool_capture(char *const argv[], char *buf, size_t buf_size)
{
	pid_t pid;
	int pipefd[2];
	int status;
	ssize_t nr;

	qemu_check(buf && buf_size > 0, "tool_capture_invalid_buffer");

	if (pipe(pipefd) != 0)
		qemu_fail("pipe_%s errno=%d", argv[0], errno);

	pid = fork();
	if (pid < 0)
		qemu_fail("fork_%s errno=%d", argv[0], errno);

	if (pid == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		execv(argv[0], argv);
		_exit(127);
	}

	close(pipefd[1]);
	nr = read(pipefd[0], buf, buf_size - 1);
	if (nr < 0) {
		close(pipefd[0]);
		qemu_fail("read_tool_%s errno=%d", argv[0], errno);
	}
	buf[nr] = '\0';
	close(pipefd[0]);

	if (waitpid(pid, &status, 0) < 0)
		qemu_fail("waitpid_%s errno=%d", argv[0], errno);

	qemu_check(WIFEXITED(status), "tool_signal_%s status=%d", argv[0], status);
	qemu_check(WEXITSTATUS(status) == 0, "tool_exit_%s status=%d", argv[0],
		   WEXITSTATUS(status));
}

static int qemu_open_session(void)
{
	struct lkmdbg_open_session_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};
	int proc_fd;
	int session_fd;

	proc_fd = open("/proc/version", O_RDONLY | O_CLOEXEC);
	if (proc_fd < 0)
		qemu_fail("open_proc_version_for_session errno=%d", errno);

	session_fd = ioctl(proc_fd, LKMDBG_IOC_OPEN_SESSION, &req);
	if (session_fd < 0) {
		close(proc_fd);
		qemu_fail("open_session_ioctl errno=%d", errno);
	}

	close(proc_fd);
	return session_fd;
}

static bool qemu_bitmap64_test(const __u64 *words, size_t word_count,
			       unsigned int bit)
{
	size_t word_index = bit / 64U;

	if (word_index >= word_count)
		return false;

	return (words[word_index] & (1ULL << (bit % 64U))) != 0;
}

static void qemu_get_input_device_info(int session_fd, __u64 device_id,
				       struct lkmdbg_input_device_info_request *req)
{
	memset(req, 0, sizeof(*req));
	req->version = LKMDBG_PROTO_VERSION;
	req->size = sizeof(*req);
	req->device_id = device_id;

	if (ioctl(session_fd, LKMDBG_IOC_GET_INPUT_DEVICE_INFO, req) == 0)
		return;

	qemu_fail("get_input_device_info errno=%d device=%llu", errno,
		  (unsigned long long)device_id);
}

static int qemu_open_input_channel(int session_fd, __u64 device_id, __u32 flags)
{
	struct lkmdbg_input_channel_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.device_id = device_id,
		.flags = flags,
		.channel_fd = -1,
	};

	if (ioctl(session_fd, LKMDBG_IOC_OPEN_INPUT_CHANNEL, &req) == 0)
		return req.channel_fd;

	qemu_fail("open_input_channel errno=%d device=%llu flags=0x%x", errno,
		  (unsigned long long)device_id, flags);
	return -1;
}

static ssize_t qemu_input_read_events(int channel_fd,
				      struct lkmdbg_input_event *events,
				      size_t max_events, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = channel_fd,
		.events = POLLIN,
	};
	ssize_t nr;

	qemu_check(max_events > 0, "input_read_invalid_max_events");
	if (poll(&pfd, 1, timeout_ms) < 0)
		qemu_fail("poll_input_channel errno=%d", errno);
	if (!(pfd.revents & POLLIN))
		return 0;

	nr = read(channel_fd, events, max_events * sizeof(events[0]));
	if (nr < 0)
		qemu_fail("read_input_channel errno=%d", errno);
	qemu_check((nr % (ssize_t)sizeof(events[0])) == 0,
		   "short_input_event_read=%zd", nr);
	return nr / (ssize_t)sizeof(events[0]);
}

static __u64 qemu_find_keyboard_input_device(
	int session_fd, struct lkmdbg_input_device_info_request *info_out)
{
	struct lkmdbg_input_device_entry entries[INPUT_QUERY_BATCH];
	struct lkmdbg_input_query_request query = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(query),
		.entries_addr = (uintptr_t)entries,
		.max_entries = INPUT_QUERY_BATCH,
	};
	struct lkmdbg_input_device_info_request info;
	bool saw_any = false;

	for (;;) {
		memset(entries, 0, sizeof(entries));
		if (ioctl(session_fd, LKMDBG_IOC_QUERY_INPUT_DEVICES, &query) != 0)
			qemu_fail("query_input_devices errno=%d", errno);

		for (__u32 i = 0; i < query.entries_filled; i++) {
			saw_any = true;
			printf("LKMDBG_QEMU_INPUT_CANDIDATE device_id=%llu name=%s flags=0x%x\n",
			       (unsigned long long)entries[i].device_id,
			       entries[i].name, entries[i].flags);
			fflush(stdout);
			if (!(entries[i].flags & LKMDBG_INPUT_DEVICE_FLAG_HAS_KEYS))
				continue;
			if (!(entries[i].flags & LKMDBG_INPUT_DEVICE_FLAG_CAN_INJECT))
				continue;

			qemu_get_input_device_info(session_fd, entries[i].device_id,
						   &info);
			if (!qemu_bitmap64_test(info.ev_bits,
						sizeof(info.ev_bits) /
							sizeof(info.ev_bits[0]),
						EV_KEY))
				continue;
			if (!qemu_bitmap64_test(info.key_bits,
						sizeof(info.key_bits) /
							sizeof(info.key_bits[0]),
						KEY_A))
				continue;
			if (!(info.supported_channel_flags &
			      LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED))
				continue;

			*info_out = info;
			return entries[i].device_id;
		}

		if (query.done)
			break;
		query.start_id = query.next_id;
	}

	qemu_check(saw_any, "no_input_devices_enumerated");
	qemu_fail("no_viable_keyboard_input_device_missing_KEY_A");
	return 0;
}

static void qemu_wait_for_key_event(int channel_fd, int timeout_ms,
				    bool expect_injected)
{
	struct lkmdbg_input_event events[16];
	int remaining_ms = timeout_ms;

	while (remaining_ms > 0) {
		int slice_ms = remaining_ms > 1000 ? 1000 : remaining_ms;
		ssize_t event_count;

		event_count = qemu_input_read_events(
			channel_fd, events, sizeof(events) / sizeof(events[0]),
			slice_ms);
		if (event_count > 0) {
			ssize_t i;

			for (i = 0; i < event_count; i++) {
				bool injected =
					(events[i].flags & LKMDBG_INPUT_EVENT_FLAG_INJECTED) != 0;

				if (events[i].type != EV_KEY)
					continue;
				if (expect_injected && !injected)
					continue;
				if (!expect_injected && injected)
					continue;
				return;
			}
		}

		remaining_ms -= slice_ms;
	}

	qemu_fail("missing_input_key_event injected=%u", expect_injected ? 1U : 0U);
}

static void qemu_expect_no_input_events(int channel_fd, int timeout_ms)
{
	struct lkmdbg_input_event events[4];
	ssize_t event_count;

	event_count = qemu_input_read_events(
		channel_fd, events, sizeof(events) / sizeof(events[0]),
		timeout_ms);
	qemu_check(event_count == 0, "unexpected_input_events=%zd", event_count);
}

static void qemu_report_input_stealth(void)
{
	char buf[16384];
	char line[256];
	const char *cursor;
	unsigned int handler_hits = 0;
	unsigned int name_hits = 0;
	bool printed_handler_line = false;

	qemu_read_file(QEMU_INPUT_DEVICES_PATH, buf, sizeof(buf));
	cursor = buf;

	while (*cursor) {
		const char *next = strchr(cursor, '\n');
		size_t len;

		if (!next)
			next = cursor + strlen(cursor);
		len = (size_t)(next - cursor);
		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, cursor, len);
		line[len] = '\0';

		if (strstr(line, "Handlers=") && strstr(line, "lkmdbg-input")) {
			handler_hits++;
			if (!printed_handler_line) {
				printf("LKMDBG_QEMU_INPUT_STEALTH first_handler_line=%s\n",
				       line);
				printed_handler_line = true;
			}
		}
		if (strstr(line, "lkmdbg-input"))
			name_hits++;

		cursor = *next ? next + 1 : next;
	}

	printf("LKMDBG_QEMU_INPUT_STEALTH exposed=%u handler_hits=%u name_hits=%u\n",
	       handler_hits ? 1U : 0U, handler_hits, name_hits);
	fflush(stdout);
}

static void qemu_run_input_smoke(void)
{
	struct lkmdbg_input_device_info_request info;
	struct lkmdbg_input_event inject_events[] = {
		{ .type = EV_KEY, .code = KEY_A, .value = 1 },
		{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
		{ .type = EV_KEY, .code = KEY_A, .value = 0 },
		{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
	};
	__u64 device_id;
	int session_fd;
	int host_fd;
	int default_fd;
	int include_fd;
	ssize_t nwritten;

	session_fd = qemu_open_session();
	device_id = qemu_find_keyboard_input_device(session_fd, &info);
	qemu_report_input_stealth();

	host_fd = qemu_open_input_channel(session_fd, device_id, 0);
	printf("LKMDBG_QEMU_INPUT_WAIT_HOST_KEY device_id=%llu name=%s\n",
	       (unsigned long long)device_id, info.entry.name);
	fflush(stdout);
	qemu_wait_for_key_event(host_fd, QEMU_INPUT_HOST_TIMEOUT_MS, false);
	close(host_fd);

	default_fd = qemu_open_input_channel(session_fd, device_id, 0);
	include_fd = qemu_open_input_channel(
		session_fd, device_id, LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED);

	nwritten = write(include_fd, inject_events, sizeof(inject_events));
	qemu_check(nwritten == (ssize_t)sizeof(inject_events),
		   "short_input_inject_write=%zd", nwritten);
	qemu_wait_for_key_event(include_fd, QEMU_INPUT_INJECT_TIMEOUT_MS, true);
	qemu_expect_no_input_events(default_fd, QEMU_INPUT_IDLE_TIMEOUT_MS);
	close(include_fd);
	close(default_fd);
	close(session_fd);
}

static void qemu_drain_one_event(int session_fd)
{
	struct lkmdbg_event_record event;
	ssize_t nr;

	nr = read(session_fd, &event, sizeof(event));
	if (nr < 0)
		qemu_fail("read_session_event errno=%d", errno);
	qemu_check((size_t)nr == sizeof(event), "short_session_event_read=%zd", nr);
}

static void qemu_expect_event_type(int session_fd, unsigned int type)
{
	struct lkmdbg_event_record event;
	struct pollfd pfd = {
		.fd = session_fd,
		.events = POLLIN,
	};
	int poll_ret;
	ssize_t nr;

	for (;;) {
		poll_ret = poll(&pfd, 1, 1000);
		if (poll_ret < 0)
			qemu_fail("poll_session_event errno=%d", errno);
		qemu_check(poll_ret > 0 && (pfd.revents & POLLIN),
			   "missing_session_event_type_%u", type);

		nr = read(session_fd, &event, sizeof(event));
		if (nr < 0)
			qemu_fail("read_session_event errno=%d", errno);
		qemu_check((size_t)nr == sizeof(event), "short_session_event_read=%zd",
			   nr);
		if (event.type == type)
			return;
	}
}

static void qemu_insmod(const char *params)
{
	char param_buf[256];
	int fd;

	fd = open(MODULE_PATH, O_RDONLY);
	if (fd < 0)
		qemu_fail("open_module errno=%d", errno);

	if (params && params[0]) {
		if (snprintf(param_buf, sizeof(param_buf), "enable_debugfs=1 %s",
			     params) >= (int)sizeof(param_buf)) {
			close(fd);
			qemu_fail("module_params_too_long");
		}
		params = param_buf;
	} else {
		params = "enable_debugfs=1";
	}

	if (syscall(SYS_finit_module, fd, params, 0) != 0) {
		close(fd);
		qemu_fail("insmod_%s errno=%d", params, errno);
	}

	close(fd);
}

static void qemu_rmmod(void)
{
	if (syscall(SYS_delete_module, MODULE_NAME, 0) == 0)
		return;

	qemu_fail("rmmod errno=%d", errno);
}

int main(void)
{
	static const struct {
		const char *params;
		const char *status_line;
		bool expect_installed;
		unsigned int repeats;
	} selftests[] = {
		{ "hook_selftest_mode=1", "hook_selftest_enabled=1\n", false, 1 },
		{ "hook_selftest_mode=2", "hook_selftest_exec_pool_ready=1\n", false, 1 },
		{ "hook_selftest_mode=3", "hook_selftest_exec_allocated=1\n", false, 1 },
		{ "hook_selftest_mode=4", "hook_selftest_exec_ready=1\n", false, 1 },
		{ "hook_selftest_mode=5", "hook_selftest_exec_ready=1\n", true,
		  QEMU_HOOK_SELFTEST_STRESS_REPEATS },
		{ "hook_selftest_mode=6", "hook_selftest_actual=", true,
		  QEMU_HOOK_SELFTEST_STRESS_REPEATS },
	};
	char version_buf[4096];
	char report_buf[4096];
	size_t i;
	unsigned int iter;
	char *const open_session_argv[] = { OPEN_SESSION_TOOL, NULL };
	char *const stealth_report_argv[] = { STEALTH_CTL_TOOL, "report", NULL };
	char *const mem_test_argv[] = { MEM_TEST_TOOL, "selftest", NULL };
	char *const mmu_test_argv[] = { MMU_TEST_TOOL, "selftest", NULL };
	char *const watchpoint_ctrl_argv[] = { WATCHPOINT_CTRL_TOOL, NULL };
	int watchpoint_ctrl_status;
	bool hook_soak_only;
	unsigned int selftest_stress_repeats;
	unsigned int proc_version_repeats;
	unsigned int seq_read_repeats;

	mkdir("/dev", 0755);
	mkdir("/proc", 0555);
	mkdir("/sys", 0555);
	mkdir("/sys/kernel", 0555);
	mkdir("/sys/kernel/debug", 0555);

	qemu_mount_or_fail("devtmpfs", "/dev", "devtmpfs");
	qemu_mount_or_fail("proc", "/proc", "proc");
	qemu_mount_or_fail("sysfs", "/sys", "sysfs");
	qemu_mount_or_fail("debugfs", "/sys/kernel/debug", "debugfs");

	hook_soak_only = qemu_cmdline_has_flag("lkmdbg.hook_soak_only");
	selftest_stress_repeats = qemu_cmdline_get_u32(
		"lkmdbg.selftest_stress_repeats", QEMU_HOOK_SELFTEST_STRESS_REPEATS);
	proc_version_repeats = qemu_cmdline_get_u32(
		"lkmdbg.proc_version_repeats",
		hook_soak_only ? QEMU_HOOK_SOAK_PROC_VERSION_REPEATS :
				 QEMU_PROC_VERSION_REPEATS);
	seq_read_repeats = qemu_cmdline_get_u32(
		"lkmdbg.seq_read_repeats",
		hook_soak_only ? QEMU_HOOK_SOAK_SEQ_READ_REPEATS :
				 QEMU_SEQ_READ_REPEATS);

	if (hook_soak_only) {
		printf("LKMDBG_QEMU_HOOK_SOAK_BEGIN proc=%u seq=%u\n",
		       proc_version_repeats, seq_read_repeats);
	} else {
		printf("LKMDBG_QEMU_SMOKE_BEGIN\n");
	}
	fflush(stdout);

	if (!hook_soak_only) {
		watchpoint_ctrl_status = qemu_run_tool_status(watchpoint_ctrl_argv);
		if (watchpoint_ctrl_status == 0) {
			printf("LKMDBG_QEMU_WATCHPOINT_CTRL_OK\n");
		} else if (watchpoint_ctrl_status == 2) {
			printf("LKMDBG_QEMU_WATCHPOINT_CTRL_SKIP\n");
		} else {
			qemu_fail("watchpoint_ctrl_exit_%d", watchpoint_ctrl_status);
		}
		fflush(stdout);
	}

	if (!hook_soak_only) {
		for (i = 0; i < sizeof(selftests) / sizeof(selftests[0]); i++) {
			unsigned int repeats = selftests[i].repeats;

			if (strcmp(selftests[i].params, "hook_selftest_mode=5") == 0 ||
			    strcmp(selftests[i].params, "hook_selftest_mode=6") == 0)
				repeats = selftest_stress_repeats;

			for (iter = 0; iter < repeats; iter++) {
				qemu_insmod(selftests[i].params);
				qemu_expect_status_line(selftests[i].status_line);
				if (selftests[i].expect_installed) {
					qemu_expect_status_line("hook_selftest_installed=1\n");
					qemu_expect_status_u64_at_least("inline_hook_active=", 1);
				} else {
					qemu_expect_status_line("hook_selftest_installed=0\n");
				}
				qemu_rmmod();
			}
		}
	}

	for (iter = 0; iter < proc_version_repeats; iter++) {
		qemu_insmod("hook_proc_version=1");
		qemu_expect_status_u64_at_least("inline_hook_active=", 1);
		qemu_read_file("/proc/version", version_buf, sizeof(version_buf));
		qemu_check(version_buf[0] != '\0', "empty_proc_version");
		qemu_expect_status_line("proc_version_hook_active=1\n");
		qemu_expect_status_u64_at_least("proc_open_successes=", 1);
		if (!hook_soak_only && iter == 0) {
			qemu_run_tool(open_session_argv);
			qemu_run_tool_capture(stealth_report_argv, report_buf,
					      sizeof(report_buf));
			printf("%s", report_buf);
			fflush(stdout);
			qemu_check(strstr(report_buf,
					  "report.stealth.flags=0x1(debugfs)") != NULL,
				   "missing_report_stealth_flags");
			qemu_check(strstr(report_buf,
					  "report.exposure.proc_modules=visible") != NULL,
				   "missing_report_proc_modules");
			qemu_check(strstr(report_buf,
					  "report.exposure.sysfs_module=visible") != NULL,
				   "missing_report_sysfs_module");
			qemu_check(strstr(report_buf,
					  "report.exposure.debugfs_dir=visible") != NULL,
				   "missing_report_debugfs_dir");
			qemu_run_tool(mem_test_argv);
			qemu_run_tool(mmu_test_argv);
			qemu_run_input_smoke();
			printf("LKMDBG_QEMU_HWPOINT_STATUS callback=%llu breakpoint_callback=%llu watchpoint_callback=%llu stop_reads=%llu breakpoint_reads=%llu watchpoint_reads=%llu last_reason=%llu last_type=0x%llx last_addr=0x%llx last_ip=0x%llx\n",
			       qemu_read_status_u64("hwpoint_callback_total="),
			       qemu_read_status_u64("breakpoint_callback_total="),
			       qemu_read_status_u64("watchpoint_callback_total="),
			       qemu_read_status_u64("target_stop_event_read_total="),
			       qemu_read_status_u64("breakpoint_stop_event_read_total="),
			       qemu_read_status_u64("watchpoint_stop_event_read_total="),
			       qemu_read_status_u64("hwpoint_last_reason="),
			       qemu_read_status_u64("hwpoint_last_type="),
			       qemu_read_status_u64("hwpoint_last_addr="),
			       qemu_read_status_u64("hwpoint_last_ip="));
			fflush(stdout);
		}
		qemu_rmmod();
	}

	/*
	 * Keep seq_read covered in the main smoke path, but cap the repeat count
	 * so runtime stays short enough for CI.
	 */
	for (iter = 0; iter < seq_read_repeats; iter++) {
		int session_fd = -1;

		qemu_insmod("hook_proc_version=1 hook_seq_read=1");
		qemu_expect_status_line("seq_read_hook_active=1\n");
		qemu_expect_status_line("proc_version_hook_active=1\n");
		qemu_expect_status_u64_at_least("inline_hook_active=", 1);
		qemu_read_file(HOOKS_PATH, version_buf, sizeof(version_buf));
		qemu_check(strstr(version_buf, "name=seq_read") != NULL,
			   "missing_seq_read_registry");
		qemu_check(strstr(version_buf, "name=proc_version_open") != NULL,
			   "missing_proc_version_open_registry");
		if (!hook_soak_only) {
			session_fd = qemu_open_session();
			qemu_drain_one_event(session_fd);
		}
		qemu_read_file("/proc/version", version_buf, sizeof(version_buf));
		qemu_check(version_buf[0] != '\0', "empty_proc_version_seq_read");
		if (!hook_soak_only)
			qemu_expect_event_type(session_fd, LKMDBG_EVENT_HOOK_HIT);
		qemu_expect_status_u64_at_least("seq_read_hook_hits=", 2);
		qemu_expect_status_u64_at_least("inline_hook_install_total=", 1);
		if (session_fd >= 0)
			close(session_fd);
		qemu_rmmod();
	}

	if (hook_soak_only) {
		printf("LKMDBG_QEMU_HOOK_SOAK_OK\n");
		fflush(stdout);
		qemu_poweroff();
		return 0;
	}

	printf("LKMDBG_QEMU_SMOKE_OK\n");
	fflush(stdout);
	qemu_poweroff();
	return 0;
}
