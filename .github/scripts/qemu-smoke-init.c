#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
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
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"

#define STATUS_PATH "/sys/kernel/debug/lkmdbg/status"
#define HOOKS_PATH "/sys/kernel/debug/lkmdbg/hooks"
#define MODULE_SYSFS_DIR "/sys/module/lkmdbg"
#define MODULE_PATH "/lkmdbg.ko"
#define MODULE_NAME "lkmdbg"
#define OPEN_SESSION_TOOL "/lkmdbg_open_session"
#define STEALTH_CTL_TOOL "/lkmdbg_stealth_ctl"
#define MEM_TEST_TOOL "/lkmdbg_mem_test"
#define MMU_TEST_TOOL "/lkmdbg_mmu_test"
#define WATCHPOINT_CTRL_TOOL "/qemu_watchpoint_control"
#define EXAMPLE_SESSION_STATUS_TOOL "/lkmdbg_example_session_status"
#define EXAMPLE_MEM_RW_TOOL "/lkmdbg_example_mem_rw"
#define EXAMPLE_THREADS_QUERY_TOOL "/lkmdbg_example_threads_query"
#define EXAMPLE_REGS_FP_TOOL "/lkmdbg_example_regs_fp"
#define EXAMPLE_STEALTH_ROUNDTRIP_TOOL "/lkmdbg_example_stealth_roundtrip"
#define EXAMPLE_SYSRULE_COMBO_TOOL "/lkmdbg_example_sysrule_combo"
#define EXAMPLE_VMA_PAGE_QUERY_TOOL "/lkmdbg_example_vma_page_query"
#define EXAMPLE_REMOTE_ALLOC_RW_TOOL "/lkmdbg_example_remote_alloc_rw"
#define EXAMPLE_PHYS_TRANSLATE_READ_TOOL "/lkmdbg_example_phys_translate_read"
#define EXAMPLE_PERF_BASELINE_TOOL "/lkmdbg_example_perf_baseline"
#define EXAMPLE_VIEW_EXTERNAL_READ_TOOL "/lkmdbg_example_view_external_read"
#define EXAMPLE_VIEW_WXSHADOW_EXEC_TOOL "/lkmdbg_example_view_wxshadow_exec"
#define INPUT_QUERY_BATCH 16U
#define QEMU_HOOK_SELFTEST_STRESS_REPEATS 5U
#define QEMU_PROC_VERSION_REPEATS 5U
#define QEMU_SEQ_READ_REPEATS 3U
#define QEMU_HOOK_SOAK_PROC_VERSION_REPEATS 50U
#define QEMU_HOOK_SOAK_SEQ_READ_REPEATS 50U
#define QEMU_INPUT_HOST_TIMEOUT_MS 15000
#define QEMU_INPUT_IDLE_TIMEOUT_MS 250
#define QEMU_INPUT_INJECT_TIMEOUT_MS 1000
#define QEMU_PROC_EXPOSURE_SAMPLE_US 20000
#define QEMU_PROC_EXPOSURE_LIFETIME_US 700000
#define QEMU_INPUT_DEVICES_PATH "/proc/bus/input/devices"

#define QEMU_SMOKE_CLUSTER_WATCHPOINT 0x00000001U
#define QEMU_SMOKE_CLUSTER_HOOK_SELFTEST 0x00000002U
#define QEMU_SMOKE_CLUSTER_TRANSPORT 0x00000004U
#define QEMU_SMOKE_CLUSTER_INPUT 0x00000008U
#define QEMU_SMOKE_CLUSTER_PROC_EXPOSURE 0x00000010U
#define QEMU_SMOKE_CLUSTER_SEQ_READ 0x00000020U
#define QEMU_SMOKE_CLUSTER_BEHAVIOR 0x00000040U
#define QEMU_SMOKE_CLUSTER_ALL                                             \
	(QEMU_SMOKE_CLUSTER_WATCHPOINT | QEMU_SMOKE_CLUSTER_HOOK_SELFTEST | \
	 QEMU_SMOKE_CLUSTER_TRANSPORT | QEMU_SMOKE_CLUSTER_INPUT |         \
	 QEMU_SMOKE_CLUSTER_PROC_EXPOSURE | QEMU_SMOKE_CLUSTER_SEQ_READ)

#define QEMU_BEHAVIOR_PERF_SAMPLES 3U
#define QEMU_BEHAVIOR_HEARTBEAT_POLL_US 1000U
#define QEMU_BEHAVIOR_HEARTBEAT_WINDOW_MS 1200U
#define QEMU_BEHAVIOR_SYSCALL_SAMPLE_MS 300U
#define QEMU_BEHAVIOR_ALLOC_P99_RATIO_LIMIT 3.5
#define QEMU_BEHAVIOR_ALLOC_P99_MARGIN_US 200.0
#define QEMU_BEHAVIOR_SYSCALL_RATIO_LIMIT 20.0
#define QEMU_BEHAVIOR_SYSCALL_MARGIN_NS 200000.0
#define QEMU_BEHAVIOR_HEARTBEAT_MAX_US 1800000ULL
#define QEMU_BEHAVIOR_HEARTBEAT_OVER_100MS_MAX 12U
#define QEMU_BEHAVIOR_MINFLT_DELTA_MAX 200000ULL
#define QEMU_BEHAVIOR_MAJFLT_DELTA_MAX 256ULL
#define QEMU_BEHAVIOR_DEBUG_TIMEOUT_MS 5000U
#define QEMU_BEHAVIOR_DISTURBANCE_RETRIES 3U

struct qemu_hook_selftest_case {
	const char *params;
	const char *status_line;
	bool expect_installed;
	unsigned int repeats;
};

static const struct qemu_hook_selftest_case qemu_hook_selftests[] = {
	{ "hook_selftest_mode=1", "hook_selftest_enabled=1\n", false, 1 },
	{ "hook_selftest_mode=2", "hook_selftest_exec_pool_ready=1\n", false, 1 },
	{ "hook_selftest_mode=3", "hook_selftest_exec_allocated=1\n", false, 1 },
	{ "hook_selftest_mode=4", "hook_selftest_exec_ready=1\n", false, 1 },
	{ "hook_selftest_mode=5", "hook_selftest_exec_ready=1\n", true,
	  QEMU_HOOK_SELFTEST_STRESS_REPEATS },
	{ "hook_selftest_mode=6", "hook_selftest_actual=", true,
	  QEMU_HOOK_SELFTEST_STRESS_REPEATS },
};

struct qemu_smoke_cluster_spec {
	const char *name;
	unsigned int bit;
};

static const struct qemu_smoke_cluster_spec qemu_smoke_cluster_specs[] = {
	{ "watchpoint", QEMU_SMOKE_CLUSTER_WATCHPOINT },
	{ "selftest", QEMU_SMOKE_CLUSTER_HOOK_SELFTEST },
	{ "transport", QEMU_SMOKE_CLUSTER_TRANSPORT },
	{ "input", QEMU_SMOKE_CLUSTER_INPUT },
	{ "exposure", QEMU_SMOKE_CLUSTER_PROC_EXPOSURE },
	{ "seqread", QEMU_SMOKE_CLUSTER_SEQ_READ },
	{ "behavior", QEMU_SMOKE_CLUSTER_BEHAVIOR },
};

struct qemu_behavior_shared {
	volatile __u64 heartbeat_seq;
	volatile __u64 heartbeat_ns;
	volatile __u64 syscall_total_ns;
	volatile __u64 syscall_iters;
	volatile __u64 hot_value;
	volatile __u32 ready;
	volatile __u32 stop;
};

struct qemu_behavior_heartbeat_stats {
	__u64 sample_count;
	__u64 p50_us;
	__u64 p95_us;
	__u64 p99_us;
	__u64 max_us;
	__u64 over_50ms;
	__u64 over_100ms;
};

struct qemu_behavior_perf_stats {
	double alloc_p50_us;
	double alloc_p95_us;
	double alloc_p99_us;
};

struct qemu_behavior_debug_result {
	__s32 ret_code;
	__u32 freeze_rounds;
	__u32 step_rounds;
	__u32 hwpoint_rounds;
	__u32 hwpoint_supported;
	__u32 mmu_supported;
	__u32 mmu_armed;
};

static int qemu_open_session(void);
static void qemu_drain_one_event(int session_fd);
static void qemu_read_event_blocking(int session_fd,
				     struct lkmdbg_event_record *event_out);
static bool qemu_status_debugfs_available = true;
static bool qemu_status_debugfs_reported;
static bool qemu_hooks_debugfs_available = true;
static bool qemu_hooks_debugfs_reported;

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

static bool qemu_try_read_file_errno(const char *path, char *buf, size_t size,
				     int *err_out)
{
	ssize_t nr;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (err_out)
			*err_out = errno;
		return false;
	}

	nr = read(fd, buf, size - 1);
	if (nr < 0) {
		int saved_errno = errno;

		close(fd);
		qemu_fail("read_%s errno=%d", path, saved_errno);
	}

	buf[nr] = '\0';
	close(fd);
	if (err_out)
		*err_out = 0;
	return true;
}

struct qemu_proc_exposure_stats {
	unsigned int samples;
	unsigned int enum_hits;
	unsigned int direct_hits;
	unsigned int comm_hits;
	unsigned int cmdline_hits;
};

static bool qemu_str_is_decimal(const char *s)
{
	size_t i;

	if (!s || !s[0])
		return false;

	for (i = 0; s[i]; i++) {
		if (!isdigit((unsigned char)s[i]))
			return false;
	}

	return true;
}

static bool qemu_try_read_file(const char *path, char *buf, size_t size)
{
	int fd;
	ssize_t nr;

	if (!buf || size == 0)
		return false;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;

	nr = read(fd, buf, size - 1);
	close(fd);
	if (nr < 0)
		return false;

	buf[nr] = '\0';
	return true;
}

static __u64 qemu_now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		qemu_fail("clock_gettime errno=%d", errno);

	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

