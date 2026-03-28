#include "lkmdbg_agent_protocol.h"

#include "include/lkmdbg_ioctl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

static const char *image_name_ptr(const lkmdbg_image_query_request *reply,
				  const lkmdbg_image_entry *entry,
				  const char *names)
{
	if (!entry->name_size)
		return "";
	if (static_cast<uint64_t>(entry->name_offset) + entry->name_size >
	    reply->names_used)
		return "";
	return names + entry->name_offset;
}

static const char *vma_name_ptr(const lkmdbg_vma_query_request *reply,
				const lkmdbg_vma_entry *entry,
				const char *names)
{
	if (!entry->name_size)
		return "";
	if (static_cast<uint64_t>(entry->name_offset) + entry->name_size >
	    reply->names_used)
		return "";
	return names + entry->name_offset;
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

static bool handle_read_memory(agent_state *state, const void *payload, uint32_t payload_size)
{
	lkmdbg_agent_memory_request request = {};
	lkmdbg_agent_memory_reply reply = {};
	lkmdbg_mem_request req = {};
	lkmdbg_mem_op op = {};
	std::vector<char> data;
	std::vector<char> frame;

	if (payload_size < sizeof(request)) {
		reply.status = LKMDBG_AGENT_STATUS_INVALID_PAYLOAD;
		copy_cstr(reply.message, sizeof(reply.message), "payload too small");
		return write_frame(LKMDBG_AGENT_CMD_READ_MEMORY, &reply, sizeof(reply));
	}
	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_NO_SESSION;
		copy_cstr(reply.message, sizeof(reply.message), "session not open");
		return write_frame(LKMDBG_AGENT_CMD_READ_MEMORY, &reply, sizeof(reply));
	}

	memcpy(&request, payload, sizeof(request));
	if (!request.length || request.length > 65536u) {
		reply.status = -EINVAL;
		reply.remote_addr = request.remote_addr;
		reply.requested_length = request.length;
		reply.flags = request.flags;
		copy_cstr(reply.message, sizeof(reply.message), "invalid read length");
		return write_frame(LKMDBG_AGENT_CMD_READ_MEMORY, &reply, sizeof(reply));
	}

	data.resize(request.length);
	op.remote_addr = request.remote_addr;
	op.local_addr = reinterpret_cast<uintptr_t>(data.data());
	op.length = request.length;
	op.flags = request.flags;
	req.version = LKMDBG_PROTO_VERSION;
	req.size = sizeof(req);
	req.ops_addr = reinterpret_cast<uintptr_t>(&op);
	req.op_count = 1;
	if (ioctl(state->session_fd, LKMDBG_IOC_READ_MEM, &req) < 0) {
		reply.status = -errno;
		reply.remote_addr = request.remote_addr;
		reply.requested_length = request.length;
		reply.flags = request.flags;
		copy_cstr(reply.message, sizeof(reply.message), "READ_MEM failed");
		return write_frame(LKMDBG_AGENT_CMD_READ_MEMORY, &reply, sizeof(reply));
	}

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.bytes_done = op.bytes_done;
	reply.remote_addr = request.remote_addr;
	reply.requested_length = request.length;
	reply.flags = request.flags;
	copy_cstr(reply.message, sizeof(reply.message), "memory read ready");
	frame.resize(sizeof(reply) + op.bytes_done);
	memcpy(frame.data(), &reply, sizeof(reply));
	if (op.bytes_done)
		memcpy(frame.data() + sizeof(reply), data.data(), op.bytes_done);
	return write_frame(LKMDBG_AGENT_CMD_READ_MEMORY, frame.data(),
			   static_cast<uint32_t>(frame.size()));
}

