#include "events.hpp"
#include "common.hpp"

#include <errno.h>
#include <sys/ioctl.h>

namespace lkmdbg {
namespace driver {
namespace events {

int set_event_config(int session_fd,
		     struct lkmdbg_event_config_request *req_out)
{
	if (!req_out) {
		errno = EINVAL;
		return -1;
	}

	req_out->version = LKMDBG_PROTO_VERSION;
	req_out->size = sizeof(*req_out);
	if (ioctl(session_fd, LKMDBG_IOC_SET_EVENT_CONFIG, req_out) < 0) {
		lkmdbg_log_errnof("SET_EVENT_CONFIG");
		return -1;
	}

	return 0;
}

int get_event_config(int session_fd,
		     struct lkmdbg_event_config_request *req_out)
{
	if (!req_out) {
		errno = EINVAL;
		return -1;
	}

	memset(req_out, 0, sizeof(*req_out));
	req_out->version = LKMDBG_PROTO_VERSION;
	req_out->size = sizeof(*req_out);
	if (ioctl(session_fd, LKMDBG_IOC_GET_EVENT_CONFIG, req_out) < 0) {
		lkmdbg_log_errnof("GET_EVENT_CONFIG");
		return -1;
	}

	return 0;
}

} // namespace events
} // namespace driver
} // namespace lkmdbg
