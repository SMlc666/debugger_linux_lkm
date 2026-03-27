#include "driver.hpp"

#include <errno.h>
#include <unistd.h>

#include "events.hpp"
#include "memory.hpp"
#include "session.hpp"

namespace lkmdbg {
namespace driver {

Driver::Driver() : session_fd_(-1)
{
}

Driver::~Driver()
{
	close_session();
}

int Driver::open_session()
{
	int fd;

	if (is_open()) {
		errno = EBUSY;
		return -1;
	}

	fd = session::open_session_fd();
	if (fd < 0)
		return -1;

	session_fd_ = fd;
	return 0;
}

void Driver::close_session()
{
	if (!is_open())
		return;

	close(session_fd_);
	session_fd_ = -1;
}

bool Driver::is_open() const
{
	return session_fd_ >= 0;
}

int Driver::session_fd() const
{
	return session_fd_;
}

int Driver::set_target(pid_t tgid, pid_t tid)
{
	if (!is_open()) {
		errno = ENOTCONN;
		return -1;
	}

	return session::set_target_ex(session_fd_, tgid, tid);
}

int Driver::get_status(struct lkmdbg_status_reply *reply_out)
{
	if (!is_open()) {
		errno = ENOTCONN;
		return -1;
	}

	return session::get_status(session_fd_, reply_out);
}

int Driver::get_event_config(struct lkmdbg_event_config_request *reply_out)
{
	if (!is_open()) {
		errno = ENOTCONN;
		return -1;
	}

	return events::get_event_config(session_fd_, reply_out);
}

int Driver::set_event_config(struct lkmdbg_event_config_request *req_out)
{
	if (!is_open()) {
		errno = ENOTCONN;
		return -1;
	}

	return events::set_event_config(session_fd_, req_out);
}

int Driver::read_memory(uintptr_t remote_addr, void *buf, size_t len,
			uint32_t flags)
{
	if (!is_open()) {
		errno = ENOTCONN;
		return -1;
	}

	return memory::read_memory(session_fd_, remote_addr, buf, len, flags);
}

int Driver::write_memory(uintptr_t remote_addr, const void *buf, size_t len,
			 uint32_t flags)
{
	if (!is_open()) {
		errno = ENOTCONN;
		return -1;
	}

	return memory::write_memory(session_fd_, remote_addr, buf, len, flags);
}

} // namespace driver
} // namespace lkmdbg