static bool handle_write_memory(agent_state *state, const void *payload, uint32_t payload_size)
{
	lkmdbg_agent_memory_request request = {};
	lkmdbg_agent_memory_reply reply = {};
	lkmdbg_mem_request req = {};
	lkmdbg_mem_op op = {};
	const char *bytes;

	if (payload_size < sizeof(request)) {
		reply.status = LKMDBG_AGENT_STATUS_INVALID_PAYLOAD;
		copy_cstr(reply.message, sizeof(reply.message), "payload too small");
		return write_frame(LKMDBG_AGENT_CMD_WRITE_MEMORY, &reply, sizeof(reply));
	}
	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_NO_SESSION;
		copy_cstr(reply.message, sizeof(reply.message), "session not open");
		return write_frame(LKMDBG_AGENT_CMD_WRITE_MEMORY, &reply, sizeof(reply));
	}

	memcpy(&request, payload, sizeof(request));
	bytes = static_cast<const char *>(payload) + sizeof(request);
	if (!request.length || request.length > 65536u ||
	    payload_size < sizeof(request) + request.length) {
		reply.status = -EINVAL;
		reply.remote_addr = request.remote_addr;
		reply.requested_length = request.length;
		reply.flags = request.flags;
		copy_cstr(reply.message, sizeof(reply.message), "invalid write payload");
		return write_frame(LKMDBG_AGENT_CMD_WRITE_MEMORY, &reply, sizeof(reply));
	}

	op.remote_addr = request.remote_addr;
	op.local_addr = reinterpret_cast<uintptr_t>(bytes);
	op.length = request.length;
	op.flags = request.flags;
	req.version = LKMDBG_PROTO_VERSION;
	req.size = sizeof(req);
	req.ops_addr = reinterpret_cast<uintptr_t>(&op);
	req.op_count = 1;
	if (ioctl(state->session_fd, LKMDBG_IOC_WRITE_MEM, &req) < 0) {
		reply.status = -errno;
		reply.remote_addr = request.remote_addr;
		reply.requested_length = request.length;
		reply.flags = request.flags;
		copy_cstr(reply.message, sizeof(reply.message), "WRITE_MEM failed");
		return write_frame(LKMDBG_AGENT_CMD_WRITE_MEMORY, &reply, sizeof(reply));
	}

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.bytes_done = op.bytes_done;
	reply.remote_addr = request.remote_addr;
	reply.requested_length = request.length;
	reply.flags = request.flags;
	copy_cstr(reply.message, sizeof(reply.message), "memory write complete");
	return write_frame(LKMDBG_AGENT_CMD_WRITE_MEMORY, &reply, sizeof(reply));
}

static bool handle_query_threads(agent_state *state)
{
	lkmdbg_agent_query_threads_reply reply = {};
	std::vector<char> payload;
	std::vector<lkmdbg_agent_thread_record> records;
	int32_t cursor = 0;

	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_NO_SESSION;
		copy_cstr(reply.message, sizeof(reply.message), "session not open");
		return write_frame(LKMDBG_AGENT_CMD_QUERY_THREADS, &reply, sizeof(reply));
	}

	for (;;) {
		lkmdbg_thread_query_request req = {};
		lkmdbg_thread_entry entries[64] = {};

		req.version = LKMDBG_PROTO_VERSION;
		req.size = sizeof(req);
		req.entries_addr = reinterpret_cast<uintptr_t>(entries);
		req.max_entries = static_cast<uint32_t>(sizeof(entries) / sizeof(entries[0]));
		req.start_tid = cursor;

		if (ioctl(state->session_fd, LKMDBG_IOC_QUERY_THREADS, &req) < 0) {
			reply.status = -errno;
			copy_cstr(reply.message, sizeof(reply.message), "QUERY_THREADS failed");
			return write_frame(LKMDBG_AGENT_CMD_QUERY_THREADS, &reply, sizeof(reply));
		}

		for (uint32_t i = 0; i < req.entries_filled; ++i) {
			lkmdbg_agent_thread_record record = {};

			record.tid = entries[i].tid;
			record.tgid = entries[i].tgid;
			record.flags = entries[i].flags;
			record.user_pc = entries[i].user_pc;
			record.user_sp = entries[i].user_sp;
			memcpy(record.comm, entries[i].comm, sizeof(record.comm));
			records.push_back(record);
		}

		reply.done = req.done;
		reply.next_tid = req.next_tid;
		if (req.done)
			break;
		cursor = req.next_tid;
	}

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.count = static_cast<uint32_t>(records.size());
	copy_cstr(reply.message, sizeof(reply.message), "threads ready");
	payload.resize(sizeof(reply) + records.size() * sizeof(records[0]));
	memcpy(payload.data(), &reply, sizeof(reply));
	if (!records.empty()) {
		memcpy(payload.data() + sizeof(reply), records.data(),
		       records.size() * sizeof(records[0]));
	}
	return write_frame(LKMDBG_AGENT_CMD_QUERY_THREADS, payload.data(),
			   static_cast<uint32_t>(payload.size()));
}

