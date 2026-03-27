#ifndef LKMDBG_DRIVER_EVENTS_HPP
#define LKMDBG_DRIVER_EVENTS_HPP

#include "../../include/lkmdbg_ioctl.h"

namespace lkmdbg {
namespace driver {
namespace events {

int set_event_config(int session_fd,
		     struct lkmdbg_event_config_request *req_out);
int get_event_config(int session_fd,
		     struct lkmdbg_event_config_request *req_out);

} // namespace events
} // namespace driver
} // namespace lkmdbg

#endif
