#ifndef _LKMDBG_IOCTL_H
#define _LKMDBG_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define LKMDBG_PROTO_VERSION 5
#define LKMDBG_IOC_MAGIC 0xBD
#define LKMDBG_EVENT_VERSION 3

#define LKMDBG_EVENT_SESSION_OPENED 1
#define LKMDBG_EVENT_SESSION_RESET 2
#define LKMDBG_EVENT_INTERNAL_NOTICE 3
#define LKMDBG_EVENT_HOOK_INSTALLED 16
#define LKMDBG_EVENT_HOOK_REMOVED 17
#define LKMDBG_EVENT_HOOK_HIT 18
#define LKMDBG_EVENT_TARGET_CLONE 32
#define LKMDBG_EVENT_TARGET_EXEC 33
#define LKMDBG_EVENT_TARGET_EXIT 34
#define LKMDBG_EVENT_TARGET_SIGNAL 35
#define LKMDBG_EVENT_TARGET_STOP 36

#define LKMDBG_THREAD_COMM_MAX 16

#define LKMDBG_THREAD_FLAG_GROUP_LEADER 0x00000001U
#define LKMDBG_THREAD_FLAG_SESSION_TARGET 0x00000002U
#define LKMDBG_THREAD_FLAG_FREEZE_TRACKED 0x00000004U
#define LKMDBG_THREAD_FLAG_FREEZE_SETTLED 0x00000008U
#define LKMDBG_THREAD_FLAG_FREEZE_PARKED 0x00000010U
#define LKMDBG_THREAD_FLAG_EXITING 0x00000020U

#define LKMDBG_TARGET_CLONE_THREAD 1U
#define LKMDBG_TARGET_CLONE_PROCESS 2U

#define LKMDBG_SIGNAL_EVENT_GROUP 0x00000001U

#define LKMDBG_STOP_REASON_FREEZE 1U
#define LKMDBG_STOP_REASON_BREAKPOINT 2U
#define LKMDBG_STOP_REASON_WATCHPOINT 3U
#define LKMDBG_STOP_REASON_SINGLE_STEP 4U

#define LKMDBG_STOP_FLAG_ACTIVE 0x00000001U
#define LKMDBG_STOP_FLAG_FROZEN 0x00000002U
#define LKMDBG_STOP_FLAG_ASYNC 0x00000004U
#define LKMDBG_STOP_FLAG_REARM_REQUIRED 0x00000008U
#define LKMDBG_STOP_FLAG_REGS_VALID 0x00000010U

#define LKMDBG_HWPOINT_TYPE_READ 0x00000001U
#define LKMDBG_HWPOINT_TYPE_WRITE 0x00000002U
#define LKMDBG_HWPOINT_TYPE_READWRITE \
	(LKMDBG_HWPOINT_TYPE_READ | LKMDBG_HWPOINT_TYPE_WRITE)
#define LKMDBG_HWPOINT_TYPE_EXEC 0x00000004U

#define LKMDBG_HWPOINT_FLAG_COUNTER_MODE 0x00000001U
#define LKMDBG_HWPOINT_FLAG_MMU 0x00000002U
#define LKMDBG_HWPOINT_FLAG_MMU_EXEC LKMDBG_HWPOINT_FLAG_MMU

/*
 * HWPOINT state bits are query-time observable state, not requested policy.
 *
 * ACTIVE:
 *   The backend is currently armed.
 *
 * LATCHED:
 *   A stop was delivered and the hwpoint is waiting for explicit rearm or
 *   removal. For MMU hwpoints this is the normal post-hit one-shot state.
 *
 * LOST:
 *   The guarded mapping disappeared and the hwpoint can no longer be rearmed.
 *   Typical cause: munmap/exit.
 *
 * MUTATED:
 *   The guarded mapping or effective PTEs no longer match the original armed
 *   baseline. Typical causes: mprotect, remap, or kernel access paths that
 *   disturb a READ-style MMU trap. Rearm may fail with ESTALE until user
 *   space removes and recreates the hwpoint.
 */