static int qemu_u64_cmp(const void *a, const void *b)
{
	const __u64 va = *(const __u64 *)a;
	const __u64 vb = *(const __u64 *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

static int qemu_double_cmp(const void *a, const void *b)
{
	const double va = *(const double *)a;
	const double vb = *(const double *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

static __u64 qemu_percentile_u64(const __u64 *values, size_t count,
				 unsigned int percentile)
{
	size_t index;

	qemu_check(values != NULL && count > 0, "percentile_u64_empty");
	if (percentile > 100U)
		percentile = 100U;
	if (count == 1)
		return values[0];

	index = ((count - 1U) * (size_t)percentile + 99U) / 100U;
	if (index >= count)
		index = count - 1U;
	return values[index];
}

static double qemu_percentile_double(const double *values, size_t count,
				     unsigned int percentile)
{
	size_t index;

	qemu_check(values != NULL && count > 0, "percentile_double_empty");
	if (percentile > 100U)
		percentile = 100U;
	if (count == 1)
		return values[0];

	index = ((count - 1U) * (size_t)percentile + 99U) / 100U;
	if (index >= count)
		index = count - 1U;
	return values[index];
}

static void qemu_sample_proc_exposure(pid_t pid, const char *comm_needle,
				      const char *cmdline_needle,
				      struct qemu_proc_exposure_stats *stats)
{
	char path[64];
	char text[256];
	bool enum_hit = false;
	bool direct_hit = false;
	bool comm_hit = false;
	bool cmdline_hit = false;
	DIR *dir;
	struct dirent *ent;

	if (!stats || pid <= 0)
		return;

	dir = opendir("/proc");
	if (dir) {
		while ((ent = readdir(dir)) != NULL) {
			long v;
			char *endp;

			if (!qemu_str_is_decimal(ent->d_name))
				continue;
			v = strtol(ent->d_name, &endp, 10);
			if (*endp != '\0')
				continue;
			if ((pid_t)v == pid) {
				enum_hit = true;
				break;
			}
		}
		closedir(dir);
	}

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	if (qemu_try_read_file(path, text, sizeof(text)))
		direct_hit = true;

	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	if (qemu_try_read_file(path, text, sizeof(text)) && comm_needle &&
	    strstr(text, comm_needle) != NULL)
		comm_hit = true;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	if (qemu_try_read_file(path, text, sizeof(text)) && cmdline_needle &&
	    strstr(text, cmdline_needle) != NULL)
		cmdline_hit = true;

	stats->samples++;
	if (enum_hit)
		stats->enum_hits++;
	if (direct_hit)
		stats->direct_hits++;
	if (comm_hit)
		stats->comm_hits++;
	if (cmdline_hit)
		stats->cmdline_hits++;
}

static bool qemu_run_proc_exposure_phase(const char *phase,
					 const char *comm_name,
					 bool enable_owner_hide,
					 struct qemu_proc_exposure_stats *stats)
{
	int ready_pipe[2];
	int stop_pipe[2];
	pid_t child;
	int status = 0;
	unsigned char ready_code = 0xff;
	bool hide_enabled = false;
	unsigned int sample_rounds;
	unsigned int round;

	if (pipe(ready_pipe) != 0)
		qemu_fail("pipe_proc_exposure_%s errno=%d", phase, errno);
	if (pipe(stop_pipe) != 0) {
		close(ready_pipe[0]);
		close(ready_pipe[1]);
		qemu_fail("pipe_proc_exposure_stop_%s errno=%d", phase, errno);
	}

	child = fork();
	if (child < 0)
		qemu_fail("fork_proc_exposure_%s errno=%d", phase, errno);

	if (child == 0) {
		int session_fd = -1;
		struct lkmdbg_stealth_request req = {
			.version = LKMDBG_PROTO_VERSION,
			.size = sizeof(req),
		};
		uint32_t old_flags = 0;

		close(ready_pipe[0]);
		close(stop_pipe[1]);
		if (comm_name)
			prctl(PR_SET_NAME, comm_name, 0, 0, 0);

		if (enable_owner_hide) {
			session_fd = qemu_open_session();
			if (ioctl(session_fd, LKMDBG_IOC_GET_STEALTH, &req) != 0) {
				ready_code = 1;
				(void)write(ready_pipe[1], &ready_code, 1);
				_exit(3);
			}
			old_flags = req.flags;
			if (!(req.supported_flags &
			      LKMDBG_STEALTH_FLAG_OWNER_PROC_HIDDEN)) {
				ready_code = 2;
				(void)write(ready_pipe[1], &ready_code, 1);
				close(session_fd);
				_exit(0);
			}
			req.flags = old_flags | LKMDBG_STEALTH_FLAG_OWNER_PROC_HIDDEN;
			if (ioctl(session_fd, LKMDBG_IOC_SET_STEALTH, &req) != 0) {
				ready_code = 1;
				(void)write(ready_pipe[1], &ready_code, 1);
				close(session_fd);
				_exit(4);
			}
			hide_enabled = true;
		}

		ready_code = 0;
		(void)write(ready_pipe[1], &ready_code, 1);
		for (;;) {
			unsigned char stop_code;
			ssize_t nr;

			nr = read(stop_pipe[0], &stop_code, 1);
			if (nr > 0 || nr == 0)
				break;
			if (errno == EINTR)
				continue;
			break;
		}

		if (hide_enabled) {
			req.flags = old_flags;
			(void)ioctl(session_fd, LKMDBG_IOC_SET_STEALTH, &req);
			close(session_fd);
		}
		close(ready_pipe[1]);
		close(stop_pipe[0]);
		_exit(0);
	}

	close(ready_pipe[1]);
	close(stop_pipe[0]);
	if (read(ready_pipe[0], &ready_code, 1) != 1) {
		close(ready_pipe[0]);
		close(stop_pipe[1]);
		qemu_fail("read_proc_exposure_ready_%s errno=%d", phase, errno);
	}
	close(ready_pipe[0]);

	if (ready_code == 2) {
		close(stop_pipe[1]);
		if (waitpid(child, &status, 0) < 0)
			qemu_fail("waitpid_proc_exposure_%s errno=%d", phase, errno);
		qemu_check(WIFEXITED(status), "proc_exposure_signal_%s status=%d",
			   phase, status);
		return false;
	}
	qemu_check(ready_code == 0, "proc_exposure_setup_fail_%s code=%u", phase,
		   (unsigned int)ready_code);

	memset(stats, 0, sizeof(*stats));
	sample_rounds = QEMU_PROC_EXPOSURE_LIFETIME_US /
			QEMU_PROC_EXPOSURE_SAMPLE_US;
	if (sample_rounds == 0)
		sample_rounds = 1;
	for (round = 0; round < sample_rounds; round++) {
		qemu_sample_proc_exposure(child, comm_name, "/init", stats);
		if (round + 1 < sample_rounds)
			usleep(QEMU_PROC_EXPOSURE_SAMPLE_US);
	}

	close(stop_pipe[1]);
	if (waitpid(child, &status, 0) < 0)
		qemu_fail("waitpid_proc_exposure_%s errno=%d", phase, errno);
	qemu_check(WIFEXITED(status), "proc_exposure_child_signal_%s status=%d",
		   phase, status);
	qemu_check(WEXITSTATUS(status) == 0,
		   "proc_exposure_child_exit_%s status=%d", phase,
		   WEXITSTATUS(status));
	return true;
}

static void qemu_report_user_proc_exposure(void)
{
	struct qemu_proc_exposure_stats baseline_stats;
	struct qemu_proc_exposure_stats hidden_stats;
	bool hidden_ran;

	qemu_run_proc_exposure_phase("baseline", "lkmdbg-base", false,
				     &baseline_stats);
	printf("LKMDBG_QEMU_PROC_EXPOSURE phase=baseline samples=%u enum_hits=%u direct_hits=%u comm_hits=%u cmdline_hits=%u\n",
	       baseline_stats.samples, baseline_stats.enum_hits,
	       baseline_stats.direct_hits, baseline_stats.comm_hits,
	       baseline_stats.cmdline_hits);
	fflush(stdout);
	qemu_check(baseline_stats.samples > 0, "proc_exposure_no_samples_baseline");
	qemu_check(baseline_stats.enum_hits > 0 || baseline_stats.direct_hits > 0,
		   "proc_exposure_baseline_not_visible");

	hidden_ran = qemu_run_proc_exposure_phase("ownerhide", "lkmdbg-hide",
						  true, &hidden_stats);
	if (!hidden_ran) {
		printf("LKMDBG_QEMU_PROC_EXPOSURE phase=ownerhide skip=unsupported\n");
		fflush(stdout);
		return;
	}

	printf("LKMDBG_QEMU_PROC_EXPOSURE phase=ownerhide samples=%u enum_hits=%u direct_hits=%u comm_hits=%u cmdline_hits=%u\n",
	       hidden_stats.samples, hidden_stats.enum_hits,
	       hidden_stats.direct_hits, hidden_stats.comm_hits,
	       hidden_stats.cmdline_hits);
	fflush(stdout);

	qemu_check(hidden_stats.enum_hits == 0,
		   "proc_exposure_ownerhide_enum_hits=%u",
		   hidden_stats.enum_hits);
	qemu_check(hidden_stats.direct_hits == 0,
		   "proc_exposure_ownerhide_direct_hits=%u",
		   hidden_stats.direct_hits);
	qemu_check(hidden_stats.comm_hits == 0,
		   "proc_exposure_ownerhide_comm_hits=%u",
		   hidden_stats.comm_hits);
	qemu_check(hidden_stats.cmdline_hits == 0,
		   "proc_exposure_ownerhide_cmdline_hits=%u",
		   hidden_stats.cmdline_hits);
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

static bool qemu_cmdline_get_value(const char *key, char *buf, size_t buf_size)
{
	char needle[64];
	const char *cmdline = qemu_cmdline();
	const char *pos = cmdline;
	const char *end;
	int len;

	qemu_check(buf && buf_size > 0, "cmdline_value_buffer_invalid_%s", key);
	len = snprintf(needle, sizeof(needle), "%s=", key);
	qemu_check(len > 0 && (size_t)len < sizeof(needle),
		   "cmdline_value_key_too_long_%s", key);

	while ((pos = strstr(pos, needle)) != NULL) {
		size_t value_len;

		if (!(pos == cmdline || pos[-1] == ' ')) {
			pos += len;
			continue;
		}

		pos += len;
		end = pos;
		while (*end && *end != ' ' && *end != '\n')
			end++;

		value_len = (size_t)(end - pos);
		qemu_check(value_len + 1U <= buf_size,
			   "cmdline_value_too_long_%s", key);
		memcpy(buf, pos, value_len);
		buf[value_len] = '\0';
		return true;
	}

	return false;
}

static unsigned int qemu_default_smoke_clusters(bool hook_soak_only)
{
	if (hook_soak_only)
		return QEMU_SMOKE_CLUSTER_SEQ_READ;

	return QEMU_SMOKE_CLUSTER_ALL;
}

static void qemu_append_name(char *buf, size_t buf_size, const char *name)
{
	size_t len;

	if (!buf || !buf_size || !name || !name[0])
		return;

	len = strlen(buf);
	if (len >= buf_size - 1)
		return;

	if (len) {
		snprintf(buf + len, buf_size - len, ",%s", name);
		return;
	}

	snprintf(buf, buf_size, "%s", name);
}

static const char *qemu_describe_smoke_clusters(unsigned int mask, char *buf,
						size_t buf_size)
{
	size_t i;

	qemu_check(buf && buf_size > 0, "smoke_cluster_desc_buffer_invalid");
	buf[0] = '\0';
	for (i = 0; i < sizeof(qemu_smoke_cluster_specs) /
			    sizeof(qemu_smoke_cluster_specs[0]);
	     i++) {
		if (!(mask & qemu_smoke_cluster_specs[i].bit))
			continue;
		qemu_append_name(buf, buf_size, qemu_smoke_cluster_specs[i].name);
	}

	if (!buf[0])
		snprintf(buf, buf_size, "none");
	return buf;
}

static unsigned int qemu_parse_smoke_clusters(bool hook_soak_only)
{
	char value[128];
	unsigned int mask = 0;
	char *cursor;
	char *token;

	if (!qemu_cmdline_get_value("lkmdbg.smoke_clusters", value, sizeof(value)))
		return qemu_default_smoke_clusters(hook_soak_only);
	if (!value[0] || strcmp(value, "default") == 0)
		return qemu_default_smoke_clusters(hook_soak_only);
	if (strcmp(value, "all") == 0)
		return QEMU_SMOKE_CLUSTER_ALL;
	if (strcmp(value, "none") == 0)
		return 0;

	cursor = value;
	while ((token = strsep(&cursor, ",")) != NULL) {
		size_t i;
		bool found = false;

		if (!token[0])
			continue;
		for (i = 0; i < sizeof(qemu_smoke_cluster_specs) /
				    sizeof(qemu_smoke_cluster_specs[0]);
		     i++) {
			if (strcmp(token, qemu_smoke_cluster_specs[i].name) != 0)
				continue;
			mask |= qemu_smoke_cluster_specs[i].bit;
			found = true;
			break;
		}
		qemu_check(found, "unknown_smoke_cluster_%s", token);
	}

	return mask;
}

static void qemu_cluster_begin(const char *name)
{
	printf("LKMDBG_QEMU_CLUSTER_BEGIN name=%s\n", name);
	fflush(stdout);
}

static void qemu_cluster_ok(const char *name)
{
	printf("LKMDBG_QEMU_CLUSTER_OK name=%s\n", name);
	fflush(stdout);
}

static void qemu_cluster_skip(const char *name, const char *reason)
{
	printf("LKMDBG_QEMU_CLUSTER_SKIP name=%s reason=%s\n", name, reason);
	fflush(stdout);
}

static void qemu_expect_status_line(const char *needle)
{
	char buf[8192];
	int err;

	if (!qemu_status_debugfs_available)
		return;
	if (!qemu_try_read_file_errno(STATUS_PATH, buf, sizeof(buf), &err)) {
		if (err == ENOENT) {
			qemu_status_debugfs_available = false;
			if (!qemu_status_debugfs_reported) {
				printf("LKMDBG_QEMU_STATUS_UNAVAILABLE errno=%d\n",
				       err);
				fflush(stdout);
				qemu_status_debugfs_reported = true;
			}
			return;
		}
		qemu_fail("open_%s errno=%d", STATUS_PATH, err);
	}
	qemu_check(strstr(buf, needle) != NULL, "missing_status_%s", needle);
}

static void qemu_report_module_sysfs_once(void)
{
	static bool reported;
	static const char *const paths[] = {
		MODULE_SYSFS_DIR "/initstate",
		MODULE_SYSFS_DIR "/coresize",
		MODULE_SYSFS_DIR "/initsize",
		MODULE_SYSFS_DIR "/taint",
		MODULE_SYSFS_DIR "/refcnt",
		MODULE_SYSFS_DIR "/sections/.text",
		MODULE_SYSFS_DIR "/sections/.init.text",
	};
	char buf[256];
	size_t i;
	int err;

	if (reported)
		return;
	reported = true;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (qemu_try_read_file_errno(paths[i], buf, sizeof(buf), &err)) {
			printf("LKMDBG_QEMU_MODULE_SYSFS path=%s value=%s\n",
			       paths[i], buf);
		} else {
			printf("LKMDBG_QEMU_MODULE_SYSFS path=%s errno=%d\n",
			       paths[i], err);
		}
	}
	fflush(stdout);
}

static unsigned long long qemu_read_status_u64(const char *key)
{
	char buf[8192];
	char *line;
	char *endptr;
	unsigned long long value;
	int err;

	if (!qemu_status_debugfs_available)
		return 0;
	if (!qemu_try_read_file_errno(STATUS_PATH, buf, sizeof(buf), &err)) {
		if (err == ENOENT) {
			qemu_status_debugfs_available = false;
			if (!qemu_status_debugfs_reported) {
				printf("LKMDBG_QEMU_STATUS_UNAVAILABLE errno=%d\n",
				       err);
				fflush(stdout);
				qemu_status_debugfs_reported = true;
			}
			qemu_report_module_sysfs_once();
			return 0;
		}
		qemu_fail("open_%s errno=%d", STATUS_PATH, err);
	}
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

	if (!qemu_status_debugfs_available)
		return;
	value = qemu_read_status_u64(key);
	if (!qemu_status_debugfs_available)
		return;
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

static void qemu_expect_bootstrap_open_session_errno(
	const struct lkmdbg_open_session_request *req, int expected_errno,
	const char *label)
{
	int proc_fd;
	int session_fd;
	int saved_errno;

	proc_fd = open("/proc/version", O_RDONLY | O_CLOEXEC);
	if (proc_fd < 0)
		qemu_fail("open_proc_version_%s errno=%d", label, errno);

	errno = 0;
	session_fd = ioctl(proc_fd, LKMDBG_IOC_OPEN_SESSION, req);
	saved_errno = errno;
	close(proc_fd);

	qemu_check(session_fd < 0, "bootstrap_%s_unexpected_success", label);
	qemu_check(saved_errno == expected_errno,
		   "bootstrap_%s_errno=%d expected=%d", label, saved_errno,
		   expected_errno);
}

static void qemu_expect_session_ioctl_errno(int session_fd, unsigned int cmd,
					    void *argp, int expected_errno,
					    const char *label)
{
	int ret;
	int saved_errno;

	errno = 0;
	ret = ioctl(session_fd, cmd, argp);
	saved_errno = errno;
	qemu_check(ret < 0, "session_%s_unexpected_success", label);
	qemu_check(saved_errno == expected_errno,
		   "session_%s_errno=%d expected=%d", label, saved_errno,
		   expected_errno);
}

static void qemu_expect_session_read_errno(int session_fd, size_t len,
					   int expected_errno,
					   const char *label)
{
	char buf[sizeof(struct lkmdbg_event_record)];
	ssize_t nr;
	int saved_errno;

	qemu_check(len > 0 && len <= sizeof(buf), "session_read_len_invalid_%s",
		   label);
	errno = 0;
	nr = read(session_fd, buf, len);
	saved_errno = errno;
	qemu_check(nr < 0, "session_read_%s_unexpected_success", label);
	qemu_check(saved_errno == expected_errno,
		   "session_read_%s_errno=%d expected=%d", label, saved_errno,
		   expected_errno);
}

static void qemu_run_transport_negative_tests(void)
{
	struct lkmdbg_event_config_request event_cfg = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(event_cfg),
	};
	struct lkmdbg_open_session_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};
	struct lkmdbg_status_reply status = { 0 };
	struct lkmdbg_stop_query_request stop_req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(stop_req),
	};
	struct lkmdbg_freeze_request freeze_req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(freeze_req),
	};
	struct lkmdbg_signal_config_request signal_cfg = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(signal_cfg),
	};
	struct lkmdbg_syscall_trace_request syscall_trace = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(syscall_trace),
	};
	struct lkmdbg_remote_thread_create_request remote_thread_req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(remote_thread_req),
	};
	struct lkmdbg_target_request target_req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(target_req),
	};
	struct lkmdbg_event_record event;
	int session_fd;

	qemu_cluster_begin("transport-negative");

	req.version++;
	qemu_expect_bootstrap_open_session_errno(&req, ENOTTY, "bad_version");
	req.version = LKMDBG_PROTO_VERSION;
	req.size -= 4;
	qemu_expect_bootstrap_open_session_errno(&req, ENOTTY, "bad_size");

	session_fd = qemu_open_session();
	qemu_expect_session_read_errno(session_fd,
				       sizeof(struct lkmdbg_event_record) - 1U,
				       EINVAL, "short_event_read");
	event_cfg.size -= 8U;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_GET_EVENT_CONFIG,
					&event_cfg, EINVAL,
					"bad_get_event_config_size");
	event_cfg.size = sizeof(event_cfg);
	event_cfg.flags = 1U;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_GET_EVENT_CONFIG,
					&event_cfg, EINVAL,
					"bad_get_event_config_flags");
	stop_req.size -= 8U;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_GET_STOP_STATE,
					&stop_req, EINVAL,
					"bad_get_stop_state_size");
	stop_req.size = sizeof(stop_req);
	stop_req.flags = 1U;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_GET_STOP_STATE,
					&stop_req, EINVAL,
					"bad_get_stop_state_flags");
	freeze_req.size -= 8U;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_FREEZE_THREADS,
					&freeze_req, EINVAL,
					"bad_freeze_threads_size");
	freeze_req.size = sizeof(freeze_req);
	freeze_req.flags = 1U;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_THAW_THREADS,
					&freeze_req, EINVAL,
					"bad_thaw_threads_flags");
	signal_cfg.flags = ~LKMDBG_SIGNAL_CONFIG_STOP;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_SET_SIGNAL_CONFIG,
					&signal_cfg, EINVAL,
					"bad_set_signal_config_flags");
	syscall_trace.mode = LKMDBG_SYSCALL_TRACE_MODE_EVENT;
	syscall_trace.phases = 0;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_SET_SYSCALL_TRACE,
					&syscall_trace, EINVAL,
					"bad_set_syscall_trace_phases");
	remote_thread_req.flags = ~LKMDBG_REMOTE_THREAD_CREATE_FLAG_SET_TLS;
	qemu_expect_session_ioctl_errno(session_fd,
					LKMDBG_IOC_REMOTE_THREAD_CREATE,
					&remote_thread_req, EINVAL,
					"bad_remote_thread_create_flags");
	target_req.tgid = 0;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_SET_TARGET,
					&target_req, EINVAL,
					"bad_set_target_zero_tgid");
	target_req.tgid = 1;
	target_req.tid = 2;
	qemu_expect_session_ioctl_errno(session_fd, LKMDBG_IOC_SET_TARGET,
					&target_req, ESRCH,
					"bad_set_target_mismatched_tid");
	if (ioctl(session_fd, LKMDBG_IOC_GET_STATUS, &status) != 0)
		qemu_fail("get_status_before_reset errno=%d", errno);
	qemu_check(status.session_ioctl_calls >= 1,
		   "status_before_reset_ioctl_calls=%llu",
		   (unsigned long long)status.session_ioctl_calls);
	qemu_drain_one_event(session_fd);
	if (ioctl(session_fd, LKMDBG_IOC_RESET_SESSION) != 0)
		qemu_fail("reset_session errno=%d", errno);
	qemu_read_event_blocking(session_fd, &event);
	qemu_check(event.type == LKMDBG_EVENT_SESSION_RESET,
		   "unexpected_reset_event_type=%u", event.type);
	qemu_check(event.value0 >= 1, "reset_event_old_calls=%llu",
		   (unsigned long long)event.value0);
	if (ioctl(session_fd, LKMDBG_IOC_GET_STATUS, &status) != 0)
		qemu_fail("get_status_after_reset errno=%d", errno);
	qemu_check(status.session_ioctl_calls == 1,
		   "status_after_reset_ioctl_calls=%llu",
		   (unsigned long long)status.session_ioctl_calls);
	qemu_expect_session_ioctl_errno(session_fd,
					_IO(LKMDBG_IOC_MAGIC, 0x7f), NULL,
					ENOTTY, "unknown_ioctl");
	close(session_fd);

	qemu_cluster_ok("transport-negative");
}

