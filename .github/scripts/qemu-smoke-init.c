#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
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
#define MEM_TEST_TOOL "/lkmdbg_mem_test"

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
	int fd;

	fd = open(MODULE_PATH, O_RDONLY);
	if (fd < 0)
		qemu_fail("open_module errno=%d", errno);

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
		{ "hook_selftest_mode=5", "hook_selftest_exec_ready=1\n", true, 3 },
		{ "hook_selftest_mode=6", "hook_selftest_actual=", true, 3 },
	};
	char version_buf[4096];
	size_t i;
	unsigned int iter;
	char *const open_session_argv[] = { OPEN_SESSION_TOOL, NULL };
	char *const mem_test_argv[] = { MEM_TEST_TOOL, "selftest", NULL };

	mkdir("/dev", 0755);
	mkdir("/proc", 0555);
	mkdir("/sys", 0555);
	mkdir("/sys/kernel", 0555);
	mkdir("/sys/kernel/debug", 0555);

	qemu_mount_or_fail("devtmpfs", "/dev", "devtmpfs");
	qemu_mount_or_fail("proc", "/proc", "proc");
	qemu_mount_or_fail("sysfs", "/sys", "sysfs");
	qemu_mount_or_fail("debugfs", "/sys/kernel/debug", "debugfs");

	printf("LKMDBG_QEMU_SMOKE_BEGIN\n");
	fflush(stdout);

	for (i = 0; i < sizeof(selftests) / sizeof(selftests[0]); i++) {
		for (iter = 0; iter < selftests[i].repeats; iter++) {
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

	qemu_insmod("hook_proc_version=1");
	qemu_expect_status_u64_at_least("inline_hook_active=", 1);
	qemu_read_file("/proc/version", version_buf, sizeof(version_buf));
	qemu_check(version_buf[0] != '\0', "empty_proc_version");
	qemu_expect_status_line("proc_version_hook_active=1\n");
	qemu_expect_status_u64_at_least("proc_open_successes=", 1);
	qemu_run_tool(open_session_argv);
	qemu_run_tool(mem_test_argv);
	qemu_rmmod();

	for (iter = 0; iter < 5; iter++) {
		int session_fd;

		qemu_insmod("hook_proc_version=1 hook_seq_read=1");
		qemu_expect_status_line("seq_read_hook_active=1\n");
		qemu_expect_status_line("proc_version_hook_active=1\n");
		qemu_expect_status_u64_at_least("inline_hook_active=", 1);
		qemu_read_file(HOOKS_PATH, version_buf, sizeof(version_buf));
		qemu_check(strstr(version_buf, "name=seq_read") != NULL,
			   "missing_seq_read_registry");
		qemu_check(strstr(version_buf, "name=proc_version_open") != NULL,
			   "missing_proc_version_open_registry");
		session_fd = qemu_open_session();
		qemu_drain_one_event(session_fd);
		qemu_read_file("/proc/version", version_buf, sizeof(version_buf));
		qemu_check(version_buf[0] != '\0', "empty_proc_version_seq_read");
		qemu_expect_event_type(session_fd, LKMDBG_EVENT_HOOK_HIT);
		qemu_expect_status_u64_at_least("seq_read_hook_hits=", 2);
		qemu_expect_status_u64_at_least("inline_hook_install_total=", 1);
		close(session_fd);
		qemu_rmmod();
	}

	printf("LKMDBG_QEMU_SMOKE_OK\n");
	fflush(stdout);
	qemu_poweroff();
	return 0;
}