#define LKMDBG_HWPOINT_STATE_ACTIVE 0x00000001U
#define LKMDBG_HWPOINT_STATE_LATCHED 0x00000002U
#define LKMDBG_HWPOINT_STATE_LOST 0x00000004U
#define LKMDBG_HWPOINT_STATE_MUTATED 0x00000008U

#define LKMDBG_CONTINUE_FLAG_REARM_HWPOINTS 0x00000001U

#define LKMDBG_VMA_PROT_READ 0x00000001U
#define LKMDBG_VMA_PROT_WRITE 0x00000002U
#define LKMDBG_VMA_PROT_EXEC 0x00000004U
#define LKMDBG_VMA_PROT_MAYREAD 0x00000008U
#define LKMDBG_VMA_PROT_MAYWRITE 0x00000010U
#define LKMDBG_VMA_PROT_MAYEXEC 0x00000020U

#define LKMDBG_VMA_FLAG_ANON 0x00000001U
#define LKMDBG_VMA_FLAG_FILE 0x00000002U
#define LKMDBG_VMA_FLAG_SHARED 0x00000004U
#define LKMDBG_VMA_FLAG_STACK 0x00000008U
#define LKMDBG_VMA_FLAG_HEAP 0x00000010U
#define LKMDBG_VMA_FLAG_PFNMAP 0x00000020U
#define LKMDBG_VMA_FLAG_IO 0x00000040U

#define LKMDBG_PAGE_FLAG_MAPPED 0x00000001U
#define LKMDBG_PAGE_FLAG_PRESENT 0x00000002U
#define LKMDBG_PAGE_FLAG_NOFAULT_READ 0x00000004U
#define LKMDBG_PAGE_FLAG_NOFAULT_WRITE 0x00000008U
#define LKMDBG_PAGE_FLAG_FORCE_READ 0x00000010U
#define LKMDBG_PAGE_FLAG_FORCE_WRITE 0x00000020U
#define LKMDBG_PAGE_FLAG_PROT_READ 0x00000040U
#define LKMDBG_PAGE_FLAG_PROT_WRITE 0x00000080U
#define LKMDBG_PAGE_FLAG_PROT_EXEC 0x00000100U
#define LKMDBG_PAGE_FLAG_MAYREAD 0x00000200U
#define LKMDBG_PAGE_FLAG_MAYWRITE 0x00000400U
#define LKMDBG_PAGE_FLAG_MAYEXEC 0x00000800U
#define LKMDBG_PAGE_FLAG_ANON 0x00001000U
#define LKMDBG_PAGE_FLAG_FILE 0x00002000U
#define LKMDBG_PAGE_FLAG_SHARED 0x00004000U
#define LKMDBG_PAGE_FLAG_PFNMAP 0x00008000U
#define LKMDBG_PAGE_FLAG_IO 0x00010000U

struct lkmdbg_open_session_request {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 reserved;
};

struct lkmdbg_status_reply {
	__u32 version;
	__u32 size;
	__u32 hook_requested;
	__u32 hook_active;
	__s32 owner_tgid;
	__s32 target_tgid;
	__s32 target_tid;
	__u32 event_queue_depth;
	__u64 session_id;
	__u64 active_sessions;
	__u64 load_jiffies;
	__u64 status_reads;
	__u64 bootstrap_ioctl_calls;
	__u64 session_ioctl_calls;
	__u64 session_opened_total;
	__u64 open_successes;
	__u64 session_event_drops;
	__u64 total_event_drops;
	__u64 stop_cookie;
	__u32 stop_reason;
	__u32 stop_flags;
	__s32 stop_tgid;
	__s32 stop_tid;
};

struct lkmdbg_target_request {
	__u32 version;
	__u32 size;
	__s32 tgid;
	__s32 tid;
};

