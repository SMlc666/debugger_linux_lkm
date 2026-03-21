#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define STATUS_PATH "/sys/kernel/debug/lkmdbg/status"
#define MODULE_PATH "/lkmdbg.ko"
#define MODULE_NAME "lkmdbg"

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
	char version_buf[4096];

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

	qemu_insmod("hook_selftest_mode=1");
	qemu_expect_status_line("hook_selftest_enabled=1\n");
	qemu_expect_status_line("hook_selftest_installed=0\n");
	qemu_rmmod();

	qemu_insmod("hook_proc_version=1");
	qemu_read_file("/proc/version", version_buf, sizeof(version_buf));
	qemu_check(version_buf[0] != '\0', "empty_proc_version");
	qemu_expect_status_line("proc_version_hook_active=1\n");
	qemu_rmmod();

	printf("LKMDBG_QEMU_SMOKE_OK\n");
	fflush(stdout);
	qemu_poweroff();
	return 0;
}