static void qemu_validate_transport_report(const char *report_buf)
{
	qemu_check(strstr(report_buf,
			  "report.stealth.flags=0x6(modulehide,sysfshide)") != NULL,
		   "missing_report_stealth_flags");
	qemu_check(strstr(report_buf, "report.exposure.proc_modules=hidden") != NULL,
		   "missing_report_proc_modules");
	qemu_check(strstr(report_buf, "report.exposure.sysfs_module=hidden") != NULL,
		   "missing_report_sysfs_module");
	qemu_check(strstr(report_buf, "report.exposure.debugfs_dir=hidden") != NULL,
		   "missing_report_debugfs_dir");
	qemu_check(strstr(report_buf, "report.bootstrap.proc_open_successes=") != NULL,
		   "missing_report_proc_open_successes");
	qemu_check(strstr(report_buf, "report.exposure.sysfs_holders=") != NULL,
		   "missing_report_sysfs_holders");
	qemu_check(strstr(report_buf, "report.exposure.sysfs_sections=") != NULL,
		   "missing_report_sysfs_sections");
	qemu_check(strstr(report_buf, "report.exposure.debugfs_status=hidden") != NULL,
		   "missing_report_debugfs_status");
	qemu_check(strstr(report_buf, "report.exposure.debugfs_hooks=hidden") != NULL,
		   "missing_report_debugfs_hooks");
	qemu_check(strstr(report_buf, "report.exposure.kallsyms_symbols=") != NULL,
		   "missing_report_kallsyms_symbols");
}

