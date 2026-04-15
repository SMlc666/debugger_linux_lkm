#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/lkmdbg_ioctl.h"
#include "driver/bridge_c.h"
#include "driver/bridge_control.h"
#include "driver/common.hpp"

#define fprintf lkmdbg_fprintf

#define INPUT_QUERY_BATCH 32U

static int tool_open_session_fd(void)
{
	return bridge_open_session_fd();
}

static void print_device_entry(const struct lkmdbg_input_device_entry *entry)
{
	printf("device_id=%" PRIu64 " bustype=0x%x vendor=0x%x product=0x%x version=0x%x flags=0x%x\n",
	       (uint64_t)entry->device_id, entry->bustype, entry->vendor,
	       entry->product, entry->version_id, entry->flags);
	printf("name=%s\n", entry->name);
	printf("phys=%s\n", entry->phys);
	printf("uniq=%s\n", entry->uniq);
}

static int list_devices(int session_fd)
{
	struct lkmdbg_input_device_entry entries[INPUT_QUERY_BATCH];
	struct lkmdbg_input_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = INPUT_QUERY_BATCH,
	};

	for (;;) {
		memset(entries, 0, sizeof(entries));
		if (bridge_query_input_devices(session_fd, req.start_id, entries,
					       INPUT_QUERY_BATCH, req.flags,
					       &req) < 0) {
			fprintf(stderr, "QUERY_INPUT_DEVICES failed: %s\n",
				strerror(errno));
			return -1;
		}

		for (uint32_t i = 0; i < req.entries_filled; i++) {
			print_device_entry(&entries[i]);
			printf("\n");
		}

		if (req.done)
			break;

		req.start_id = req.next_id;
	}

	return 0;
}

static void print_absinfo(const struct lkmdbg_input_absinfo *absinfo,
			  unsigned int axis)
{
	if (!absinfo->minimum && !absinfo->maximum && !absinfo->resolution &&
	    !absinfo->fuzz && !absinfo->flat && !absinfo->value)
		return;

	printf("abs[%u]=value:%d min:%d max:%d fuzz:%d flat:%d res:%d\n", axis,
	       absinfo->value, absinfo->minimum, absinfo->maximum,
	       absinfo->fuzz, absinfo->flat, absinfo->resolution);
}

static int info_device(int session_fd, uint64_t device_id)
{
	struct lkmdbg_input_device_info_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.device_id = device_id,
	};

	if (bridge_get_input_device_info(session_fd, device_id, req.flags,
					 &req) < 0) {
		fprintf(stderr, "GET_INPUT_DEVICE_INFO failed: %s\n",
			strerror(errno));
		return -1;
	}

	print_device_entry(&req.entry);
	printf("supported_channel_flags=0x%x\n", req.supported_channel_flags);

	for (size_t i = 0; i < LKMDBG_INPUT_EV_WORDS; i++)
		printf("ev_bits[%zu]=0x%016" PRIx64 "\n", i,
		       (uint64_t)req.ev_bits[i]);
	for (size_t i = 0; i < LKMDBG_INPUT_KEY_WORDS; i++) {
		if (!req.key_bits[i])
			continue;
		printf("key_bits[%zu]=0x%016" PRIx64 "\n", i,
		       (uint64_t)req.key_bits[i]);
	}
	for (size_t i = 0; i < LKMDBG_INPUT_REL_WORDS; i++) {
		if (!req.rel_bits[i])
			continue;
		printf("rel_bits[%zu]=0x%016" PRIx64 "\n", i,
		       (uint64_t)req.rel_bits[i]);
	}
	for (size_t i = 0; i < LKMDBG_INPUT_ABS_WORDS; i++) {
		if (!req.abs_bits[i])
			continue;
		printf("abs_bits[%zu]=0x%016" PRIx64 "\n", i,
		       (uint64_t)req.abs_bits[i]);
	}
	for (size_t i = 0; i < LKMDBG_INPUT_PROP_WORDS; i++) {
		if (!req.prop_bits[i])
			continue;
		printf("prop_bits[%zu]=0x%016" PRIx64 "\n", i,
		       (uint64_t)req.prop_bits[i]);
	}
	for (unsigned int i = 0; i < LKMDBG_INPUT_ABS_COUNT; i++)
		print_absinfo(&req.absinfo[i], i);

	return 0;
}

static int open_input_channel(int session_fd, uint64_t device_id, uint32_t flags)
{
	struct lkmdbg_input_channel_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.device_id = device_id,
		.flags = flags,
		.channel_fd = -1,
	};

	if (bridge_open_input_channel(session_fd, device_id, flags, &req) < 0) {
		fprintf(stderr, "OPEN_INPUT_CHANNEL failed: %s\n",
			strerror(errno));
		return -1;
	}

	return req.channel_fd;
}

