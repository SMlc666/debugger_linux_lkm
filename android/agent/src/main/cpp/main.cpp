#include "lkmdbg_agent_protocol.h"

#include "include/lkmdbg_ioctl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

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

static void copy_cstr(char *dst, size_t dst_size, const std::string &src)
{
	if (!dst || !dst_size)
		return;

	memset(dst, 0, dst_size);
	if (src.empty())
		return;
	strncpy(dst, src.c_str(), dst_size - 1);
}

static bool parse_pid_name(const char *name, int *pid)
{
	char *end = nullptr;
	long value;

	if (!name || !pid || !name[0])
		return false;

	value = strtol(name, &end, 10);
	if (!end || *end != '\0' || value <= 0)
		return false;

	*pid = static_cast<int>(value);
	return true;
}

static std::string trim_trailing_space(std::string text)
{
	while (!text.empty() &&
	       (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' ||
		text.back() == '\t' || text.back() == '\0')) {
		text.pop_back();
	}
	return text;
}

static std::string read_proc_text(const std::string &path, bool cmdline_mode)
{
	int fd;
	ssize_t nr;
	char buf[512];
	std::string out;

	fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return {};

	for (;;) {
		nr = read(fd, buf, sizeof(buf));
		if (nr <= 0)
			break;
		if (!cmdline_mode) {
			out.append(buf, static_cast<size_t>(nr));
			continue;
		}

		for (ssize_t i = 0; i < nr; ++i)
			out.push_back(buf[i] == '\0' ? ' ' : buf[i]);
	}

	close(fd);
	return trim_trailing_space(out);
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
	reply.feature_bits = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3);
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

static bool handle_query_processes(void)
{
	struct proc_entry {
		int pid;
		int uid;
		std::string comm;
		std::string cmdline;
	};

	DIR *dir = nullptr;
	struct dirent *entry;
	std::vector<proc_entry> processes;
	lkmdbg_agent_query_processes_reply reply = {};
	std::vector<char> payload;

	dir = opendir("/proc");
	if (!dir) {
		reply.status = -errno;
		copy_cstr(reply.message, sizeof(reply.message), "open /proc failed");
		return write_frame(LKMDBG_AGENT_CMD_QUERY_PROCESSES, &reply, sizeof(reply));
	}

	while ((entry = readdir(dir)) != nullptr) {
		struct stat st = {};
		int pid;
		std::string base;
		std::string comm;
		std::string cmdline;

		if (!parse_pid_name(entry->d_name, &pid))
			continue;

		base = std::string("/proc/") + entry->d_name;
		if (stat(base.c_str(), &st) < 0)
			continue;

		comm = read_proc_text(base + "/comm", false);
		cmdline = read_proc_text(base + "/cmdline", true);
		if (cmdline.empty())
			cmdline = comm;
		if (comm.empty())
			comm = cmdline;

		processes.push_back(proc_entry{
			.pid = pid,
			.uid = static_cast<int>(st.st_uid),
			.comm = comm,
			.cmdline = cmdline,
		});
	}

	closedir(dir);
	std::sort(processes.begin(), processes.end(),
		  [](const proc_entry &lhs, const proc_entry &rhs) { return lhs.pid < rhs.pid; });

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.count = static_cast<uint32_t>(processes.size());
	copy_cstr(reply.message, sizeof(reply.message), "process list ready");

	payload.resize(sizeof(reply) +
		       processes.size() * sizeof(lkmdbg_agent_process_record));
	memcpy(payload.data(), &reply, sizeof(reply));
	for (size_t i = 0; i < processes.size(); ++i) {
		lkmdbg_agent_process_record record = {};
		const proc_entry &process = processes[i];
		char *dst = payload.data() + sizeof(reply) +
			    i * sizeof(lkmdbg_agent_process_record);

		record.pid = process.pid;
		record.uid = process.uid;
		copy_cstr(record.comm, sizeof(record.comm), process.comm);
		copy_cstr(record.cmdline, sizeof(record.cmdline), process.cmdline);
		memcpy(dst, &record, sizeof(record));
	}

	return write_frame(LKMDBG_AGENT_CMD_QUERY_PROCESSES, payload.data(),
			   static_cast<uint32_t>(payload.size()));
}

} // namespace

int main(int argc, char **argv)
{
	agent_state state = {};

	(void)argc;
	(void)argv;

	for (;;) {
		lkmdbg_agent_frame_header header = {};
		std::vector<char> payload;
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
		remaining = header.payload_size;
		payload.resize(remaining);
		if (remaining && !read_full(STDIN_FILENO, payload.data(), remaining)) {
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
			if (!handle_set_target(&state, payload.data(), remaining)) {
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
		case LKMDBG_AGENT_CMD_QUERY_PROCESSES:
			if (!handle_query_processes()) {
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