static void qemu_run_transport_core_tools(void)
{
	char *const mem_test_argv[] = { MEM_TEST_TOOL, "selftest", NULL };
	char *const mmu_test_argv[] = { MMU_TEST_TOOL, "selftest", NULL };
	char *const ex_session_status_argv[] = { EXAMPLE_SESSION_STATUS_TOOL, NULL };
	char *const ex_mem_rw_argv[] = { EXAMPLE_MEM_RW_TOOL, NULL };
	char *const ex_threads_query_argv[] = { EXAMPLE_THREADS_QUERY_TOOL, NULL };
	char *const ex_regs_fp_argv[] = { EXAMPLE_REGS_FP_TOOL, NULL };
	char *const ex_stealth_roundtrip_argv[] = {
		EXAMPLE_STEALTH_ROUNDTRIP_TOOL,
		NULL,
	};
	int mem_test_status;

	qemu_cluster_begin("transport-core-tools");
	qemu_run_tool(ex_session_status_argv);
	qemu_run_tool(ex_mem_rw_argv);
	qemu_run_tool(ex_threads_query_argv);
	qemu_run_tool(ex_regs_fp_argv);
	qemu_run_tool(ex_stealth_roundtrip_argv);
	mem_test_status = qemu_run_tool_status(mem_test_argv);
	if (mem_test_status != 0) {
		printf("LKMDBG_QEMU_MEM_TEST_RETRY first_status=%d\n",
		       mem_test_status);
		fflush(stdout);
		mem_test_status = qemu_run_tool_status(mem_test_argv);
		qemu_check(mem_test_status == 0, "tool_exit_%s status=%d",
			   MEM_TEST_TOOL, mem_test_status);
	}
	qemu_run_tool(mmu_test_argv);
	qemu_cluster_ok("transport-core-tools");
}

static void qemu_run_transport_extended_tools(void)
{
	char *const ex_sysrule_combo_argv[] = { EXAMPLE_SYSRULE_COMBO_TOOL, NULL };
	char *const ex_vma_page_query_argv[] = { EXAMPLE_VMA_PAGE_QUERY_TOOL, NULL };
	char *const ex_remote_alloc_rw_argv[] = {
		EXAMPLE_REMOTE_ALLOC_RW_TOOL,
		NULL,
	};
	char *const ex_phys_translate_read_argv[] = {
		EXAMPLE_PHYS_TRANSLATE_READ_TOOL,
		NULL,
	};
	char *const ex_perf_baseline_argv[] = { EXAMPLE_PERF_BASELINE_TOOL, NULL };
	char *const ex_view_external_read_argv[] = {
		EXAMPLE_VIEW_EXTERNAL_READ_TOOL,
		NULL,
	};
	char *const ex_view_wxshadow_exec_argv[] = {
		EXAMPLE_VIEW_WXSHADOW_EXEC_TOOL,
		NULL,
	};

	qemu_cluster_begin("transport-extended-tools");
	qemu_run_tool(ex_sysrule_combo_argv);
	qemu_run_tool(ex_vma_page_query_argv);
	qemu_run_tool(ex_remote_alloc_rw_argv);
	qemu_run_tool(ex_phys_translate_read_argv);
	qemu_run_tool(ex_perf_baseline_argv);
	qemu_run_tool(ex_view_external_read_argv);
	qemu_run_tool(ex_view_wxshadow_exec_argv);
	if (qemu_status_debugfs_available) {
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
	qemu_cluster_ok("transport-extended-tools");
}

static double qemu_behavior_parse_metric_double(const char *buf, const char *key)
{
	const char *pos;
	char *endp;
	double value;

	qemu_check(buf != NULL && key != NULL, "behavior_metric_input_invalid");
	pos = strstr(buf, key);
	qemu_check(pos != NULL, "behavior_metric_missing_%s", key);
	pos += strlen(key);
	errno = 0;
	value = strtod(pos, &endp);
	qemu_check(errno == 0 && endp != pos, "behavior_metric_parse_%s", key);
	return value;
}

static void qemu_behavior_run_perf_samples(const char *phase,
					   struct qemu_behavior_perf_stats *stats)
{
	char *const perf_argv[] = { EXAMPLE_PERF_BASELINE_TOOL, NULL };
	double alloc_samples[QEMU_BEHAVIOR_PERF_SAMPLES];
	unsigned int i;

	qemu_check(stats != NULL, "behavior_perf_stats_null");
	memset(stats, 0, sizeof(*stats));

	for (i = 0; i < QEMU_BEHAVIOR_PERF_SAMPLES; i++) {
		char report_buf[4096];

		qemu_run_tool_capture(perf_argv, report_buf, sizeof(report_buf));
		alloc_samples[i] =
			qemu_behavior_parse_metric_double(report_buf, "alloc_us=");
		printf("LKMDBG_QEMU_BEHAVIOR_PERF_SAMPLE phase=%s idx=%u alloc_us=%.2f\n",
		       phase, i, alloc_samples[i]);
		fflush(stdout);
	}

	qsort(alloc_samples, QEMU_BEHAVIOR_PERF_SAMPLES, sizeof(alloc_samples[0]),
	      qemu_double_cmp);
	stats->alloc_p50_us =
		qemu_percentile_double(alloc_samples, QEMU_BEHAVIOR_PERF_SAMPLES, 50);
	stats->alloc_p95_us =
		qemu_percentile_double(alloc_samples, QEMU_BEHAVIOR_PERF_SAMPLES, 95);
	stats->alloc_p99_us =
		qemu_percentile_double(alloc_samples, QEMU_BEHAVIOR_PERF_SAMPLES, 99);
}

static void qemu_behavior_set_stealth_flags(__u32 desired_flags,
					    __u32 *old_flags_out,
					    __u32 *supported_flags_out)
{
	struct lkmdbg_stealth_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};
	int session_fd;

	session_fd = qemu_open_session();
	if (ioctl(session_fd, LKMDBG_IOC_GET_STEALTH, &req) != 0) {
		close(session_fd);
		qemu_fail("behavior_get_stealth errno=%d", errno);
	}
	if (old_flags_out)
		*old_flags_out = req.flags;
	if (supported_flags_out)
		*supported_flags_out = req.supported_flags;
	qemu_check((desired_flags & ~req.supported_flags) == 0,
		   "behavior_unsupported_stealth flags=0x%x supported=0x%x",
		   desired_flags, req.supported_flags);

	req.flags = desired_flags;
	if (ioctl(session_fd, LKMDBG_IOC_SET_STEALTH, &req) != 0) {
		close(session_fd);
		qemu_fail("behavior_set_stealth errno=%d flags=0x%x", errno,
			  desired_flags);
	}
	close(session_fd);
}