static int read_events(int session_fd, uint64_t device_id, unsigned int max_events,
		       int timeout_ms, uint32_t open_flags)
{
	struct lkmdbg_input_event events[32];
	struct pollfd pfd = { 0 };
	int channel_fd;
	ssize_t nread;

	if (!max_events || max_events > (sizeof(events) / sizeof(events[0])))
		max_events = sizeof(events) / sizeof(events[0]);

	channel_fd = open_input_channel(session_fd, device_id, open_flags);
	if (channel_fd < 0)
		return -1;

	pfd.fd = channel_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, timeout_ms) < 0) {
		fprintf(stderr, "poll failed: %s\n", strerror(errno));
		close(channel_fd);
		return -1;
	}
	if (!(pfd.revents & POLLIN)) {
		fprintf(stderr, "poll timed out revents=0x%x\n", pfd.revents);
		close(channel_fd);
		return -1;
	}

	nread = read(channel_fd, events, max_events * sizeof(events[0]));
	if (nread < 0) {
		fprintf(stderr, "read failed: %s\n", strerror(errno));
		close(channel_fd);
		return -1;
	}
	if (nread % (ssize_t)sizeof(events[0]) != 0) {
		fprintf(stderr, "short event read: %zd\n", nread);
		close(channel_fd);
		return -1;
	}

	for (size_t i = 0; i < (size_t)nread / sizeof(events[0]); i++) {
		printf("event[%zu]=seq:%" PRIu64 " ts:%" PRIu64 " type:%u code:%u value:%d flags:0x%x drops:%u\n",
		       i, (uint64_t)events[i].seq, (uint64_t)events[i].timestamp_ns,
		       events[i].type, events[i].code, events[i].value,
		       events[i].flags, events[i].reserved0);
	}

	close(channel_fd);
	return 0;
}

static int inject_events(int session_fd, uint64_t device_id, int argc, char **argv)
{
	struct lkmdbg_input_event *events;
	size_t event_count;
	size_t bytes;
	int channel_fd;
	ssize_t nwritten;

	if ((argc % 3) != 0 || argc <= 0) {
		fprintf(stderr, "inject requires type code value triples\n");
		return -1;
	}

	event_count = (size_t)argc / 3U;
	bytes = event_count * sizeof(*events);
	events = calloc(event_count, sizeof(*events));
	if (!events) {
		fprintf(stderr, "event allocation failed\n");
		return -1;
	}

	for (size_t i = 0; i < event_count; i++) {
		char *endp = NULL;

		events[i].type = (uint32_t)strtoul(argv[i * 3], &endp, 0);
		if (!endp || *endp != '\0') {
			fprintf(stderr, "invalid type: %s\n", argv[i * 3]);
			free(events);
			return -1;
		}
		events[i].code = (uint32_t)strtoul(argv[i * 3 + 1], &endp, 0);
		if (!endp || *endp != '\0') {
			fprintf(stderr, "invalid code: %s\n", argv[i * 3 + 1]);
			free(events);
			return -1;
		}
		events[i].value = (int32_t)strtol(argv[i * 3 + 2], &endp, 0);
		if (!endp || *endp != '\0') {
			fprintf(stderr, "invalid value: %s\n", argv[i * 3 + 2]);
			free(events);
			return -1;
		}
	}

	channel_fd = open_input_channel(session_fd, device_id, 0);
	if (channel_fd < 0) {
		free(events);
		return -1;
	}

	nwritten = write(channel_fd, events, bytes);
	if (nwritten < 0) {
		fprintf(stderr, "write failed: %s\n", strerror(errno));
		close(channel_fd);
		free(events);
		return -1;
	}
	if ((size_t)nwritten != bytes) {
		fprintf(stderr, "short write: %zd/%zu\n", nwritten, bytes);
		close(channel_fd);
		free(events);
		return -1;
	}

	close(channel_fd);
	free(events);
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage:\n"
		"  %s list\n"
		"  %s info <device_id>\n"
		"  %s read <device_id> [max_events] [timeout_ms] [include_injected]\n"
		"  %s inject <device_id> <type> <code> <value> [<type> <code> <value> ...]\n",
		argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	int session_fd;
	int ret = 1;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	session_fd = tool_open_session_fd();
	if (session_fd < 0)
		return 1;

	if (strcmp(argv[1], "list") == 0) {
		ret = list_devices(session_fd) == 0 ? 0 : 1;
	} else if (strcmp(argv[1], "info") == 0 && argc >= 3) {
		ret = info_device(session_fd, strtoull(argv[2], NULL, 0)) == 0 ? 0 : 1;
	} else if (strcmp(argv[1], "read") == 0 && argc >= 3) {
		unsigned int max_events = argc >= 4 ? strtoul(argv[3], NULL, 0) : 16U;
		int timeout_ms = argc >= 5 ? atoi(argv[4]) : 5000;
		uint32_t flags = 0;

		if (argc >= 6 && atoi(argv[5]) != 0)
			flags |= LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED;
		ret = read_events(session_fd, strtoull(argv[2], NULL, 0), max_events,
				  timeout_ms, flags) == 0 ?
			      0 :
			      1;
	} else if (strcmp(argv[1], "inject") == 0 && argc >= 6) {
		ret = inject_events(session_fd, strtoull(argv[2], NULL, 0), argc - 3,
				    &argv[3]) == 0 ?
			      0 :
			      1;
	} else {
		usage(argv[0]);
	}

	close(session_fd);
	return ret;
}
