#ifndef LKMDBG_AGENT_PROTOCOL_H
#define LKMDBG_AGENT_PROTOCOL_H

#include <stdint.h>

static const uint32_t LKMDBG_AGENT_MAGIC = 0x4C4B4D44u;
static const uint32_t LKMDBG_AGENT_VERSION = 1u;

enum lkmdbg_agent_command {
	LKMDBG_AGENT_CMD_HELLO = 1u,
	LKMDBG_AGENT_CMD_OPEN_SESSION = 2u,
	LKMDBG_AGENT_CMD_SET_TARGET = 3u,
	LKMDBG_AGENT_CMD_READ_MEMORY = 4u,
	LKMDBG_AGENT_CMD_WRITE_MEMORY = 5u,
	LKMDBG_AGENT_CMD_QUERY_THREADS = 6u,
	LKMDBG_AGENT_CMD_GET_REGISTERS = 7u,
	LKMDBG_AGENT_CMD_POLL_EVENTS = 9u,
	LKMDBG_AGENT_CMD_STATUS_SNAPSHOT = 10u,
	LKMDBG_AGENT_CMD_QUERY_PROCESSES = 11u,
	LKMDBG_AGENT_CMD_QUERY_IMAGES = 12u,
	LKMDBG_AGENT_CMD_QUERY_VMAS = 13u,
	LKMDBG_AGENT_CMD_SEARCH_MEMORY = 14u,
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

struct __attribute__((packed)) lkmdbg_agent_query_processes_reply {
	int32_t status;
	uint32_t count;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_process_record {
	int32_t pid;
	int32_t uid;
	char comm[64];
	char cmdline[128];
};

struct __attribute__((packed)) lkmdbg_agent_memory_request {
	uint64_t remote_addr;
	uint32_t length;
	uint32_t flags;
};

struct __attribute__((packed)) lkmdbg_agent_memory_reply {
	int32_t status;
	uint32_t bytes_done;
	uint64_t remote_addr;
	uint32_t requested_length;
	uint32_t flags;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_query_threads_reply {
	int32_t status;
	uint32_t count;
	int32_t next_tid;
	uint32_t done;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_thread_record {
	int32_t tid;
	int32_t tgid;
	uint32_t flags;
	uint32_t reserved0;
	uint64_t user_pc;
	uint64_t user_sp;
	char comm[16];
};

struct __attribute__((packed)) lkmdbg_agent_thread_request {
	int32_t tid;
	uint32_t flags;
};

struct __attribute__((packed)) lkmdbg_agent_get_registers_reply {
	int32_t status;
	int32_t tid;
	uint32_t flags;
	uint32_t reserved0;
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint32_t features;
	uint32_t fpsr;
	uint32_t fpcr;
	uint32_t reserved1;
	uint64_t v0_lo;
	uint64_t v0_hi;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_poll_events_request {
	uint32_t timeout_ms;
	uint32_t max_events;
};

struct __attribute__((packed)) lkmdbg_agent_poll_events_reply {
	int32_t status;
	uint32_t count;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_query_images_reply {
	int32_t status;
	uint32_t count;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_query_vmas_reply {
	int32_t status;
	uint32_t count;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_search_memory_request {
	uint32_t region_preset;
	uint32_t max_results;
	uint32_t pattern_size;
	uint32_t reserved0;
	uint8_t pattern[128];
};

struct __attribute__((packed)) lkmdbg_agent_search_memory_reply {
	int32_t status;
	uint32_t count;
	uint32_t searched_vma_count;
	uint32_t reserved0;
	uint64_t scanned_bytes;
	char message[64];
};

struct __attribute__((packed)) lkmdbg_agent_image_record {
	uint64_t start_addr;
	uint64_t end_addr;
	uint64_t base_addr;
	uint64_t pgoff;
	uint64_t inode;
	uint32_t prot;
	uint32_t flags;
	uint32_t dev_major;
	uint32_t dev_minor;
	uint32_t segment_count;
	uint32_t reserved0;
	char name[256];
};

struct __attribute__((packed)) lkmdbg_agent_vma_record {
	uint64_t start_addr;
	uint64_t end_addr;
	uint64_t pgoff;
	uint64_t inode;
	uint64_t vm_flags_raw;
	uint32_t prot;
	uint32_t flags;
	uint32_t dev_major;
	uint32_t dev_minor;
	uint32_t reserved0;
	uint32_t reserved1;
	char name[256];
};

struct __attribute__((packed)) lkmdbg_agent_search_result_record {
	uint64_t address;
	uint64_t region_start;
	uint64_t region_end;
	uint32_t preview_size;
	uint32_t reserved0;
	uint8_t preview[32];
};

static_assert(sizeof(struct lkmdbg_agent_hello_reply) == 80, "hello reply size");
static_assert(sizeof(struct lkmdbg_agent_open_session_reply) == 80, "open-session reply size");
static_assert(sizeof(struct lkmdbg_agent_set_target_request) == 8, "set-target request size");
static_assert(sizeof(struct lkmdbg_agent_set_target_reply) == 72, "set-target reply size");
static_assert(sizeof(struct lkmdbg_agent_status_snapshot) == 140, "status snapshot size");
static_assert(sizeof(struct lkmdbg_agent_query_processes_reply) == 72, "query-processes reply size");
static_assert(sizeof(struct lkmdbg_agent_process_record) == 200, "process record size");
static_assert(sizeof(struct lkmdbg_agent_query_threads_reply) == 80, "query-threads reply size");
static_assert(sizeof(struct lkmdbg_agent_thread_record) == 48, "thread record size");
static_assert(sizeof(struct lkmdbg_agent_thread_request) == 8, "thread request size");
static_assert(sizeof(struct lkmdbg_agent_get_registers_reply) == 384, "get-registers reply size");
static_assert(sizeof(struct lkmdbg_agent_poll_events_request) == 8, "poll-events request size");
static_assert(sizeof(struct lkmdbg_agent_poll_events_reply) == 72, "poll-events reply size");
static_assert(sizeof(struct lkmdbg_agent_memory_request) == 16, "memory request size");
static_assert(sizeof(struct lkmdbg_agent_memory_reply) == 88, "memory reply size");
static_assert(sizeof(struct lkmdbg_agent_query_images_reply) == 72, "query-images reply size");
static_assert(sizeof(struct lkmdbg_agent_image_record) == 320, "image record size");
static_assert(sizeof(struct lkmdbg_agent_query_vmas_reply) == 72, "query-vmas reply size");
static_assert(sizeof(struct lkmdbg_agent_vma_record) == 320, "vma record size");
static_assert(sizeof(struct lkmdbg_agent_search_memory_request) == 144, "search-memory request size");
static_assert(sizeof(struct lkmdbg_agent_search_memory_reply) == 88, "search-memory reply size");
static_assert(sizeof(struct lkmdbg_agent_search_result_record) == 64, "search result record size");

#endif