static double qemu_behavior_sample_syscall_avg_ns(
	const volatile struct qemu_behavior_shared *shared, unsigned int duration_ms)
{
	__u64 total_before;
	__u64 total_after;
	__u64 iters_before;
	__u64 iters_after;
	__u64 delta_total;
	__u64 delta_iters;

	qemu_check(shared != NULL, "behavior_syscall_shared_null");
	total_before = shared->syscall_total_ns;
	iters_before = shared->syscall_iters;
	usleep(duration_ms * 1000U);
	total_after = shared->syscall_total_ns;
	iters_after = shared->syscall_iters;
	delta_total = total_after - total_before;
	delta_iters = iters_after - iters_before;
	if (delta_iters == 0)
		return 0.0;
	return (double)delta_total / (double)delta_iters;
}

static void qemu_behavior_measure_heartbeat(
	const volatile struct qemu_behavior_shared *shared, unsigned int window_ms,
	struct qemu_behavior_heartbeat_stats *stats)
{
	const size_t max_samples = 4096;
	__u64 *samples;
	__u64 last_ns;
	__u64 deadline_ns;
	__u64 now_ns;
	__u64 last_seq;
	size_t count = 0;

	qemu_check(shared != NULL && stats != NULL, "behavior_heartbeat_input_null");
	memset(stats, 0, sizeof(*stats));
	samples = calloc(max_samples, sizeof(*samples));
	qemu_check(samples != NULL, "behavior_heartbeat_alloc_fail");

	now_ns = qemu_now_ns();
	last_ns = now_ns;
	deadline_ns = now_ns + (__u64)window_ms * 1000000ULL;
	last_seq = shared->heartbeat_seq;

	while ((now_ns = qemu_now_ns()) < deadline_ns) {
		__u64 current_seq = shared->heartbeat_seq;

		if (current_seq != last_seq) {
			__u64 gap_us = (now_ns - last_ns) / 1000ULL;

			if (count < max_samples)
				samples[count++] = gap_us;
			if (gap_us > stats->max_us)
				stats->max_us = gap_us;
			if (gap_us > 50000ULL)
				stats->over_50ms++;
			if (gap_us > 100000ULL)
				stats->over_100ms++;
			last_seq = current_seq;
			last_ns = now_ns;
		}

		usleep(QEMU_BEHAVIOR_HEARTBEAT_POLL_US);
	}

	{
		__u64 tail_gap_us = (deadline_ns - last_ns) / 1000ULL;

		if (count < max_samples)
			samples[count++] = tail_gap_us;
		if (tail_gap_us > stats->max_us)
			stats->max_us = tail_gap_us;
		if (tail_gap_us > 50000ULL)
			stats->over_50ms++;
		if (tail_gap_us > 100000ULL)
			stats->over_100ms++;
	}

	qemu_check(count > 0, "behavior_heartbeat_no_samples");
	qsort(samples, count, sizeof(samples[0]), qemu_u64_cmp);
	stats->sample_count = count;
	stats->p50_us = qemu_percentile_u64(samples, count, 50);
	stats->p95_us = qemu_percentile_u64(samples, count, 95);
	stats->p99_us = qemu_percentile_u64(samples, count, 99);
	free(samples);
}

static void qemu_behavior_spawn_worker(struct qemu_behavior_shared **shared_out,
				       pid_t *pid_out)
{
	struct qemu_behavior_shared *shared;
	pid_t child;
	unsigned int spin;

	qemu_check(shared_out != NULL && pid_out != NULL,
		   "behavior_worker_out_null");
	*shared_out = NULL;
	*pid_out = -1;

	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	qemu_check(shared != MAP_FAILED, "behavior_worker_mmap errno=%d", errno);
	memset(shared, 0, sizeof(*shared));

	child = fork();
	qemu_check(child >= 0, "behavior_worker_fork errno=%d", errno);
	if (child == 0) {
		__u64 total_ns = 0;
		__u64 iters = 0;

		prctl(PR_SET_NAME, "lkmdbg-behavior", 0, 0, 0);
		shared->ready = 1;
		while (!shared->stop) {
			__u64 t0 = qemu_now_ns();
			__u64 t1;

			(void)syscall(SYS_getpid);
			t1 = qemu_now_ns();
			total_ns += t1 - t0;
			iters++;
			shared->hot_value++;
			shared->heartbeat_seq++;
			shared->heartbeat_ns = t1;
			if ((iters & 0xffU) == 0) {
				shared->syscall_total_ns = total_ns;
				shared->syscall_iters = iters;
			}
			usleep(1000);
		}
		shared->syscall_total_ns = total_ns;
		shared->syscall_iters = iters;
		_exit(0);
	}

	for (spin = 0; spin < 1000U; spin++) {
		if (shared->ready)
			break;
		usleep(1000);
	}
	qemu_check(shared->ready != 0, "behavior_worker_ready_timeout");

	*shared_out = shared;
	*pid_out = child;
}

static void qemu_behavior_stop_worker(struct qemu_behavior_shared *shared,
				      pid_t pid)
{
	int status;

	if (!shared)
		return;
	if (pid > 0) {
		shared->stop = 1;
		if (waitpid(pid, &status, 0) < 0)
			qemu_fail("behavior_worker_waitpid errno=%d", errno);
		qemu_check(WIFEXITED(status),
			   "behavior_worker_signal_status=%d", status);
	}
	munmap(shared, sizeof(*shared));
}

static int qemu_behavior_read_proc_faults(pid_t pid, __u64 *minflt_out,
					  __u64 *majflt_out)
{
	char path[64];
	char buf[1024];
	char *rparen;
	char *cursor;
	char *saveptr = NULL;
	char *token;
	unsigned int field = 3U;
	__u64 minflt = 0;
	__u64 majflt = 0;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (!qemu_try_read_file(path, buf, sizeof(buf)))
		return -1;

	rparen = strrchr(buf, ')');
	if (!rparen || rparen[1] != ' ')
		return -1;
	cursor = rparen + 2;

	token = strtok_r(cursor, " ", &saveptr);
	if (!token)
		return -1;

	while ((token = strtok_r(NULL, " ", &saveptr)) != NULL) {
		char *endp;
		unsigned long long value;

		field++;
		if (field != 10U && field != 12U)
			continue;
		errno = 0;
		value = strtoull(token, &endp, 10);
		if (errno != 0 || endp == token)
			return -1;
		if (field == 10U)
			minflt = value;
		else
			majflt = value;
		if (field >= 12U)
			break;
	}

	if (field < 12U)
		return -1;
	*minflt_out = minflt;
	*majflt_out = majflt;
	return 0;
}

static int qemu_behavior_set_target(int session_fd, pid_t target_pid)
{
	struct lkmdbg_target_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tgid = target_pid,
		.tid = 0,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_TARGET, &req) != 0)
		return -errno;
	return 0;
}

static int qemu_behavior_query_step_tid(int session_fd, pid_t target_pid,
					pid_t *tid_out)
{
	struct lkmdbg_thread_entry entries[32];
	struct lkmdbg_thread_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = sizeof(entries) / sizeof(entries[0]),
	};
	pid_t fallback_tid = target_pid;

	for (;;) {
		__u32 i;

		memset(entries, 0, sizeof(entries));
		if (ioctl(session_fd, LKMDBG_IOC_QUERY_THREADS, &req) != 0)
			return -errno;
		for (i = 0; i < req.entries_filled; i++) {
			if (entries[i].tgid != target_pid || entries[i].tid <= 0)
				continue;
			if (entries[i].flags & LKMDBG_THREAD_FLAG_EXITING)
				continue;
			if (entries[i].tid != target_pid) {
				*tid_out = entries[i].tid;
				return 0;
			}
			fallback_tid = entries[i].tid;
		}
		if (req.done)
			break;
		req.start_tid = req.next_tid;
	}

	*tid_out = fallback_tid;
	return 0;
}

static int qemu_behavior_freeze(int session_fd, __u32 timeout_ms,
				struct lkmdbg_freeze_request *reply_out)
{
	struct lkmdbg_freeze_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.timeout_ms = timeout_ms,
	};

	if (ioctl(session_fd, LKMDBG_IOC_FREEZE_THREADS, &req) != 0)
		return -errno;
	if (reply_out)
		*reply_out = req;
	return 0;
}

static int qemu_behavior_thaw(int session_fd, __u32 timeout_ms)
{
	struct lkmdbg_freeze_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.timeout_ms = timeout_ms,
	};

	if (ioctl(session_fd, LKMDBG_IOC_THAW_THREADS, &req) != 0)
		return -errno;
	return 0;
}

static int qemu_behavior_get_stop_state(int session_fd,
					struct lkmdbg_stop_query_request *reply_out)
{
	struct lkmdbg_stop_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_STOP_STATE, &req) != 0)
		return -errno;
	if (reply_out)
		*reply_out = req;
	return 0;
}

static int qemu_behavior_continue(int session_fd, __u64 stop_cookie,
				  __u32 timeout_ms)
{
	struct lkmdbg_continue_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.timeout_ms = timeout_ms,
		.stop_cookie = stop_cookie,
	};

	if (ioctl(session_fd, LKMDBG_IOC_CONTINUE_TARGET, &req) != 0)
		return -errno;
	return 0;
}

static int qemu_behavior_single_step(int session_fd, pid_t tid)
{
	struct lkmdbg_single_step_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tid = tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SINGLE_STEP, &req) != 0)
		return -errno;
	return 0;
}

static int qemu_behavior_wait_stop_reason(int session_fd, __u32 reason,
					  __u32 timeout_ms)
{
	struct pollfd pfd = {
		.fd = session_fd,
		.events = POLLIN,
	};
	__u64 deadline_ns = qemu_now_ns() + (__u64)timeout_ms * 1000000ULL;

	for (;;) {
		int slice_ms = 100;
		int poll_ret;
		struct lkmdbg_event_record event;
		ssize_t nr;

		if (qemu_now_ns() >= deadline_ns)
			return -ETIMEDOUT;
		poll_ret = poll(&pfd, 1, slice_ms);
		if (poll_ret < 0)
			return -errno;
		if (poll_ret == 0)
			continue;
		if (!(pfd.revents & POLLIN))
			continue;
		nr = read(session_fd, &event, sizeof(event));
		if (nr < 0)
			return -errno;
		if ((size_t)nr != sizeof(event))
			return -EIO;
		if (event.type != LKMDBG_EVENT_TARGET_STOP)
			continue;
		if (event.code == reason)
			return 0;
	}
}