struct lkmdbg_mem_op {
	__u64 remote_addr;
	__u64 local_addr;
	__u32 length;
	__u32 flags;
	__u32 bytes_done;
	__u32 reserved0;
};

#define LKMDBG_MEM_OP_FLAG_FORCE_ACCESS 0x00000001U

struct lkmdbg_mem_request {
	__u32 version;
	__u32 size;
	__u64 ops_addr;
	__u32 op_count;
	__u32 flags;
	__u32 ops_done;
	__u32 reserved0;
	__u64 bytes_done;
};

struct lkmdbg_vma_entry {
	__u64 start_addr;
	__u64 end_addr;
	__u64 pgoff;
	__u64 inode;
	__u64 vm_flags_raw;
	__u32 prot;
	__u32 flags;
	__u32 dev_major;
	__u32 dev_minor;
	__u32 name_offset;
	__u32 name_size;
};

struct lkmdbg_vma_query_request {
	__u32 version;
	__u32 size;
	__u64 start_addr;
	__u64 entries_addr;
	__u32 max_entries;
	__u32 flags;
	__u64 names_addr;
	__u32 names_size;
	__u32 entries_filled;
	__u32 names_used;
	__u32 done;
	__u32 reserved0;
	__u64 next_addr;
};

struct lkmdbg_freeze_request {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 timeout_ms;
	__u32 threads_total;
	__u32 threads_settled;
	__u32 threads_parked;
	__u32 reserved0;
};

struct lkmdbg_thread_entry {
	__s32 tid;
	__s32 tgid;
	__u32 flags;
	__u32 reserved0;
	__u64 user_pc;
	__u64 user_sp;
	char comm[LKMDBG_THREAD_COMM_MAX];
};

struct lkmdbg_thread_query_request {
	__u32 version;
	__u32 size;
	__u64 entries_addr;
	__u32 max_entries;
	__u32 flags;
	__s32 start_tid;
	__u32 entries_filled;
	__u32 done;
	__s32 next_tid;
	__u32 reserved0;
};

struct lkmdbg_regs_arm64 {
	__u64 regs[31];
	__u64 sp;
	__u64 pc;
	__u64 pstate;
};

struct lkmdbg_thread_regs_request {
	__u32 version;
	__u32 size;
	__s32 tid;
	__u32 flags;
	struct lkmdbg_regs_arm64 regs;
};

struct lkmdbg_stop_state {
	__u64 cookie;
	__u32 reason;
	__u32 flags;
	__s32 tgid;
	__s32 tid;
	__u32 event_flags;
	__u32 reserved0;
	__u64 value0;
	__u64 value1;
	struct lkmdbg_regs_arm64 regs;
};

struct lkmdbg_stop_query_request {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 reserved0;
	struct lkmdbg_stop_state stop;
};

struct lkmdbg_continue_request {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 timeout_ms;
	__u64 stop_cookie;
	__u32 threads_total;
	__u32 threads_settled;
	__u32 threads_parked;
	__u32 reserved0;
};

struct lkmdbg_hwpoint_request {
	__u32 version;
	__u32 size;
	__u64 id;
	__u64 addr;
	__s32 tid;
	__u32 type;
	__u32 len;
	__u32 flags;
};

struct lkmdbg_hwpoint_entry {
	__u64 id;
	__u64 addr;
	__u64 hits;
	__s32 tgid;
	__s32 tid;
	__u32 type;
	__u32 len;
	__u32 flags;
	__u32 state;
};

struct lkmdbg_hwpoint_query_request {
	__u32 version;
	__u32 size;
	__u64 entries_addr;
	__u32 max_entries;
	__u32 flags;
	__u64 start_id;
	__u32 entries_filled;
	__u32 done;
	__u64 next_id;
};

struct lkmdbg_single_step_request {
	__u32 version;
	__u32 size;
	__s32 tid;
	__u32 flags;
};

