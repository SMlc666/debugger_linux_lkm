#include "lkmdbg_agent_protocol.h"

#include "include/lkmdbg_ioctl.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

struct agent_state {
	int session_fd = -1;
	int target_pid = 0;
	int target_tid = 0;
};

static const char *k_bootstrap_path = "/proc/version";

static bool read_full(int fd, void *buf, size_t len)
{
	size_t done = 0;
	char *cursor = static_cast<char *>(buf);

	while (done < len) {
		ssize_t nr = read(fd, cursor + done, len - done);
		if (nr <= 0)
			return false;
		done += static_cast<size_t>(nr);
	}

	return true;
}

static bool write_full(int fd, const void *buf, size_t len)
{
	size_t done = 0;
	const char *cursor = static_cast<const char *>(buf);

	while (done < len) {
		ssize_t nw = write(fd, cursor + done, len - done);
		if (nw <= 0)
			return false;
		done += static_cast<size_t>(nw);
	}

	return true;
}

static bool write_frame(uint32_t command, const void *payload, uint32_t payload_size)
{
	lkmdbg_agent_frame_header header = {
		.magic = LKMDBG_AGENT_MAGIC,
		.version = LKMDBG_AGENT_VERSION,
		.command = command,
		.payload_size = payload_size,
	};

	if (!write_full(STDOUT_FILENO, &header, sizeof(header)))
		return false;
	if (payload_size && !write_full(STDOUT_FILENO, payload, payload_size))
		return false;

	return true;
}

static void close_session(agent_state *state)
{
	if (!state)
		return;
	if (state->session_fd >= 0) {
		close(state->session_fd);
		state->session_fd = -1;
	}
	state->target_pid = 0;
	state->target_tid = 0;
}

static bool handle_hello(void)
{
	lkmdbg_agent_hello_reply reply = {};

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.server_version = LKMDBG_AGENT_VERSION;
	reply.feature_bits = (1ULL << 0) | (1ULL << 1) | (1ULL << 2);
	strncpy(reply.message, "stdio bridge ready", sizeof(reply.message) - 1);
	return write_frame(LKMDBG_AGENT_CMD_HELLO, &reply, sizeof(reply));
}

static bool handle_open_session(agent_state *state)
{
	lkmdbg_agent_open_session_reply reply = {};
	struct lkmdbg_open_session_request req = {};
	int proc_fd;
	int session_fd;

	req.version = LKMDBG_PROTO_VERSION;
	req.size = sizeof(req);

	close_session(state);

	proc_fd = open(k_bootstrap_path, O_RDONLY | O_CLOEXEC);
	if (proc_fd < 0) {
		reply.status = -errno;
		strncpy(reply.message, "open bootstrap failed", sizeof(reply.message) - 1);
		return write_frame(LKMDBG_AGENT_CMD_OPEN_SESSION, &reply, sizeof(reply));
	}

	session_fd = ioctl(proc_fd, LKMDBG_IOC_OPEN_SESSION, &req);
	close(proc_fd);
	if (session_fd < 0) {
		reply.status = -errno;
		strncpy(reply.message, "OPEN_SESSION failed", sizeof(reply.message) - 1);
		return write_frame(LKMDBG_AGENT_CMD_OPEN_SESSION, &reply, sizeof(reply));
	}

	state->session_fd = session_fd;
	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.session_open = 1;
	reply.session_id = 0;
	strncpy(reply.message, "session opened", sizeof(reply.message) - 1);
	return write_frame(LKMDBG_AGENT_CMD_OPEN_SESSION, &reply, sizeof(reply));
}

static bool handle_set_target(agent_state *state, const void *payload, uint32_t payload_size)
{
	lkmdbg_agent_set_target_reply reply = {};
	lkmdbg_agent_set_target_request request = {};
	struct lkmdbg_target_request ioctl_req = {};

	ioctl_req.version = LKMDBG_PROTO_VERSION;
	ioctl_req.size = sizeof(ioctl_req);

	if (payload_size < sizeof(request)) {
		reply.status = LKMDBG_AGENT_STATUS_INVALID_PAYLOAD;
		strncpy(reply.message, "payload too small", sizeof(reply.message) - 1);
		return write_frame(LKMDBG_AGENT_CMD_SET_TARGET, &reply, sizeof(reply));
	}

	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_NO_SESSION;
		strncpy(reply.message, "session not open", sizeof(reply.message) - 1);
		return write_frame(LKMDBG_AGENT_CMD_SET_TARGET, &reply, sizeof(reply));
	}

	memcpy(&request, payload, sizeof(request));
	ioctl_req.tgid = request.target_pid;
	ioctl_req.tid = request.target_tid;
	if (ioctl(state->session_fd, LKMDBG_IOC_SET_TARGET, &ioctl_req) < 0) {
		reply.status = -errno;
		reply.target_pid = request.target_pid;
		reply.target_tid = request.target_tid;
		strncpy(reply.message, "SET_TARGET failed", sizeof(reply.message) - 1);
		return write_frame(LKMDBG_AGENT_CMD_SET_TARGET, &reply, sizeof(reply));
	}

	state->target_pid = request.target_pid;
	state->target_tid = request.target_tid;
	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.target_pid = request.target_pid;
	reply.target_tid = request.target_tid;
	strncpy(reply.message, "target attached", sizeof(reply.message) - 1);
	return write_frame(LKMDBG_AGENT_CMD_SET_TARGET, &reply, sizeof(reply));
}