static int qemu_behavior_read_mem_u64(int session_fd, __u64 remote_addr,
				      __u64 *value_out)
{
	struct lkmdbg_mem_op op = {
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)value_out,
		.length = sizeof(*value_out),
	};
	struct lkmdbg_mem_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.ops_addr = (uintptr_t)&op,
		.op_count = 1U,
	};

	if (ioctl(session_fd, LKMDBG_IOC_READ_MEM, &req) != 0)
		return -errno;
	if (req.ops_done != 1U || req.bytes_done != sizeof(*value_out) ||
	    op.bytes_done != sizeof(*value_out))
		return -EIO;
	return 0;
}

static int qemu_behavior_write_mem_u64(int session_fd, __u64 remote_addr,
				       __u64 value)
{
	struct lkmdbg_mem_op op = {
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)&value,
		.length = sizeof(value),
	};
	struct lkmdbg_mem_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.ops_addr = (uintptr_t)&op,
		.op_count = 1U,
	};

	if (ioctl(session_fd, LKMDBG_IOC_WRITE_MEM, &req) != 0)
		return -errno;
	if (req.ops_done != 1U || req.bytes_done != sizeof(value) ||
	    op.bytes_done != sizeof(value))
		return -EIO;
	return 0;
}

static int qemu_behavior_add_hwpoint(int session_fd, pid_t tid, __u64 addr,
				     __u32 type, __u32 flags, __u64 *id_out)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.tid = tid,
		.type = type,
		.len = 8U,
		.flags = flags,
		.trigger_hit_count = 1U,
		.action_flags = LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE,
	};

	if (ioctl(session_fd, LKMDBG_IOC_ADD_HWPOINT, &req) != 0)
		return -errno;
	if (id_out)
		*id_out = req.id;
	return 0;
}

static int qemu_behavior_remove_hwpoint(int session_fd, __u64 id)
{
	struct lkmdbg_hwpoint_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_HWPOINT, &req) != 0)
		return -errno;
	return 0;
}

static int qemu_behavior_run_debug_ops(pid_t target_pid, __u64 hot_addr,
				       struct qemu_behavior_debug_result *result)
{
	int session_fd = -1;
	pid_t step_tid = target_pid;
	unsigned int iter;

	memset(result, 0, sizeof(*result));
	result->ret_code = -1;
	result->hwpoint_supported = 1U;

	session_fd = qemu_open_session();
	if (qemu_behavior_set_target(session_fd, target_pid) != 0)
		goto out;
	if (qemu_behavior_query_step_tid(session_fd, target_pid, &step_tid) != 0)
		goto out;

	for (iter = 0; iter < 3U; iter++) {
		struct lkmdbg_stop_query_request stop = { 0 };
		__u64 hwpoint_id = 0;
		__u64 value = 0;
		int ret;
		unsigned int mem_iter;

		ret = qemu_behavior_freeze(session_fd, QEMU_BEHAVIOR_DEBUG_TIMEOUT_MS,
					   NULL);
		if (ret != 0) {
			result->ret_code = ret;
			goto out;
		}
		result->freeze_rounds++;

		ret = qemu_behavior_get_stop_state(session_fd, &stop);
		if (ret != 0) {
			result->ret_code = ret;
			goto out;
		}
		if (!(stop.stop.flags & LKMDBG_STOP_FLAG_ACTIVE) ||
		    stop.stop.reason != LKMDBG_STOP_REASON_FREEZE) {
			result->ret_code = -EIO;
			goto out;
		}

		ret = qemu_behavior_single_step(session_fd, step_tid);
		if (ret != 0) {
			result->ret_code = ret;
			goto out;
		}
		ret = qemu_behavior_continue(session_fd, stop.stop.cookie,
					     QEMU_BEHAVIOR_DEBUG_TIMEOUT_MS);
		if (ret != 0) {
			result->ret_code = ret;
			goto out;
		}
		ret = qemu_behavior_wait_stop_reason(session_fd,
						     LKMDBG_STOP_REASON_SINGLE_STEP,
						     QEMU_BEHAVIOR_DEBUG_TIMEOUT_MS);
		if (ret != 0) {
			result->ret_code = ret;
			goto out;
		}

		ret = qemu_behavior_get_stop_state(session_fd, &stop);
		if (ret != 0) {
			result->ret_code = ret;
			goto out;
		}
		if (stop.stop.flags & LKMDBG_STOP_FLAG_ACTIVE) {
			ret = qemu_behavior_continue(session_fd, stop.stop.cookie,
						     QEMU_BEHAVIOR_DEBUG_TIMEOUT_MS);
			if (ret != 0) {
				result->ret_code = ret;
				goto out;
			}
		}
		result->step_rounds++;
		(void)qemu_behavior_thaw(session_fd, QEMU_BEHAVIOR_DEBUG_TIMEOUT_MS);

		ret = qemu_behavior_add_hwpoint(session_fd, step_tid, hot_addr,
						LKMDBG_HWPOINT_TYPE_WRITE, 0,
						&hwpoint_id);
		if (ret == -EOPNOTSUPP || ret == -EINVAL || ret == -ENOSYS ||
		    ret == -ENOENT) {
			result->hwpoint_supported = 0U;
		} else if (ret != 0) {
			result->ret_code = ret;
			goto out;
		} else {
			result->hwpoint_rounds++;
			usleep(40000);
			ret = qemu_behavior_remove_hwpoint(session_fd, hwpoint_id);
			if (ret != 0) {
				result->ret_code = ret;
				goto out;
			}
		}

		for (mem_iter = 0; mem_iter < 64U; mem_iter++) {
			ret = qemu_behavior_read_mem_u64(session_fd, hot_addr, &value);
			if (ret != 0) {
				result->ret_code = ret;
				goto out;
			}
			value++;
			ret = qemu_behavior_write_mem_u64(session_fd, hot_addr, value);
			if (ret != 0) {
				result->ret_code = ret;
				goto out;
			}
		}
	}

	{
		__u64 mmu_hwpoint_id = 0;
		int ret;

		ret = qemu_behavior_add_hwpoint(session_fd, step_tid, hot_addr,
						LKMDBG_HWPOINT_TYPE_WRITE,
						LKMDBG_HWPOINT_FLAG_MMU,
						&mmu_hwpoint_id);
		if (ret == 0) {
			result->mmu_supported = 1U;
			result->mmu_armed = 1U;
			usleep(50000);
			(void)qemu_behavior_remove_hwpoint(session_fd, mmu_hwpoint_id);
		} else if (ret == -EOPNOTSUPP || ret == -EINVAL || ret == -ENOSYS ||
			   ret == -ENOENT) {
			result->mmu_supported = 0U;
		} else {
			result->ret_code = ret;
			goto out;
		}
	}

	result->ret_code = 0;
out:
	if (session_fd >= 0) {
		struct lkmdbg_stop_query_request stop = { 0 };

		/* Best-effort cleanup so retries do not inherit a frozen target. */
		if (qemu_behavior_get_stop_state(session_fd, &stop) == 0 &&
		    (stop.stop.flags & LKMDBG_STOP_FLAG_ACTIVE) != 0U)
			(void)qemu_behavior_continue(session_fd, stop.stop.cookie,
						     QEMU_BEHAVIOR_DEBUG_TIMEOUT_MS);
		(void)qemu_behavior_thaw(session_fd, QEMU_BEHAVIOR_DEBUG_TIMEOUT_MS);
		close(session_fd);
	}
	return result->ret_code;
}

static pid_t qemu_behavior_spawn_disturbance(pid_t target_pid, __u64 hot_addr,
					     int *read_fd_out)
{
	int pipefd[2];
	pid_t child;

	qemu_check(read_fd_out != NULL, "behavior_disturbance_fd_null");
	if (pipe(pipefd) != 0)
		qemu_fail("behavior_disturbance_pipe errno=%d", errno);

	child = fork();
	qemu_check(child >= 0, "behavior_disturbance_fork errno=%d", errno);
	if (child == 0) {
		struct qemu_behavior_debug_result result;
		ssize_t nwritten;

		close(pipefd[0]);
		(void)qemu_behavior_run_debug_ops(target_pid, hot_addr, &result);
		nwritten = write(pipefd[1], &result, sizeof(result));
		close(pipefd[1]);
		if (nwritten != (ssize_t)sizeof(result))
			_exit(2);
		_exit(result.ret_code == 0 ? 0 : 1);
	}

	close(pipefd[1]);
	*read_fd_out = pipefd[0];
	return child;
}

static int qemu_behavior_collect_disturbance(pid_t child, int read_fd,
					     struct qemu_behavior_debug_result *out)
{
	struct qemu_behavior_debug_result result = { 0 };
	int status;
	ssize_t nr;

	if (waitpid(child, &status, 0) < 0) {
		close(read_fd);
		return -errno;
	}
	nr = read(read_fd, &result, sizeof(result));
	close(read_fd);
	if (nr != (ssize_t)sizeof(result))
		return -EIO;
	if (!WIFEXITED(status))
		return -EINTR;
	*out = result;
	if (WEXITSTATUS(status) != 0 || result.ret_code != 0)
		return result.ret_code != 0 ? result.ret_code : -ECHILD;
	return 0;
}

