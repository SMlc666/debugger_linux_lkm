#ifndef LKMDBG_DRIVER_SESSION_HPP
#define LKMDBG_DRIVER_SESSION_HPP

#include <sys/types.h>

#include "../../include/lkmdbg_ioctl.h"

namespace lkmdbg {
namespace driver {
namespace session {

int open_session_fd();
int set_target_ex(int session_fd, pid_t pid, pid_t tid);
int set_target(int session_fd, pid_t pid);
int get_status(int session_fd, struct lkmdbg_status_reply *reply_out);

} // namespace session
} // namespace driver
} // namespace lkmdbg

#endif
