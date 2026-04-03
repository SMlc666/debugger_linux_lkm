#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_control.h"

static int run_child(void)
{
	volatile uint64_t counter = 0;

	for (;;) {
		counter++;
		if ((counter & 0x3ffULL) == 0)
			(void)syscall(SYS_gettid);
	}
	return 0;
}

static int pick_parked_tid(int session_fd, unsigned int attempts,
			   useconds_t delay_us, pid_t *tid_out)
{
	struct lkmdbg_thread_entry entries[32];
	unsigned int attempt;

	for (attempt = 0; attempt < attempts; attempt++) {
		struct lkmdbg_thread_query_request reply;
		int32_t cursor = 0;
		uint32_t i;

		for (;;) {
			memset(entries, 0, sizeof(entries));
			memset(&reply, 0, sizeof(reply));
			if (query_target_threads(session_fd, cursor, entries,
						 (uint32_t)(sizeof(entries) /
							    sizeof(entries[0])),
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

		if (attempt + 1 < attempts)
			usleep(delay_us);
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
	uint64_t saved_x20;
	uint32_t saved_fpsr;
	uint32_t saved_fpcr;
	uint64_t saved_v0_lo;
	uint64_t saved_v0_hi;
	uint64_t saved_v1_lo;
	uint64_t saved_v1_hi;
	uint64_t expect_x19;
	uint64_t expect_x20;
	uint32_t expect_fpsr;
	uint32_t expect_fpcr;
	uint64_t expect_v0_lo;
	uint64_t expect_v0_hi;
	uint64_t expect_v1_lo;
	uint64_t expect_v1_hi;
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

	if (!freeze_reply.threads_parked) {
		fprintf(stderr,
			"example_regs_fp: freeze reported no parked threads total=%u settled=%u parked=%u\n",
			freeze_reply.threads_total, freeze_reply.threads_settled,
			freeze_reply.threads_parked);
		goto out;
	}

	if (pick_parked_tid(session_fd, 50, 20000, &tid) < 0) {
		fprintf(stderr,
			"example_regs_fp: no parked frozen thread after freeze total=%u settled=%u parked=%u\n",
			freeze_reply.threads_total, freeze_reply.threads_settled,
			freeze_reply.threads_parked);
		goto out;
	}

	if (get_target_regs(session_fd, tid, &regs) < 0)
		goto out;
	if (!(regs.regs.features & LKMDBG_REGS_ARM64_FEATURE_FP)) {
		fprintf(stderr, "example_regs_fp: FP feature missing features=0x%x\n",
			regs.regs.features);
		goto out;
	}

	saved_x19 = regs.regs.regs[19];
	saved_x20 = regs.regs.regs[20];
	saved_fpsr = regs.regs.fpsr;
	saved_fpcr = regs.regs.fpcr;
	saved_v0_lo = regs.regs.vregs[0].lo;
	saved_v0_hi = regs.regs.vregs[0].hi;
	saved_v1_lo = regs.regs.vregs[1].lo;
	saved_v1_hi = regs.regs.vregs[1].hi;

	expect_x19 = saved_x19 ^ 0x5a5a5a5a5a5a5a5aULL;
	expect_x20 = saved_x20 ^ 0xa5a5a5a5a5a5a5a5ULL;
	expect_fpsr = saved_fpsr ^ 0x1U;
	expect_fpcr = saved_fpcr ^ 0x2U;
	expect_v0_lo = saved_v0_lo ^ 0x1ULL;
	expect_v0_hi = saved_v0_hi ^ 0x10ULL;
	expect_v1_lo = saved_v1_lo ^ 0x100ULL;
	expect_v1_hi = saved_v1_hi ^ 0x1000ULL;

	regs.regs.regs[19] = expect_x19;
	regs.regs.regs[20] = expect_x20;
	regs.regs.fpsr = expect_fpsr;
	regs.regs.fpcr = expect_fpcr;
	regs.regs.vregs[0].lo = expect_v0_lo;
	regs.regs.vregs[0].hi = expect_v0_hi;
	regs.regs.vregs[1].lo = expect_v1_lo;
	regs.regs.vregs[1].hi = expect_v1_hi;
	if (set_target_regs(session_fd, &regs) < 0)
		goto out;

	memset(&regs, 0, sizeof(regs));
	if (get_target_regs(session_fd, tid, &regs) < 0)
		goto out;
	if (regs.regs.regs[19] != expect_x19 ||
	    regs.regs.regs[20] != expect_x20 ||
	    regs.regs.fpsr != expect_fpsr ||
	    regs.regs.fpcr != expect_fpcr ||
	    regs.regs.vregs[0].lo != expect_v0_lo ||
	    regs.regs.vregs[0].hi != expect_v0_hi ||
	    regs.regs.vregs[1].lo != expect_v1_lo ||
	    regs.regs.vregs[1].hi != expect_v1_hi) {
		fprintf(stderr,
			"example_regs_fp: verify failed x19=0x%" PRIx64
			" x20=0x%" PRIx64 " fpsr=0x%x fpcr=0x%x"
			" v0=(0x%" PRIx64 ",0x%" PRIx64 ")"
			" v1=(0x%" PRIx64 ",0x%" PRIx64 ")\n",
			(uint64_t)regs.regs.regs[19],
			(uint64_t)regs.regs.regs[20],
			regs.regs.fpsr, regs.regs.fpcr,
			(uint64_t)regs.regs.vregs[0].lo,
			(uint64_t)regs.regs.vregs[0].hi,
			(uint64_t)regs.regs.vregs[1].lo,
			(uint64_t)regs.regs.vregs[1].hi);
		goto out;
	}

	regs.regs.regs[19] = saved_x19;
	regs.regs.regs[20] = saved_x20;
	regs.regs.fpsr = saved_fpsr;
	regs.regs.fpcr = saved_fpcr;
	regs.regs.vregs[0].lo = saved_v0_lo;
	regs.regs.vregs[0].hi = saved_v0_hi;
	regs.regs.vregs[1].lo = saved_v1_lo;
	regs.regs.vregs[1].hi = saved_v1_hi;
	if (set_target_regs(session_fd, &regs) < 0)
		goto out;

	memset(&regs, 0, sizeof(regs));
	if (get_target_regs(session_fd, tid, &regs) < 0)
		goto out;
	if (regs.regs.regs[19] != saved_x19 ||
	    regs.regs.regs[20] != saved_x20 ||
	    regs.regs.fpsr != saved_fpsr ||
	    regs.regs.fpcr != saved_fpcr ||
	    regs.regs.vregs[0].lo != saved_v0_lo ||
	    regs.regs.vregs[0].hi != saved_v0_hi ||
	    regs.regs.vregs[1].lo != saved_v1_lo ||
	    regs.regs.vregs[1].hi != saved_v1_hi) {
		fprintf(stderr,
			"example_regs_fp: restore verify failed x19=0x%" PRIx64
			" x20=0x%" PRIx64 " fpsr=0x%x fpcr=0x%x"
			" v0=(0x%" PRIx64 ",0x%" PRIx64 ")"
			" v1=(0x%" PRIx64 ",0x%" PRIx64 ")\n",
			(uint64_t)regs.regs.regs[19],
			(uint64_t)regs.regs.regs[20],
			regs.regs.fpsr, regs.regs.fpcr,
			(uint64_t)regs.regs.vregs[0].lo,
			(uint64_t)regs.regs.vregs[0].hi,
			(uint64_t)regs.regs.vregs[1].lo,
			(uint64_t)regs.regs.vregs[1].hi);
		goto out;
	}

	status = 0;
	printf("example_regs_fp: ok tid=%d fields=x19,x20,fpsr,fpcr,v0,v1\n",
	       tid);

out:
	if (frozen)
		(void)thaw_target_threads(session_fd, 2000, NULL, 0);
	if (session_fd >= 0)
		close(session_fd);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return status;
}
