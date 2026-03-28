#include "lkmdbg_agent_protocol.h"

#include <string.h>
#include <unistd.h>

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

static bool handle_hello(void)
{
	lkmdbg_agent_hello_reply reply = {};

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.server_version = LKMDBG_AGENT_VERSION;
	reply.feature_bits = 0;
	strncpy(reply.message, "lkmdbg-agent skeleton", sizeof(reply.message) - 1);
	return write_frame(LKMDBG_AGENT_CMD_HELLO, &reply, sizeof(reply));
}

static bool handle_status_snapshot(void)
{
	lkmdbg_agent_status_snapshot reply = {};

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.connected = 0;
	reply.target_pid = 0;
	reply.target_tid = 0;
	reply.session_open = 0;
	reply.agent_pid = static_cast<int32_t>(getpid());
	strncpy(reply.transport, "stdio-pipe", sizeof(reply.transport) - 1);
	return write_frame(LKMDBG_AGENT_CMD_STATUS_SNAPSHOT, &reply, sizeof(reply));
}

int main(void)
{
	for (;;) {
		lkmdbg_agent_frame_header header = {};
		char discard[256];
		size_t remaining;

		if (!read_full(STDIN_FILENO, &header, sizeof(header)))
			return 0;
		if (header.magic != LKMDBG_AGENT_MAGIC ||
		    header.version != LKMDBG_AGENT_VERSION)
			return 1;

		remaining = header.payload_size;
		while (remaining > 0) {
			size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
			if (!read_full(STDIN_FILENO, discard, chunk))
				return 1;
			remaining -= chunk;
		}

		switch (header.command) {
		case LKMDBG_AGENT_CMD_HELLO:
			if (!handle_hello())
				return 1;
			break;
		case LKMDBG_AGENT_CMD_STATUS_SNAPSHOT:
			if (!handle_status_snapshot())
				return 1;
			break;
		default: {
			lkmdbg_agent_hello_reply reply = {};

			reply.status = LKMDBG_AGENT_STATUS_UNSUPPORTED;
			reply.server_version = LKMDBG_AGENT_VERSION;
			strncpy(reply.message, "unsupported command", sizeof(reply.message) - 1);
			if (!write_frame(header.command, &reply, sizeof(reply)))
				return 1;
			break;
		}
		}
	}
}