static void qemu_run_behavior_cluster(void)
{
	const __u32 behavior_stealth_flags =
		LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN |
		LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN;
	struct qemu_behavior_perf_stats perf_base;
	struct qemu_behavior_perf_stats perf_stealth;
	struct qemu_behavior_shared *shared = NULL;
	struct qemu_behavior_heartbeat_stats hb_base;
	struct qemu_behavior_heartbeat_stats hb_stress;
	struct qemu_behavior_debug_result disturbance = { 0 };
	pid_t worker_pid = -1;
	pid_t disturbance_pid;
	int disturbance_fd = -1;
	__u64 faults_before_min = 0;
	__u64 faults_before_maj = 0;
	__u64 faults_after_min = 0;
	__u64 faults_after_maj = 0;
	__u64 minflt_delta;
	__u64 majflt_delta;
	__u32 old_stealth_flags = 0;
	__u32 supported_stealth_flags = 0;
	double syscall_base_ns;
	double syscall_stress_ns;
	double alloc_p99_limit;
	int disturbance_ret = -1;
	unsigned int disturbance_attempt = 0;
	unsigned int disturbance_attempts_run = 0;

	qemu_cluster_begin("behavior");

	qemu_behavior_set_stealth_flags(0, NULL, NULL);
	qemu_behavior_run_perf_samples("baseline", &perf_base);

	qemu_behavior_set_stealth_flags(behavior_stealth_flags, &old_stealth_flags,
					&supported_stealth_flags);
	qemu_check((supported_stealth_flags & behavior_stealth_flags) ==
			   behavior_stealth_flags,
		   "behavior_stealth_not_supported supported=0x%x required=0x%x",
		   supported_stealth_flags, behavior_stealth_flags);
	qemu_behavior_run_perf_samples("stealth", &perf_stealth);
	qemu_behavior_set_stealth_flags(old_stealth_flags, NULL, NULL);

	alloc_p99_limit = perf_base.alloc_p99_us * QEMU_BEHAVIOR_ALLOC_P99_RATIO_LIMIT +
			  QEMU_BEHAVIOR_ALLOC_P99_MARGIN_US;
	printf("LKMDBG_QEMU_BEHAVIOR_PERF baseline_p50_us=%.2f baseline_p95_us=%.2f baseline_p99_us=%.2f stealth_p50_us=%.2f stealth_p95_us=%.2f stealth_p99_us=%.2f limit_p99_us=%.2f\n",
	       perf_base.alloc_p50_us, perf_base.alloc_p95_us, perf_base.alloc_p99_us,
	       perf_stealth.alloc_p50_us, perf_stealth.alloc_p95_us,
	       perf_stealth.alloc_p99_us, alloc_p99_limit);
	fflush(stdout);
	qemu_check(perf_stealth.alloc_p99_us <= alloc_p99_limit,
		   "behavior_perf_alloc_p99_regression stealth=%.2f limit=%.2f",
		   perf_stealth.alloc_p99_us, alloc_p99_limit);

	qemu_behavior_spawn_worker(&shared, &worker_pid);
	qemu_behavior_measure_heartbeat(shared, QEMU_BEHAVIOR_HEARTBEAT_WINDOW_MS,
					&hb_base);
	syscall_base_ns = qemu_behavior_sample_syscall_avg_ns(
		shared, QEMU_BEHAVIOR_SYSCALL_SAMPLE_MS);
	qemu_check(qemu_behavior_read_proc_faults(worker_pid, &faults_before_min,
						  &faults_before_maj) == 0,
		   "behavior_faults_before_failed");

	for (disturbance_attempt = 0;
	     disturbance_attempt < QEMU_BEHAVIOR_DISTURBANCE_RETRIES;
	     disturbance_attempt++) {
		disturbance_attempts_run = disturbance_attempt + 1U;
		disturbance_pid = qemu_behavior_spawn_disturbance(
			worker_pid, (uintptr_t)&shared->hot_value, &disturbance_fd);
		syscall_stress_ns = qemu_behavior_sample_syscall_avg_ns(
			shared, QEMU_BEHAVIOR_SYSCALL_SAMPLE_MS);
		qemu_behavior_measure_heartbeat(shared,
						QEMU_BEHAVIOR_HEARTBEAT_WINDOW_MS,
						&hb_stress);
		disturbance_ret = qemu_behavior_collect_disturbance(
			disturbance_pid, disturbance_fd, &disturbance);
		if (disturbance_ret == 0)
			break;
		usleep(50000);
	}
	qemu_check(disturbance_ret == 0,
		   "behavior_disturbance_fail ret=%d attempts=%u freeze=%u step=%u hwpoint=%u",
		   disturbance_ret, disturbance_attempts_run,
		   disturbance.freeze_rounds, disturbance.step_rounds,
		   disturbance.hwpoint_rounds);
	qemu_check(qemu_behavior_read_proc_faults(worker_pid, &faults_after_min,
						  &faults_after_maj) == 0,
		   "behavior_faults_after_failed");
	qemu_behavior_stop_worker(shared, worker_pid);
	shared = NULL;
	worker_pid = -1;

	minflt_delta = faults_after_min - faults_before_min;
	majflt_delta = faults_after_maj - faults_before_maj;

	printf("LKMDBG_QEMU_BEHAVIOR_DEBUG freeze_rounds=%u step_rounds=%u hwpoint_rounds=%u hwpoint_supported=%u mmu_supported=%u mmu_armed=%u\n",
	       disturbance.freeze_rounds, disturbance.step_rounds,
	       disturbance.hwpoint_rounds, disturbance.hwpoint_supported,
	       disturbance.mmu_supported, disturbance.mmu_armed);
	printf("LKMDBG_QEMU_BEHAVIOR_MEMORY minflt_delta=%llu majflt_delta=%llu syscall_base_ns=%.2f syscall_stress_ns=%.2f\n",
	       (unsigned long long)minflt_delta, (unsigned long long)majflt_delta,
	       syscall_base_ns, syscall_stress_ns);
	printf("LKMDBG_QEMU_BEHAVIOR_APP base_p99_us=%llu base_max_us=%llu stress_p99_us=%llu stress_max_us=%llu stress_over_100ms=%llu\n",
	       (unsigned long long)hb_base.p99_us, (unsigned long long)hb_base.max_us,
	       (unsigned long long)hb_stress.p99_us,
	       (unsigned long long)hb_stress.max_us,
	       (unsigned long long)hb_stress.over_100ms);
	fflush(stdout);

	qemu_check(disturbance.freeze_rounds >= 1U, "behavior_no_freeze_rounds");
	qemu_check(disturbance.step_rounds >= 1U, "behavior_no_step_rounds");
	if (disturbance.hwpoint_supported != 0U)
		qemu_check(disturbance.hwpoint_rounds >= 1U,
			   "behavior_no_hwpoint_rounds");
	qemu_check(minflt_delta <= QEMU_BEHAVIOR_MINFLT_DELTA_MAX,
		   "behavior_minflt_delta_too_large=%llu",
		   (unsigned long long)minflt_delta);
	qemu_check(majflt_delta <= QEMU_BEHAVIOR_MAJFLT_DELTA_MAX,
		   "behavior_majflt_delta_too_large=%llu",
		   (unsigned long long)majflt_delta);
	if (syscall_base_ns > 0.0 && syscall_stress_ns > 0.0) {
		qemu_check(
			syscall_stress_ns <=
				syscall_base_ns * QEMU_BEHAVIOR_SYSCALL_RATIO_LIMIT +
					QEMU_BEHAVIOR_SYSCALL_MARGIN_NS,
			"behavior_syscall_latency_regression base=%.2f stress=%.2f",
			syscall_base_ns, syscall_stress_ns);
	}
	qemu_check(hb_stress.max_us <= QEMU_BEHAVIOR_HEARTBEAT_MAX_US,
		   "behavior_heartbeat_max_too_large=%llu",
		   (unsigned long long)hb_stress.max_us);
	qemu_check(hb_stress.over_100ms <= QEMU_BEHAVIOR_HEARTBEAT_OVER_100MS_MAX,
		   "behavior_heartbeat_over_100ms=%llu",
		   (unsigned long long)hb_stress.over_100ms);

	qemu_cluster_ok("behavior");
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

static void qemu_drain_input_until_idle(int channel_fd, int idle_ms,
					int budget_ms)
{
	struct lkmdbg_input_event events[16];
	int remaining_ms = budget_ms;

	while (remaining_ms > 0) {
		int slice_ms = remaining_ms > idle_ms ? idle_ms : remaining_ms;
		ssize_t event_count;

		event_count = qemu_input_read_events(
			channel_fd, events, sizeof(events) / sizeof(events[0]),
			slice_ms);
		if (event_count == 0)
			return;

		remaining_ms -= slice_ms;
	}

	qemu_fail("input_never_went_idle budget_ms=%d idle_ms=%d", budget_ms,
		  idle_ms);
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
	qemu_check(handler_hits == 0, "input_stealth_handler_exposed=%u",
		   handler_hits);
	qemu_check(name_hits == 0, "input_stealth_name_exposed=%u", name_hits);
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
	printf("LKMDBG_QEMU_INPUT_HOST_KEY_OK device_id=%llu\n",
	       (unsigned long long)device_id);
	fflush(stdout);
	qemu_drain_input_until_idle(host_fd, 150, 2000);
	close(host_fd);

	default_fd = qemu_open_input_channel(session_fd, device_id, 0);
	include_fd = qemu_open_input_channel(
		session_fd, device_id, LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED);

	nwritten = write(include_fd, inject_events, sizeof(inject_events));
	qemu_check(nwritten == (ssize_t)sizeof(inject_events),
		   "short_input_inject_write=%zd", nwritten);
	qemu_wait_for_key_event(include_fd, QEMU_INPUT_INJECT_TIMEOUT_MS, true);
	printf("LKMDBG_QEMU_INPUT_INJECT_OK device_id=%llu\n",
	       (unsigned long long)device_id);
	fflush(stdout);
	qemu_expect_no_input_events(default_fd, QEMU_INPUT_IDLE_TIMEOUT_MS);
	printf("LKMDBG_QEMU_INPUT_DEFAULT_CHANNEL_CLEAN device_id=%llu\n",
	       (unsigned long long)device_id);
	fflush(stdout);
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

static void qemu_read_event_blocking(int session_fd,
				     struct lkmdbg_event_record *event_out)
{
	struct pollfd pfd = {
		.fd = session_fd,
		.events = POLLIN,
	};
	struct lkmdbg_event_record event;
	int poll_ret;
	ssize_t nr;

	poll_ret = poll(&pfd, 1, 1000);
	if (poll_ret < 0)
		qemu_fail("poll_session_event errno=%d", errno);
	qemu_check(poll_ret > 0 && (pfd.revents & POLLIN),
		   "missing_session_event");

	nr = read(session_fd, &event, sizeof(event));
	if (nr < 0)
		qemu_fail("read_session_event errno=%d", errno);
	qemu_check((size_t)nr == sizeof(event), "short_session_event_read=%zd",
		   nr);
	if (event_out)
		*event_out = event;
}

static void qemu_expect_event_type(int session_fd, unsigned int type)
{
	struct lkmdbg_event_record event;

	for (;;) {
		qemu_read_event_blocking(session_fd, &event);
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
		if (snprintf(param_buf, sizeof(param_buf),
			     "enable_debugfs=1 enable_kmsg=1 %s",
			     params) >= (int)sizeof(param_buf)) {
			close(fd);
			qemu_fail("module_params_too_long");
		}
		params = param_buf;
	} else {
		params = "enable_debugfs=1 enable_kmsg=1";
	}

	if (syscall(SYS_finit_module, fd, params, 0) != 0) {
		close(fd);
		qemu_fail("insmod_%s errno=%d", params, errno);
	}

	close(fd);
}

static void qemu_rmmod(void)
{
	unsigned int attempt;

	for (attempt = 0; attempt < 80; attempt++) {
		if (syscall(SYS_delete_module, MODULE_NAME, 0) == 0)
			return;
		if (errno == EINTR)
			continue;
		if (errno == EBUSY || errno == EAGAIN) {
			usleep(50000);
			continue;
		}
		qemu_fail("rmmod errno=%d", errno);
	}

	qemu_fail("rmmod timeout errno=%d", errno);
}

static bool qemu_probe_session_transport_available(void)
{
	char *const open_session_argv[] = { OPEN_SESSION_TOOL, NULL };
	int open_session_status;

	open_session_status = qemu_run_tool_status(open_session_argv);
	if (open_session_status == 0)
		return true;
	if (!qemu_status_debugfs_available && open_session_status == 1) {
		printf("LKMDBG_QEMU_SESSION_UNAVAILABLE status=%d\n",
		       open_session_status);
		fflush(stdout);
		return false;
	}

	qemu_fail("open_session_tool_exit_%d", open_session_status);
	return false;
}

static void qemu_run_watchpoint_control_smoke(void)
{
	char *const watchpoint_ctrl_argv[] = { WATCHPOINT_CTRL_TOOL, NULL };
	int watchpoint_ctrl_status;

	qemu_cluster_begin("watchpoint");
	watchpoint_ctrl_status = qemu_run_tool_status(watchpoint_ctrl_argv);
	if (watchpoint_ctrl_status == 0) {
		printf("LKMDBG_QEMU_WATCHPOINT_CTRL_OK\n");
	} else if (watchpoint_ctrl_status == 2) {
		printf("LKMDBG_QEMU_WATCHPOINT_CTRL_SKIP\n");
	} else {
		qemu_fail("watchpoint_ctrl_exit_%d", watchpoint_ctrl_status);
	}
	fflush(stdout);
	qemu_cluster_ok("watchpoint");
}

static void qemu_run_hook_selftests(unsigned int stress_repeats)
{
	size_t i;
	unsigned int iter;

	qemu_cluster_begin("selftest");
	for (i = 0; i < sizeof(qemu_hook_selftests) /
			    sizeof(qemu_hook_selftests[0]);
	     i++) {
		unsigned int repeats = qemu_hook_selftests[i].repeats;

		if (strcmp(qemu_hook_selftests[i].params, "hook_selftest_mode=5") == 0 ||
		    strcmp(qemu_hook_selftests[i].params, "hook_selftest_mode=6") == 0)
			repeats = stress_repeats;

		for (iter = 0; iter < repeats; iter++) {
			qemu_insmod(qemu_hook_selftests[i].params);
			qemu_expect_status_line(qemu_hook_selftests[i].status_line);
			if (qemu_hook_selftests[i].expect_installed) {
				qemu_expect_status_line("hook_selftest_installed=1\n");
				qemu_expect_status_u64_at_least("inline_hook_active=", 1);
			} else {
				qemu_expect_status_line("hook_selftest_installed=0\n");
			}
			qemu_rmmod();
		}
	}
	qemu_cluster_ok("selftest");
}

static void qemu_run_transport_tool_smoke(void)
{
	char report_buf[4096];
	char *const stealth_report_argv[] = { STEALTH_CTL_TOOL, "report", NULL };

	qemu_cluster_begin("transport");
	qemu_run_transport_negative_tests();
	qemu_cluster_begin("transport-report");
	qemu_run_tool_capture(stealth_report_argv, report_buf, sizeof(report_buf));
	printf("%s", report_buf);
	fflush(stdout);
	qemu_validate_transport_report(report_buf);
	qemu_cluster_ok("transport-report");
	qemu_run_transport_core_tools();
	qemu_run_transport_extended_tools();
	qemu_cluster_ok("transport");
}

static void qemu_run_input_cluster(void)
{
	qemu_cluster_begin("input");
	qemu_run_input_smoke();
	qemu_cluster_ok("input");
}

static void qemu_run_proc_exposure_cluster(void)
{
	qemu_cluster_begin("exposure");
	qemu_report_user_proc_exposure();
	qemu_cluster_ok("exposure");
}

static void qemu_run_seq_read_smoke(unsigned int seq_read_repeats,
				    bool allow_session_transport,
				    bool session_transport_available)
{
	char version_buf[4096];
	unsigned int iter;

	qemu_cluster_begin("seqread");
	for (iter = 0; iter < seq_read_repeats; iter++) {
		int session_fd = -1;
		int err;

		qemu_insmod("hook_proc_version=1 hook_seq_read=1");
		qemu_expect_status_line("seq_read_hook_active=1\n");
		qemu_expect_status_line("proc_version_hook_active=1\n");
		qemu_expect_status_u64_at_least("inline_hook_active=", 1);
		if (qemu_hooks_debugfs_available) {
			if (!qemu_try_read_file_errno(HOOKS_PATH, version_buf,
						      sizeof(version_buf),
						      &err)) {
				if (err == ENOENT) {
					qemu_hooks_debugfs_available = false;
					if (!qemu_hooks_debugfs_reported) {
						printf("LKMDBG_QEMU_HOOKS_UNAVAILABLE errno=%d\n",
						       err);
						fflush(stdout);
						qemu_hooks_debugfs_reported = true;
					}
				} else {
					qemu_fail("open_%s errno=%d", HOOKS_PATH, err);
				}
			}
			if (qemu_hooks_debugfs_available) {
				qemu_check(strstr(version_buf, "name=seq_read") != NULL,
					   "missing_seq_read_registry");
				qemu_check(strstr(version_buf,
						  "name=proc_version_open") != NULL,
					   "missing_proc_version_open_registry");
			}
		}
		if (allow_session_transport && session_transport_available) {
			session_fd = qemu_open_session();
			qemu_drain_one_event(session_fd);
		}
		qemu_read_file("/proc/version", version_buf, sizeof(version_buf));
		qemu_check(version_buf[0] != '\0', "empty_proc_version_seq_read");
		if (allow_session_transport && session_transport_available)
			qemu_expect_event_type(session_fd, LKMDBG_EVENT_HOOK_HIT);
		qemu_expect_status_u64_at_least("seq_read_hook_hits=", 2);
		qemu_expect_status_u64_at_least("inline_hook_install_total=", 1);
		if (session_fd >= 0)
			close(session_fd);
		qemu_rmmod();
	}
	qemu_cluster_ok("seqread");
}

int main(void)
{
	char version_buf[4096];
	char cluster_buf[128];
	unsigned int smoke_clusters;
	unsigned int iter;
	bool hook_soak_only;
	bool session_transport_available = true;
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
	smoke_clusters = qemu_parse_smoke_clusters(hook_soak_only);

	if (hook_soak_only) {
		printf("LKMDBG_QEMU_HOOK_SOAK_BEGIN proc=%u seq=%u\n",
		       proc_version_repeats, seq_read_repeats);
	} else {
		printf("LKMDBG_QEMU_SMOKE_BEGIN\n");
	}
	printf("LKMDBG_QEMU_SMOKE_CLUSTERS mask=0x%x names=%s\n", smoke_clusters,
	       qemu_describe_smoke_clusters(smoke_clusters, cluster_buf,
					    sizeof(cluster_buf)));
	fflush(stdout);

	if (!hook_soak_only &&
	    (smoke_clusters & QEMU_SMOKE_CLUSTER_WATCHPOINT))
		qemu_run_watchpoint_control_smoke();
	else if (!hook_soak_only)
		qemu_cluster_skip("watchpoint", "not-selected");

	if (!hook_soak_only &&
	    (smoke_clusters & QEMU_SMOKE_CLUSTER_HOOK_SELFTEST))
		qemu_run_hook_selftests(selftest_stress_repeats);
	else if (!hook_soak_only)
		qemu_cluster_skip("selftest", "not-selected");

	for (iter = 0; iter < proc_version_repeats; iter++) {
		qemu_insmod(!hook_soak_only && iter == 0 ?
			    "hook_proc_version=1 enable_input_tracking=1" :
			    "hook_proc_version=1");
		qemu_expect_status_u64_at_least("inline_hook_active=", 1);
		qemu_read_file("/proc/version", version_buf, sizeof(version_buf));
		qemu_check(version_buf[0] != '\0', "empty_proc_version");
		qemu_expect_status_line("proc_version_hook_active=1\n");
		qemu_expect_status_u64_at_least("proc_open_successes=", 1);
		if (!hook_soak_only && iter == 0) {
			if (smoke_clusters & (QEMU_SMOKE_CLUSTER_TRANSPORT |
					      QEMU_SMOKE_CLUSTER_INPUT |
					      QEMU_SMOKE_CLUSTER_PROC_EXPOSURE)) {
				session_transport_available =
					qemu_probe_session_transport_available();
			}
				if (session_transport_available) {
					if (smoke_clusters & QEMU_SMOKE_CLUSTER_TRANSPORT)
						qemu_run_transport_tool_smoke();
					else
						qemu_cluster_skip("transport", "not-selected");
					if (smoke_clusters & QEMU_SMOKE_CLUSTER_INPUT)
						qemu_run_input_cluster();
					else
						qemu_cluster_skip("input", "not-selected");
					if (smoke_clusters & QEMU_SMOKE_CLUSTER_PROC_EXPOSURE)
						qemu_run_proc_exposure_cluster();
					else
						qemu_cluster_skip("exposure", "not-selected");
					if (smoke_clusters & QEMU_SMOKE_CLUSTER_BEHAVIOR)
						qemu_run_behavior_cluster();
					else
						qemu_cluster_skip("behavior", "not-selected");
				} else {
					if (smoke_clusters & QEMU_SMOKE_CLUSTER_TRANSPORT)
						qemu_cluster_skip("transport",
								  "session-unavailable");
					if (smoke_clusters & QEMU_SMOKE_CLUSTER_INPUT)
						qemu_cluster_skip("input",
								  "session-unavailable");
					if (smoke_clusters & QEMU_SMOKE_CLUSTER_PROC_EXPOSURE)
						qemu_cluster_skip("exposure",
								  "session-unavailable");
					if (smoke_clusters & QEMU_SMOKE_CLUSTER_BEHAVIOR)
						qemu_cluster_skip("behavior",
								  "session-unavailable");
				}
			}
		qemu_rmmod();
	}

	if (smoke_clusters & QEMU_SMOKE_CLUSTER_SEQ_READ)
		qemu_run_seq_read_smoke(seq_read_repeats, !hook_soak_only,
					session_transport_available);
	else
		qemu_cluster_skip("seqread", "not-selected");

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
