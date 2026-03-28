#ifndef LKMDBG_AGENT_PROTOCOL_H
#define LKMDBG_AGENT_PROTOCOL_H

#include <stdint.h>

static const uint32_t LKMDBG_AGENT_MAGIC = 0x4C4B4D44u;
static const uint32_t LKMDBG_AGENT_VERSION = 1u;

enum lkmdbg_agent_command {
	LKMDBG_AGENT_CMD_HELLO = 1u,
	LKMDBG_AGENT_CMD_OPEN_SESSION = 2u,
	LKMDBG_AGENT_CMD_SET_TARGET = 3u,
	LKMDBG_AGENT_CMD_STATUS_SNAPSHOT = 10u,
};

enum lkmdbg_agent_status {
	LKMDBG_AGENT_STATUS_OK = 0,
	LKMDBG_AGENT_STATUS_UNSUPPORTED = -1,
	LKMDBG_AGENT_STATUS_INVALID_HEADER = -2,
	LKMDBG_AGENT_STATUS_INVALID_PAYLOAD = -3,
	LKMDBG_AGENT_STATUS_INTERNAL = -4,
	LKMDBG_AGENT_STATUS_NO_SESSION = -5,
};

struct __attribute__((packed)) lkmdbg_agent_frame_header {
	uint32_t magic;
	uint32_t version;
	uint32_t command;
	uint32_t payload_size;
};

struct __attribute__((packed)) lkmdbg_agent_hello_reply {
	int32_t status;
	uint32_t server_version;
	uint64_t feature_bits;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_open_session_reply {
	int32_t status;
	int32_t session_open;
	uint64_t session_id;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_set_target_request {
	int32_t target_pid;
	int32_t target_tid;
};

struct __attribute__((packed)) lkmdbg_agent_set_target_reply {
	int32_t status;
	int32_t target_pid;
	int32_t target_tid;
	char message[60];
};

struct __attribute__((packed)) lkmdbg_agent_status_snapshot {
	int32_t status;
	int32_t connected;
	int32_t target_pid;
	int32_t target_tid;
	int32_t session_open;
	int32_t agent_pid;
	int32_t owner_pid;
	int32_t hook_active;
	uint32_t event_queue_depth;
	uint64_t session_id;
	char transport[32];
	char message[64];
};

static_assert(sizeof(struct lkmdbg_agent_hello_reply) == 80, "hello reply size");
static_assert(sizeof(struct lkmdbg_agent_open_session_reply) == 80, "open-session reply size");
static_assert(sizeof(struct lkmdbg_agent_set_target_request) == 8, "set-target request size");
static_assert(sizeof(struct lkmdbg_agent_set_target_reply) == 72, "set-target reply size");
static_assert(sizeof(struct lkmdbg_agent_status_snapshot) == 140, "status snapshot size");

#endif
