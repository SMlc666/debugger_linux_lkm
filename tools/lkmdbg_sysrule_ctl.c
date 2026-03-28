#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "driver/bridge_c.h"
#include "driver/bridge_control.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s cfg-get <pid>\n"
		"  %s cfg <pid> <observe|enforce> <raw|raw+rule|rule>\n"
		"  %s add <pid> <syscall_nr|-1> <setret|stop|setret+stop> <retval> [tid] [priority] [oneshot|persistent] [enter|exit|both]\n"
		"  %s del <pid> <rule_id>\n"
		"  %s list <pid>\n",
		prog, prog, prog, prog, prog);
}

static int parse_pid(const char *s, pid_t *pid_out)
{
	char *endp = NULL;
	long v = strtol(s, &endp, 0);

	if (!endp || *endp != '\0' || v <= 0)
		return -1;
	*pid_out = (pid_t)v;
	return 0;
}

static int parse_i32(const char *s, int32_t *out)
{
	char *endp = NULL;
	long v = strtol(s, &endp, 0);

	if (!endp || *endp != '\0' || v < INT32_MIN || v > INT32_MAX)
		return -1;
	*out = (int32_t)v;
	return 0;
}

static int parse_u32(const char *s, uint32_t *out)
{
	char *endp = NULL;
	unsigned long v = strtoul(s, &endp, 0);

	if (!endp || *endp != '\0' || v > UINT32_MAX)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static int parse_u64(const char *s, uint64_t *out)
{
	char *endp = NULL;
	unsigned long long v = strtoull(s, &endp, 0);

	if (!endp || *endp != '\0')
		return -1;
	*out = (uint64_t)v;
	return 0;
}

static int parse_s64(const char *s, int64_t *out)
{
	char *endp = NULL;
	long long v = strtoll(s, &endp, 0);

	if (!endp || *endp != '\0')
		return -1;
	*out = (int64_t)v;
	return 0;
}

static int parse_mode(const char *s, uint32_t *mode_out)
{
	if (strcmp(s, "observe") == 0) {
		*mode_out = LKMDBG_SYSCALL_RULE_MODE_OBSERVE;
		return 0;
	}
	if (strcmp(s, "enforce") == 0) {
		*mode_out = LKMDBG_SYSCALL_RULE_MODE_ENFORCE;
		return 0;
	}
	return -1;
}

static int parse_event_policy(const char *s, uint32_t *policy_out)
{
	if (strcmp(s, "raw") == 0) {
		*policy_out = LKMDBG_SYSCALL_RULE_EVENT_RAW_ONLY;
		return 0;
	}
	if (strcmp(s, "raw+rule") == 0) {
		*policy_out = LKMDBG_SYSCALL_RULE_EVENT_RAW_AND_RULE;
		return 0;
	}
	if (strcmp(s, "rule") == 0) {
		*policy_out = LKMDBG_SYSCALL_RULE_EVENT_RULE_ONLY;
		return 0;
	}
	return -1;
}

static int parse_actions(const char *s, uint32_t *actions_out)
{
	if (strcmp(s, "setret") == 0) {
		*actions_out = LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN;
		return 0;
	}
	if (strcmp(s, "stop") == 0) {
		*actions_out = LKMDBG_SYSCALL_RULE_ACTION_STOP;
		return 0;
	}
	if (strcmp(s, "setret+stop") == 0 || strcmp(s, "stop+setret") == 0) {
		*actions_out = LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN |
			       LKMDBG_SYSCALL_RULE_ACTION_STOP;
		return 0;
	}
	return -1;
}

static int parse_phases(const char *s, uint32_t *phases_out)
{
	if (strcmp(s, "enter") == 0) {
		*phases_out = LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
		return 0;
	}
	if (strcmp(s, "exit") == 0) {
		*phases_out = LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
		return 0;
	}
	if (strcmp(s, "both") == 0) {
		*phases_out = LKMDBG_SYSCALL_TRACE_PHASE_ENTER |
			      LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
		return 0;
	}
	return -1;
}

static const char *mode_name(uint32_t mode)
{
	if (mode == LKMDBG_SYSCALL_RULE_MODE_OBSERVE)
		return "observe";
	if (mode == LKMDBG_SYSCALL_RULE_MODE_ENFORCE)
		return "enforce";
	return "unknown";
}

static const char *policy_name(uint32_t policy)
{
	if (policy == LKMDBG_SYSCALL_RULE_EVENT_RAW_ONLY)
		return "raw";
	if (policy == LKMDBG_SYSCALL_RULE_EVENT_RAW_AND_RULE)
		return "raw+rule";
	if (policy == LKMDBG_SYSCALL_RULE_EVENT_RULE_ONLY)
		return "rule";
	return "unknown";
}

static void print_rule(const struct lkmdbg_syscall_rule_entry *e)
{
	printf("rule_id=%llu tid=%d nr=%d phases=0x%x actions=0x%x flags=0x%x priority=%u arg_match=0x%x rewrite=0x%x retval=%lld hits=%llu\n",
	       (unsigned long long)e->rule_id, e->tid, e->syscall_nr,
	       e->phases, e->actions, e->flags, e->priority,
	       e->arg_match_mask, e->rewrite_mask,
	       (long long)e->retval, (unsigned long long)e->hits);
}

int main(int argc, char **argv)
{
	pid_t pid;
	int session_fd = -1;
	int ret = 1;

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}
	if (parse_pid(argv[2], &pid) < 0) {
		fprintf(stderr, "invalid pid: %s\n", argv[2]);
		return 1;
	}

	session_fd = open_session_fd();
	if (session_fd < 0)
		return 1;
	if (set_target(session_fd, pid) < 0)
		goto out;

	if (strcmp(argv[1], "cfg-get") == 0) {
		struct lkmdbg_syscall_rule_config_request cfg;

		if (get_syscall_rule_config(session_fd, &cfg) < 0)
			goto out;
		printf("mode=%u(%s) event_policy=%u(%s) supported_mode_mask=0x%x supported_event_policy_mask=0x%x\n",
		       cfg.mode, mode_name(cfg.mode), cfg.event_policy,
		       policy_name(cfg.event_policy), cfg.supported_mode_mask,
		       cfg.supported_event_policy_mask);
		ret = 0;
		goto out;
	}

	if (strcmp(argv[1], "cfg") == 0) {
		uint32_t mode = 0;
		uint32_t policy = 0;
		struct lkmdbg_syscall_rule_config_request cfg;

		if (argc < 5 || parse_mode(argv[3], &mode) < 0 ||
		    parse_event_policy(argv[4], &policy) < 0) {
			usage(argv[0]);
			goto out;
		}
		if (set_syscall_rule_config(session_fd, mode, policy, &cfg) < 0)
			goto out;
		printf("mode=%u(%s) event_policy=%u(%s)\n", cfg.mode,
		       mode_name(cfg.mode), cfg.event_policy,
		       policy_name(cfg.event_policy));
		ret = 0;
		goto out;
	}

	if (strcmp(argv[1], "add") == 0) {
		struct lkmdbg_syscall_rule_entry entry;
		struct lkmdbg_syscall_rule_request reply;
		int32_t syscall_nr = -1;
		int32_t tid = 0;
		uint32_t actions = 0;
		uint32_t priority = 0;
		uint32_t phases = LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
		int64_t retval64 = 0;
		uint32_t flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
		int phase_consumed = 0;

		memset(&entry, 0, sizeof(entry));
		if (argc < 6 || parse_i32(argv[3], &syscall_nr) < 0 ||
		    parse_actions(argv[4], &actions) < 0 ||
		    parse_s64(argv[5], &retval64) < 0) {
			usage(argv[0]);
			goto out;
		}
		if (argc >= 7 && parse_i32(argv[6], &tid) < 0) {
			fprintf(stderr, "invalid tid: %s\n", argv[6]);
			goto out;
		}
		if (argc >= 8 && parse_u32(argv[7], &priority) < 0) {
			fprintf(stderr, "invalid priority: %s\n", argv[7]);
			goto out;
		}
		if (argc >= 9) {
			if (strcmp(argv[8], "oneshot") == 0)
				flags |= LKMDBG_SYSCALL_RULE_FLAG_ONESHOT;
			else if (parse_phases(argv[8], &phases) == 0)
				phase_consumed = 1;
			else if (strcmp(argv[8], "persistent") != 0) {
				fprintf(stderr, "invalid rule persistence: %s\n",
					argv[8]);
				goto out;
			}
		}
		if (argc >= 10 && !phase_consumed &&
		    parse_phases(argv[9], &phases) < 0) {
			fprintf(stderr, "invalid phases: %s\n", argv[9]);
			goto out;
		}

		entry.tid = tid;
		entry.syscall_nr = syscall_nr;
		entry.phases = phases;
		entry.actions = actions;
		entry.flags = flags;
		entry.priority = priority;
		entry.retval = retval64;

		if (upsert_syscall_rule(session_fd, &entry, &reply) < 0)
			goto out;
		print_rule(&reply.rule);
		ret = 0;
		goto out;
	}

	if (strcmp(argv[1], "del") == 0) {
		uint64_t rule_id = 0;

		if (argc < 4 || parse_u64(argv[3], &rule_id) < 0) {
			usage(argv[0]);
			goto out;
		}
		if (remove_syscall_rule(session_fd, rule_id) < 0)
			goto out;
		printf("removed rule_id=%llu\n", (unsigned long long)rule_id);
		ret = 0;
		goto out;
	}

	if (strcmp(argv[1], "list") == 0) {
		struct lkmdbg_syscall_rule_entry entries[32];
		struct lkmdbg_syscall_rule_query_request reply;
		uint64_t cursor = 0;
		uint32_t i;

		for (;;) {
			memset(entries, 0, sizeof(entries));
			memset(&reply, 0, sizeof(reply));
			if (query_syscall_rules(session_fd, cursor, entries,
						(uint32_t)(sizeof(entries) /
							   sizeof(entries[0])),
						&reply) < 0) {
				goto out;
			}
			for (i = 0; i < reply.entries_filled; i++)
				print_rule(&entries[i]);
			if (reply.done)
				break;
			cursor = reply.next_id;
		}
		ret = 0;
		goto out;
	}

	usage(argv[0]);

out:
	if (session_fd >= 0)
		close(session_fd);
	return ret;
}