static bool handle_get_registers(agent_state *state, const void *payload, uint32_t payload_size)
{
	lkmdbg_agent_get_registers_reply reply = {};
	lkmdbg_agent_thread_request request = {};
	lkmdbg_thread_regs_request req = {};

	if (payload_size < sizeof(request)) {
		reply.status = LKMDBG_AGENT_STATUS_INVALID_PAYLOAD;
		copy_cstr(reply.message, sizeof(reply.message), "payload too small");
		return write_frame(LKMDBG_AGENT_CMD_GET_REGISTERS, &reply, sizeof(reply));
	}
	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_NO_SESSION;
		copy_cstr(reply.message, sizeof(reply.message), "session not open");
		return write_frame(LKMDBG_AGENT_CMD_GET_REGISTERS, &reply, sizeof(reply));
	}

	memcpy(&request, payload, sizeof(request));
	req.version = LKMDBG_PROTO_VERSION;
	req.size = sizeof(req);
	req.tid = request.tid;
	req.flags = request.flags;
	if (ioctl(state->session_fd, LKMDBG_IOC_GET_REGS, &req) < 0) {
		reply.status = -errno;
		reply.tid = request.tid;
		copy_cstr(reply.message, sizeof(reply.message), "GET_REGS failed");
		return write_frame(LKMDBG_AGENT_CMD_GET_REGISTERS, &reply, sizeof(reply));
	}

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.tid = req.tid;
	reply.flags = req.flags;
	memcpy(reply.regs, req.regs.regs, sizeof(reply.regs));
	reply.sp = req.regs.sp;
	reply.pc = req.regs.pc;
	reply.pstate = req.regs.pstate;
	reply.features = req.regs.features;
	reply.fpsr = req.regs.fpsr;
	reply.fpcr = req.regs.fpcr;
	reply.v0_lo = req.regs.vregs[0].lo;
	reply.v0_hi = req.regs.vregs[0].hi;
	copy_cstr(reply.message, sizeof(reply.message), "registers ready");
	return write_frame(LKMDBG_AGENT_CMD_GET_REGISTERS, &reply, sizeof(reply));
}

static bool handle_poll_events(agent_state *state, const void *payload, uint32_t payload_size)
{
	lkmdbg_agent_poll_events_request request = {};
	lkmdbg_agent_poll_events_reply reply = {};
	std::vector<lkmdbg_event_record> records;
	std::vector<char> frame;
	uint32_t max_events;

	if (payload_size < sizeof(request)) {
		reply.status = LKMDBG_AGENT_STATUS_INVALID_PAYLOAD;
		copy_cstr(reply.message, sizeof(reply.message), "payload too small");
		return write_frame(LKMDBG_AGENT_CMD_POLL_EVENTS, &reply, sizeof(reply));
	}
	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_NO_SESSION;
		copy_cstr(reply.message, sizeof(reply.message), "session not open");
		return write_frame(LKMDBG_AGENT_CMD_POLL_EVENTS, &reply, sizeof(reply));
	}

	memcpy(&request, payload, sizeof(request));
	max_events = request.max_events ? request.max_events : 1u;
	if (max_events > 64u)
		max_events = 64u;

	for (;;) {
		pollfd pfd = {
			.fd = state->session_fd,
			.events = POLLIN,
			.revents = 0,
		};
		int poll_timeout = records.empty() ? static_cast<int>(request.timeout_ms) : 0;
		int poll_ret = poll(&pfd, 1, poll_timeout);
		ssize_t nread;
		size_t count;
		size_t remaining;

		if (poll_ret < 0) {
			reply.status = -errno;
			copy_cstr(reply.message, sizeof(reply.message), "poll failed");
			return write_frame(LKMDBG_AGENT_CMD_POLL_EVENTS, &reply, sizeof(reply));
		}
		if (!(pfd.revents & POLLIN))
			break;

		remaining = static_cast<size_t>(max_events) - records.size();
		std::vector<lkmdbg_event_record> batch(remaining);
		nread = read(state->session_fd, batch.data(),
			     batch.size() * sizeof(batch[0]));
		if (nread < 0) {
			reply.status = -errno;
			copy_cstr(reply.message, sizeof(reply.message), "event read failed");
			return write_frame(LKMDBG_AGENT_CMD_POLL_EVENTS, &reply, sizeof(reply));
		}
		if (nread == 0)
			break;
		if (nread % static_cast<ssize_t>(sizeof(batch[0])) != 0) {
			reply.status = LKMDBG_AGENT_STATUS_INTERNAL;
			copy_cstr(reply.message, sizeof(reply.message), "short event read");
			return write_frame(LKMDBG_AGENT_CMD_POLL_EVENTS, &reply, sizeof(reply));
		}

		count = static_cast<size_t>(nread) / sizeof(batch[0]);
		records.insert(records.end(), batch.begin(), batch.begin() + count);
		if (records.size() >= max_events)
			break;
	}

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.count = static_cast<uint32_t>(records.size());
	copy_cstr(reply.message, sizeof(reply.message),
		  records.empty() ? "no pending events" : "events ready");
	frame.resize(sizeof(reply) + records.size() * sizeof(records[0]));
	memcpy(frame.data(), &reply, sizeof(reply));
	if (!records.empty()) {
		memcpy(frame.data() + sizeof(reply), records.data(),
		       records.size() * sizeof(records[0]));
	}
	return write_frame(LKMDBG_AGENT_CMD_POLL_EVENTS, frame.data(),
			   static_cast<uint32_t>(frame.size()));
}

