#ifndef LKMDBG_DRIVER_DRIVER_HPP
#define LKMDBG_DRIVER_DRIVER_HPP

#include <sys/types.h>

#include <stddef.h>
#include <stdint.h>

#include "../../include/lkmdbg_ioctl.h"

namespace lkmdbg {
namespace driver {

class Driver {
public:
	Driver();
	~Driver();

	Driver(const Driver &) = delete;
	Driver &operator=(const Driver &) = delete;

	int open_session();
	void close_session();
	bool is_open() const;
	int session_fd() const;

	int set_target(pid_t tgid, pid_t tid = 0);
	int get_status(struct lkmdbg_status_reply *reply_out);
	int get_event_config(struct lkmdbg_event_config_request *reply_out);
	int set_event_config(struct lkmdbg_event_config_request *req_out);
	int read_memory(uintptr_t remote_addr, void *buf, size_t len,
			uint32_t flags = 0);
	int write_memory(uintptr_t remote_addr, const void *buf, size_t len,
			 uint32_t flags = 0);

private:
	int session_fd_;
};

} // namespace driver
} // namespace lkmdbg

#endif
