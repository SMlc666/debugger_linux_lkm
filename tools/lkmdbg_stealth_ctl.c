#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../include/lkmdbg_ioctl.h"

#define TARGET_PATH "/proc/version"

static int open_session_fd(void)
{
	struct lkmdbg_open_session_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};
	int proc_fd;
	int session_fd;

	proc_fd = open(TARGET_PATH, O_RDONLY | O_CLOEXEC);
	if (proc_fd < 0) {
		fprintf(stderr, "open(%s) failed: %s\n", TARGET_PATH,
			strerror(errno));
		return -1;
	}

	session_fd = ioctl(proc_fd, LKMDBG_IOC_OPEN_SESSION, &req);
	if (session_fd < 0) {
		fprintf(stderr, "OPEN_SESSION failed: %s\n", strerror(errno));
		close(proc_fd);
		return -1;
	}

	close(proc_fd);
	return session_fd;
}

static int get_status(int session_fd, struct lkmdbg_status_reply *reply_out)
{
	struct lkmdbg_status_reply reply = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(reply),
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_STATUS, &reply) < 0) {
		fprintf(stderr, "GET_STATUS failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = reply;

	return 0;
}

static int get_stealth(int session_fd,
		       struct lkmdbg_stealth_request *reply_out)
{
	struct lkmdbg_stealth_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
	};

	if (ioctl(session_fd, LKMDBG_IOC_GET_STEALTH, &req) < 0) {
		fprintf(stderr, "GET_STEALTH failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;

	return 0;
}

static int set_stealth(int session_fd, uint32_t flags,
		       struct lkmdbg_stealth_request *reply_out)
{
	struct lkmdbg_stealth_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.flags = flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_STEALTH, &req) < 0) {
		fprintf(stderr, "SET_STEALTH failed: %s\n", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;

	return 0;
}

static void append_flag_name(char *buf, size_t buf_size, const char *name)
{
	size_t len;

	if (!buf_size)
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

static const char *describe_flags(uint32_t flags, char *buf, size_t buf_size)
{
	if (!buf_size)
		return "";

	buf[0] = '\0';
	if (flags & LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE)
		append_flag_name(buf, buf_size, "debugfs");
	if (flags & LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN)
		append_flag_name(buf, buf_size, "modulehide");
	if (!buf[0])
		snprintf(buf, buf_size, "none");
	return buf;
}

static int parse_flags(const char *arg, uint32_t *flags_out)
{
	char *copy;
	char *token;
	char *saveptr = NULL;
	char *endp = NULL;
	uint32_t flags = 0;

	if (strcmp(arg, "none") == 0 || strcmp(arg, "restore") == 0 ||
	    strcmp(arg, "0") == 0) {
		*flags_out = 0;
		return 0;
	}

	if (strcmp(arg, "hide") == 0) {
		*flags_out = LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN;
		return 0;
	}

	copy = strdup(arg);
	if (!copy)
		return -1;

	for (token = strtok_r(copy, ",|", &saveptr); token;
	     token = strtok_r(NULL, ",|", &saveptr)) {
		if (strcmp(token, "debugfs") == 0 ||
		    strcmp(token, "debugfs_on") == 0) {
			flags |= LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE;
			continue;
		}
		if (strcmp(token, "modulehide") == 0 ||
		    strcmp(token, "module_hidden") == 0 ||
		    strcmp(token, "hide") == 0) {
			flags |= LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN;
			continue;
		}
		free(copy);
		goto numeric;
	}

	free(copy);
	*flags_out = flags;
	return 0;

numeric:
	*flags_out = (uint32_t)strtoul(arg, &endp, 0);
	if (*endp != '\0')
		return -1;
	return 0;
}

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"usage:\n"
		"  %s show\n"
		"  %s set <none|hide|debugfs|modulehide|debugfs,modulehide>\n"
		"  %s hide\n"
		"  %s restore\n",
		prog, prog, prog, prog);
}

static void print_reply(const struct lkmdbg_status_reply *status,
			const struct lkmdbg_stealth_request *stealth)
{
	char flags_buf[48];
	char supported_buf[48];

	printf("session_id=%" PRIu64 "\n", (uint64_t)status->session_id);
	printf("hook_active=%u\n", status->hook_active);
	printf("active_sessions=%" PRIu64 "\n",
	       (uint64_t)status->active_sessions);
	printf("stealth.flags=0x%x(%s)\n", stealth->flags,
	       describe_flags(stealth->flags, flags_buf, sizeof(flags_buf)));
	printf("stealth.supported=0x%x(%s)\n", stealth->supported_flags,
	       describe_flags(stealth->supported_flags, supported_buf,
			      sizeof(supported_buf)));
}

int main(int argc, char **argv)
{
	struct lkmdbg_status_reply status;
	struct lkmdbg_stealth_request stealth;
	const char *cmd;
	uint32_t flags = 0;
	int session_fd;
	int ret = 1;

	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	cmd = argv[1];
	if (strcmp(cmd, "show") != 0 && strcmp(cmd, "set") != 0 &&
	    strcmp(cmd, "hide") != 0 && strcmp(cmd, "restore") != 0) {
		print_usage(argv[0]);
		return 1;
	}

	if (strcmp(cmd, "set") == 0) {
		if (argc != 3) {
			print_usage(argv[0]);
			return 1;
		}
		if (parse_flags(argv[2], &flags) < 0) {
			fprintf(stderr, "invalid flags: %s\n", argv[2]);
			return 1;
		}
	} else if (strcmp(cmd, "hide") == 0) {
		flags = LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN;
	} else if (strcmp(cmd, "restore") == 0) {
		flags = 0;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		return 1;

	memset(&status, 0, sizeof(status));
	memset(&stealth, 0, sizeof(stealth));

	if (strcmp(cmd, "show") == 0) {
		if (get_stealth(session_fd, &stealth) < 0)
			goto out;
		if (get_status(session_fd, &status) < 0)
			goto out;
	} else {
		if (set_stealth(session_fd, flags, &stealth) < 0)
			goto out;
		if (get_status(session_fd, &status) < 0)
			goto out;
	}

	print_reply(&status, &stealth);
	ret = 0;

out:
	close(session_fd);
	return ret;
}