static bool handle_query_images(agent_state *state)
{
	lkmdbg_agent_query_images_reply reply = {};
	std::vector<lkmdbg_agent_image_record> records;
	std::vector<char> payload;
	uint64_t cursor = 0;

	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_NO_SESSION;
		copy_cstr(reply.message, sizeof(reply.message), "session not open");
		return write_frame(LKMDBG_AGENT_CMD_QUERY_IMAGES, &reply, sizeof(reply));
	}

	for (;;) {
		lkmdbg_image_entry entries[64] = {};
		char names[8192] = {};
		lkmdbg_image_query_request req = {};

		req.version = LKMDBG_PROTO_VERSION;
		req.size = sizeof(req);
		req.start_addr = cursor;
		req.entries_addr = reinterpret_cast<uintptr_t>(entries);
		req.max_entries = static_cast<uint32_t>(sizeof(entries) / sizeof(entries[0]));
		req.names_addr = reinterpret_cast<uintptr_t>(names);
		req.names_size = sizeof(names);
		if (ioctl(state->session_fd, LKMDBG_IOC_QUERY_IMAGES, &req) < 0) {
			reply.status = -errno;
			copy_cstr(reply.message, sizeof(reply.message), "QUERY_IMAGES failed");
			return write_frame(LKMDBG_AGENT_CMD_QUERY_IMAGES, &reply, sizeof(reply));
		}

		for (uint32_t i = 0; i < req.entries_filled; ++i) {
			lkmdbg_agent_image_record record = {};
			record.start_addr = entries[i].start_addr;
			record.end_addr = entries[i].end_addr;
			record.base_addr = entries[i].base_addr;
			record.pgoff = entries[i].pgoff;
			record.inode = entries[i].inode;
			record.prot = entries[i].prot;
			record.flags = entries[i].flags;
			record.dev_major = entries[i].dev_major;
			record.dev_minor = entries[i].dev_minor;
			record.segment_count = entries[i].segment_count;
			copy_cstr(record.name, sizeof(record.name),
				  image_name_ptr(&req, &entries[i], names));
			records.push_back(record);
		}

		if (req.done)
			break;
		if (req.next_addr <= cursor) {
			reply.status = -EIO;
			copy_cstr(reply.message, sizeof(reply.message), "QUERY_IMAGES cursor stalled");
			return write_frame(LKMDBG_AGENT_CMD_QUERY_IMAGES, &reply, sizeof(reply));
		}
		cursor = req.next_addr;
	}

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.count = static_cast<uint32_t>(records.size());
	copy_cstr(reply.message, sizeof(reply.message), "images ready");
	payload.resize(sizeof(reply) + records.size() * sizeof(records[0]));
	memcpy(payload.data(), &reply, sizeof(reply));
	if (!records.empty()) {
		memcpy(payload.data() + sizeof(reply), records.data(),
		       records.size() * sizeof(records[0]));
	}
	return write_frame(LKMDBG_AGENT_CMD_QUERY_IMAGES, payload.data(),
			   static_cast<uint32_t>(payload.size()));
}

