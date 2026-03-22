#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
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
#define QEMU_HOOK_SELFTEST_STRESS_REPEATS 5U
#define QEMU_PROC_VERSION_REPEATS 5U
#define QEMU_SEQ_READ_REPEATS 3U
#define QEMU_HOOK_SOAK_PROC_VERSION_REPEATS 50U
#define QEMU_HOOK_SOAK_SEQ_READ_REPEATS 50U

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
		    (pos[flag_len] == '\0' || pos[flag_len] == ' '))
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
				   (*endptr == '\0' || *endptr == ' '),
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