static bool handle_status_snapshot(agent_state *state)
{
	lkmdbg_agent_status_snapshot reply = {};
	struct lkmdbg_status_reply status = {};

	status.version = LKMDBG_PROTO_VERSION;
	status.size = sizeof(status);

	reply.agent_pid = static_cast<int32_t>(getpid());
	strncpy(reply.transport, "su->stdio-pipe", sizeof(reply.transport) - 1);

	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_OK;
		strncpy(reply.message, "session closed", sizeof(reply.message) - 1);
		return write_frame(LKMDBG_AGENT_CMD_STATUS_SNAPSHOT, &reply, sizeof(reply));
	}

	if (ioctl(state->session_fd, LKMDBG_IOC_GET_STATUS, &status) < 0) {
		reply.status = -errno;
		reply.session_open = 1;
		strncpy(reply.message, "GET_STATUS failed", sizeof(reply.message) - 1);
		return write_frame(LKMDBG_AGENT_CMD_STATUS_SNAPSHOT, &reply, sizeof(reply));
	}

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.connected = 1;
	reply.target_pid = status.target_tgid;
	reply.target_tid = status.target_tid;
	reply.session_open = 1;
	reply.owner_pid = status.owner_tgid;
	reply.hook_active = static_cast<int32_t>(status.hook_active);
	reply.event_queue_depth = status.event_queue_depth;
	reply.session_id = status.session_id;
	strncpy(reply.message, "status snapshot ok", sizeof(reply.message) - 1);
	return write_frame(LKMDBG_AGENT_CMD_STATUS_SNAPSHOT, &reply, sizeof(reply));
}

} // namespace

int main(int argc, char **argv)
{
	agent_state state = {};

	(void)argc;
	(void)argv;

	for (;;) {
		lkmdbg_agent_frame_header header = {};
		char payload[256];
		uint32_t remaining;

		if (!read_full(STDIN_FILENO, &header, sizeof(header))) {
			close_session(&state);
			return 0;
		}
		if (header.magic != LKMDBG_AGENT_MAGIC ||
		    header.version != LKMDBG_AGENT_VERSION) {
			close_session(&state);
			return 1;
		}
		if (header.payload_size > sizeof(payload)) {
			close_session(&state);
			return 1;
		}

		remaining = header.payload_size;
		if (remaining && !read_full(STDIN_FILENO, payload, remaining)) {
			close_session(&state);
			return 1;
		}

		switch (header.command) {
		case LKMDBG_AGENT_CMD_HELLO:
			if (!handle_hello()) {
				close_session(&state);
				return 1;
			}
			break;
		case LKMDBG_AGENT_CMD_OPEN_SESSION:
			if (!handle_open_session(&state)) {
				close_session(&state);
				return 1;
			}
			break;
		case LKMDBG_AGENT_CMD_SET_TARGET:
			if (!handle_set_target(&state, payload, remaining)) {
				close_session(&state);
				return 1;
			}
			break;
		case LKMDBG_AGENT_CMD_STATUS_SNAPSHOT:
			if (!handle_status_snapshot(&state)) {
				close_session(&state);
				return 1;
			}
			break;
		default: {
			lkmdbg_agent_hello_reply reply = {};

			reply.status = LKMDBG_AGENT_STATUS_UNSUPPORTED;
			reply.server_version = LKMDBG_AGENT_VERSION;
			strncpy(reply.message, "unsupported command", sizeof(reply.message) - 1);
			if (!write_frame(header.command, &reply, sizeof(reply))) {
				close_session(&state);
				return 1;
			}
			break;
		}
		}
	}
}
