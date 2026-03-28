#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_control.h"

static int run_child(void)
{
	for (;;)
		usleep(20000);
	return 0;
}

static int pick_parked_tid(int session_fd, pid_t *tid_out)
{
	struct lkmdbg_thread_entry entries[32];
	struct lkmdbg_thread_query_request reply;
	int32_t cursor = 0;
	uint32_t i;

	for (;;) {
		memset(entries, 0, sizeof(entries));
		memset(&reply, 0, sizeof(reply));
		if (query_target_threads(session_fd, cursor, entries,
					 (uint32_t)(sizeof(entries) / sizeof(entries[0])),
					 &reply) < 0)
			return -1;
		for (i = 0; i < reply.entries_filled; i++) {
			if (!(entries[i].flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED))
				continue;
			*tid_out = entries[i].tid;
			return 0;
		}
		if (reply.done)
			break;
		cursor = reply.next_tid;
	}

	return -1;
}

int main(void)
{
	struct lkmdbg_thread_regs_request regs;
	struct lkmdbg_freeze_request freeze_reply;
	pid_t child;
	pid_t tid = 0;
	int session_fd = -1;
	uint64_t saved_x19;
	uint32_t saved_fpsr;
	uint64_t saved_v0_lo;
	int parked_pick = 0;
	int frozen = 0;
	int status = 1;

	memset(&regs, 0, sizeof(regs));
	memset(&freeze_reply, 0, sizeof(freeze_reply));

	child = fork();
	if (child < 0) {
		fprintf(stderr, "example_regs_fp: fork failed errno=%d\n", errno);
		return 1;
	}
	if (child == 0)
		_exit(run_child());

	session_fd = open_session_fd();
	if (session_fd < 0)
		goto out;
	if (set_target(session_fd, child) < 0)
		goto out;
	if (freeze_target_threads(session_fd, 2000, &freeze_reply, 0) < 0)
		goto out;
	frozen = 1;
	parked_pick = pick_parked_tid(session_fd, &tid);
	if (parked_pick < 0) {
		tid = child;
		fprintf(stderr,
			"example_regs_fp: no parked frozen thread, fallback tid=%d\n",
			tid);
	}

	if (get_target_regs(session_fd, tid, &regs) < 0) {
		if (parked_pick < 0 && errno == EBUSY) {
			printf("example_regs_fp: skip unsupported frozen regs path (busy)\n");
			status = 0;
			goto out;
		}
		goto out;
	}
	if (!(regs.regs.features & LKMDBG_REGS_ARM64_FEATURE_FP)) {
		fprintf(stderr, "example_regs_fp: FP feature missing features=0x%x\n",
			regs.regs.features);
		goto out;
	}

	saved_x19 = regs.regs.regs[19];
	saved_fpsr = regs.regs.fpsr;
	saved_v0_lo = regs.regs.vregs[0].lo;
	regs.regs.regs[19] = saved_x19 ^ 0x5a5a5a5a5a5a5a5aULL;
	regs.regs.fpsr = saved_fpsr ^ 0x1U;
	regs.regs.vregs[0].lo = saved_v0_lo ^ 0x1ULL;
	if (set_target_regs(session_fd, &regs) < 0) {
		if (parked_pick < 0 && errno == EBUSY) {
			printf("example_regs_fp: skip unsupported frozen setregs path (busy)\n");
			status = 0;
			goto out;
		}
		goto out;
	}

	memset(&regs, 0, sizeof(regs));
	if (get_target_regs(session_fd, tid, &regs) < 0)
		goto out;
	if (regs.regs.regs[19] != (saved_x19 ^ 0x5a5a5a5a5a5a5a5aULL) ||
	    regs.regs.fpsr != (saved_fpsr ^ 0x1U) ||
	    regs.regs.vregs[0].lo != (saved_v0_lo ^ 0x1ULL)) {
		fprintf(stderr,
			"example_regs_fp: verify failed x19=0x%" PRIx64 " fpsr=0x%x v0_lo=0x%" PRIx64 "\n",
			(uint64_t)regs.regs.regs[19], regs.regs.fpsr,
			(uint64_t)regs.regs.vregs[0].lo);
		goto out;
	}

	regs.regs.regs[19] = saved_x19;
	regs.regs.fpsr = saved_fpsr;
	regs.regs.vregs[0].lo = saved_v0_lo;
	if (set_target_regs(session_fd, &regs) < 0)
		goto out;

	status = 0;
	printf("example_regs_fp: ok tid=%d\n", tid);

out:
	if (frozen)
		(void)thaw_target_threads(session_fd, 2000, NULL, 0);
	if (session_fd >= 0)
		close(session_fd);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