static bool handle_query_vmas(agent_state *state)
{
	lkmdbg_agent_query_vmas_reply reply = {};
	std::vector<lkmdbg_agent_vma_record> records;
	std::vector<char> payload;
	uint64_t cursor = 0;

	if (state->session_fd < 0) {
		reply.status = LKMDBG_AGENT_STATUS_NO_SESSION;
		copy_cstr(reply.message, sizeof(reply.message), "session not open");
		return write_frame(LKMDBG_AGENT_CMD_QUERY_VMAS, &reply, sizeof(reply));
	}

	for (;;) {
		lkmdbg_vma_entry entries[64] = {};
		char names[8192] = {};
		lkmdbg_vma_query_request req = {};

		req.version = LKMDBG_PROTO_VERSION;
		req.size = sizeof(req);
		req.start_addr = cursor;
		req.entries_addr = reinterpret_cast<uintptr_t>(entries);
		req.max_entries = static_cast<uint32_t>(sizeof(entries) / sizeof(entries[0]));
		req.names_addr = reinterpret_cast<uintptr_t>(names);
		req.names_size = sizeof(names);
		if (ioctl(state->session_fd, LKMDBG_IOC_QUERY_VMAS, &req) < 0) {
			reply.status = -errno;
			copy_cstr(reply.message, sizeof(reply.message), "QUERY_VMAS failed");
			return write_frame(LKMDBG_AGENT_CMD_QUERY_VMAS, &reply, sizeof(reply));
		}

		for (uint32_t i = 0; i < req.entries_filled; ++i) {
			lkmdbg_agent_vma_record record = {};
			record.start_addr = entries[i].start_addr;
			record.end_addr = entries[i].end_addr;
			record.pgoff = entries[i].pgoff;
			record.inode = entries[i].inode;
			record.vm_flags_raw = entries[i].vm_flags_raw;
			record.prot = entries[i].prot;
			record.flags = entries[i].flags;
			record.dev_major = entries[i].dev_major;
			record.dev_minor = entries[i].dev_minor;
			copy_cstr(record.name, sizeof(record.name),
				  vma_name_ptr(&req, &entries[i], names));
			records.push_back(record);
		}

		if (req.done)
			break;
		if (req.next_addr <= cursor) {
			reply.status = -EIO;
			copy_cstr(reply.message, sizeof(reply.message), "QUERY_VMAS cursor stalled");
			return write_frame(LKMDBG_AGENT_CMD_QUERY_VMAS, &reply, sizeof(reply));
		}
		cursor = req.next_addr;
	}

	reply.status = LKMDBG_AGENT_STATUS_OK;
	reply.count = static_cast<uint32_t>(records.size());
	copy_cstr(reply.message, sizeof(reply.message), "vmas ready");
	payload.resize(sizeof(reply) + records.size() * sizeof(records[0]));
	memcpy(payload.data(), &reply, sizeof(reply));
	if (!records.empty()) {
		memcpy(payload.data() + sizeof(reply), records.data(),
		       records.size() * sizeof(records[0]));
	}
	return write_frame(LKMDBG_AGENT_CMD_QUERY_VMAS, payload.data(),
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
		case LKMDBG_AGENT_CMD_READ_MEMORY:
			if (!handle_read_memory(&state, payload.data(), remaining)) {
				close_session(&state);
				return 1;
			}
			break;
		case LKMDBG_AGENT_CMD_WRITE_MEMORY:
			if (!handle_write_memory(&state, payload.data(), remaining)) {
				close_session(&state);
				return 1;
			}
			break;
		case LKMDBG_AGENT_CMD_QUERY_THREADS:
			if (!handle_query_threads(&state)) {
				close_session(&state);
				return 1;
			}
			break;
		case LKMDBG_AGENT_CMD_GET_REGISTERS:
			if (!handle_get_registers(&state, payload.data(), remaining)) {
				close_session(&state);
				return 1;
			}
			break;
		case LKMDBG_AGENT_CMD_POLL_EVENTS:
			if (!handle_poll_events(&state, payload.data(), remaining)) {
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
		case LKMDBG_AGENT_CMD_QUERY_IMAGES:
			if (!handle_query_images(&state)) {
				close_session(&state);
				return 1;
			}
			break;
		case LKMDBG_AGENT_CMD_QUERY_VMAS:
			if (!handle_query_vmas(&state)) {
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