struct lkmdbg_page_entry {
	__u64 page_addr;
	__u64 pgoff;
	__u64 inode;
	__u64 vm_flags_raw;
	__u32 flags;
	__u32 dev_major;
	__u32 dev_minor;
	__u32 reserved0;
};

struct lkmdbg_page_query_request {
	__u32 version;
	__u32 size;
	__u64 start_addr;
	__u64 length;
	__u64 entries_addr;
	__u32 max_entries;
	__u32 flags;
	__u32 entries_filled;
	__u32 done;
	__u64 next_addr;
};

struct lkmdbg_event_record {
	__u32 version;
	__u32 type;
	__u32 size;
	__u32 code;
	__u64 session_id;
	__u64 seq;
	__s32 tgid;
	__s32 tid;
	__u32 flags;
	__u32 reserved0;
	__u64 value0;
	__u64 value1;
};

#define LKMDBG_IOC_OPEN_SESSION \
	_IOW(LKMDBG_IOC_MAGIC, 0x01, struct lkmdbg_open_session_request)
#define LKMDBG_IOC_GET_STATUS \
	_IOR(LKMDBG_IOC_MAGIC, 0x02, struct lkmdbg_status_reply)
#define LKMDBG_IOC_RESET_SESSION _IO(LKMDBG_IOC_MAGIC, 0x03)
#define LKMDBG_IOC_SET_TARGET \
	_IOW(LKMDBG_IOC_MAGIC, 0x10, struct lkmdbg_target_request)
#define LKMDBG_IOC_READ_MEM \
	_IOWR(LKMDBG_IOC_MAGIC, 0x11, struct lkmdbg_mem_request)
#define LKMDBG_IOC_WRITE_MEM \
	_IOWR(LKMDBG_IOC_MAGIC, 0x12, struct lkmdbg_mem_request)
#define LKMDBG_IOC_QUERY_VMAS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x13, struct lkmdbg_vma_query_request)
#define LKMDBG_IOC_FREEZE_THREADS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x14, struct lkmdbg_freeze_request)
#define LKMDBG_IOC_THAW_THREADS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x15, struct lkmdbg_freeze_request)
#define LKMDBG_IOC_QUERY_THREADS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x16, struct lkmdbg_thread_query_request)
#define LKMDBG_IOC_GET_REGS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x17, struct lkmdbg_thread_regs_request)
#define LKMDBG_IOC_SET_REGS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x18, struct lkmdbg_thread_regs_request)
#define LKMDBG_IOC_ADD_HWPOINT \
	_IOWR(LKMDBG_IOC_MAGIC, 0x19, struct lkmdbg_hwpoint_request)
#define LKMDBG_IOC_REMOVE_HWPOINT \
	_IOWR(LKMDBG_IOC_MAGIC, 0x1A, struct lkmdbg_hwpoint_request)
#define LKMDBG_IOC_QUERY_HWPOINTS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x1B, struct lkmdbg_hwpoint_query_request)
#define LKMDBG_IOC_SINGLE_STEP \
	_IOWR(LKMDBG_IOC_MAGIC, 0x1C, struct lkmdbg_single_step_request)
#define LKMDBG_IOC_REARM_HWPOINT \
	_IOWR(LKMDBG_IOC_MAGIC, 0x1D, struct lkmdbg_hwpoint_request)
#define LKMDBG_IOC_GET_STOP_STATE \
	_IOWR(LKMDBG_IOC_MAGIC, 0x1E, struct lkmdbg_stop_query_request)
#define LKMDBG_IOC_CONTINUE_TARGET \
	_IOWR(LKMDBG_IOC_MAGIC, 0x1F, struct lkmdbg_continue_request)
#define LKMDBG_IOC_QUERY_PAGES \
	_IOWR(LKMDBG_IOC_MAGIC, 0x20, struct lkmdbg_page_query_request)

#endif
