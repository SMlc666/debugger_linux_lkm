#include "session.hpp"
#include "common.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace lkmdbg {
namespace driver {
namespace session {

static const char *kTargetPath = "/proc/version";

int open_session_fd()
{
	struct lkmdbg_open_session_request req;
	int proc_fd;
	int session_fd;

	memset(&req, 0, sizeof(req));
	req.version = LKMDBG_PROTO_VERSION;
	req.size = sizeof(req);

	proc_fd = open(kTargetPath, O_RDONLY | O_CLOEXEC);
	if (proc_fd < 0) {
		lkmdbg_log_errorf("open(%s) failed: %s", kTargetPath,
				  strerror(errno));
		return -1;
	}

	session_fd = ioctl(proc_fd, LKMDBG_IOC_OPEN_SESSION, &req);
	if (session_fd < 0) {
		lkmdbg_log_errnof("OPEN_SESSION");
		close(proc_fd);
		return -1;
	}

	close(proc_fd);
	return session_fd;
}

int set_target_ex(int session_fd, pid_t pid, pid_t tid)
{
	struct lkmdbg_target_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.tgid = pid,
		.tid = tid,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_TARGET, &req) < 0) {
		lkmdbg_log_errnof("SET_TARGET");
		return -1;
	}

	return 0;
}

int set_target(int session_fd, pid_t pid)
{
	return set_target_ex(session_fd, pid, 0);
}

int get_status(int session_fd, struct lkmdbg_status_reply *reply_out)
{
	struct lkmdbg_status_reply reply;

	memset(&reply, 0, sizeof(reply));
	reply.version = LKMDBG_PROTO_VERSION;
	reply.size = sizeof(reply);

	if (ioctl(session_fd, LKMDBG_IOC_GET_STATUS, &reply) < 0) {
		lkmdbg_log_errnof("GET_STATUS");
		return -1;
	}

	if (reply_out)
		*reply_out = reply;

	return 0;
}

} // namespace session
} // namespace driver
} // namespace lkmdbg
