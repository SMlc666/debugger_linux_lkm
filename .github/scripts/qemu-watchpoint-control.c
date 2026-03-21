#define _GNU_SOURCE

#include <errno.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static long qemu_perf_event_open(struct perf_event_attr *attr, pid_t pid,
				 int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

int main(void)
{
	struct perf_event_attr attr;
	struct pollfd pfd;
	volatile uint64_t watched = 0;
	volatile uint64_t *ptr = &watched;
	uint64_t count = 0;
	ssize_t nr;
	int fd;
	int poll_ret;

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_BREAKPOINT;
	attr.size = sizeof(attr);
	attr.bp_type = HW_BREAKPOINT_W;
	attr.bp_addr = (unsigned long)ptr;
	attr.bp_len = HW_BREAKPOINT_LEN_8;
	attr.sample_period = 1;
	attr.disabled = 1;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;

	fd = (int)qemu_perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0) {
		printf("watchpoint-ctrl: perf_event_open unavailable errno=%d\n",
		       errno);
		return 2;
	}

	if (ioctl(fd, PERF_EVENT_IOC_RESET, 0) != 0) {
		printf("watchpoint-ctrl: reset failed errno=%d\n", errno);
		close(fd);
		return 1;
	}

	if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
		printf("watchpoint-ctrl: enable failed errno=%d\n", errno);
		close(fd);
		return 2;
	}

	*ptr = 1;
	asm volatile("" ::: "memory");
	*ptr = 2;
	asm volatile("" ::: "memory");

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	poll_ret = poll(&pfd, 1, 100);
	if (poll_ret < 0) {
		printf("watchpoint-ctrl: poll failed errno=%d\n", errno);
		close(fd);
		return 1;
	}

	if (ioctl(fd, PERF_EVENT_IOC_DISABLE, 0) != 0) {
		printf("watchpoint-ctrl: disable failed errno=%d\n", errno);
		close(fd);
		return 1;
	}

	nr = read(fd, &count, sizeof(count));
	if (nr != (ssize_t)sizeof(count)) {
		printf("watchpoint-ctrl: read failed nr=%zd errno=%d\n", nr, errno);
		close(fd);
		return 1;
	}

	close(fd);
	printf("watchpoint-ctrl: count=%llu value=%llu poll=%d revents=0x%x\n",
	       (unsigned long long)count, (unsigned long long)watched, poll_ret,
	       pfd.revents);
	if (count == 0) {
		printf("watchpoint-ctrl: no watchpoint hit observed\n");
		return 2;
	}

	printf("watchpoint-ctrl: watchpoint hit observed\n");
	return 0;
}
