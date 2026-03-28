#ifndef _LKMDBG_IOCTL_H
#define _LKMDBG_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define LKMDBG_PROTO_VERSION 26
#define LKMDBG_IOC_MAGIC 0xBD
#define LKMDBG_EVENT_VERSION 3
#define LKMDBG_EVENT_MASK_WORDS 2U

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
#define LKMDBG_EVENT_TARGET_SYSCALL 37
#define LKMDBG_EVENT_TARGET_MMAP 38
#define LKMDBG_EVENT_TARGET_MUNMAP 39
#define LKMDBG_EVENT_TARGET_MPROTECT 40

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
#define LKMDBG_SIGNAL_CONFIG_STOP 0x00000001U

#define LKMDBG_SYSCALL_TRACE_MODE_OFF 0U
#define LKMDBG_SYSCALL_TRACE_MODE_EVENT 0x00000001U
#define LKMDBG_SYSCALL_TRACE_MODE_STOP 0x00000002U
#define LKMDBG_SYSCALL_TRACE_MODE_CONTROL 0x00000004U

#define LKMDBG_SYSCALL_TRACE_PHASE_ENTER 0x00000001U
#define LKMDBG_SYSCALL_TRACE_PHASE_EXIT 0x00000002U

#define LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_TRACEPOINT 0x00000001U
#define LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_ENTRY_HOOK 0x00000002U
#define LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_CONTROL 0x00000004U

#define LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE 0x00000001U
#define LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN 0x00000002U
#define LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN 0x00000004U
#define LKMDBG_STEALTH_FLAG_OWNER_PROC_HIDDEN 0x00000008U

#define LKMDBG_INPUT_DEVICE_TEXT_MAX 80U
#define LKMDBG_INPUT_EV_WORDS 1U
#define LKMDBG_INPUT_KEY_WORDS 12U
#define LKMDBG_INPUT_REL_WORDS 1U
#define LKMDBG_INPUT_ABS_WORDS 1U
#define LKMDBG_INPUT_PROP_WORDS 1U
#define LKMDBG_INPUT_ABS_COUNT 64U

#define LKMDBG_INPUT_DEVICE_FLAG_HAS_KEYS 0x00000001U
#define LKMDBG_INPUT_DEVICE_FLAG_HAS_REL 0x00000002U
#define LKMDBG_INPUT_DEVICE_FLAG_HAS_ABS 0x00000004U
#define LKMDBG_INPUT_DEVICE_FLAG_HAS_MT 0x00000008U
#define LKMDBG_INPUT_DEVICE_FLAG_CAN_INJECT 0x00000010U

#define LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED 0x00000001U

#define LKMDBG_INPUT_EVENT_FLAG_INJECTED 0x00000001U

#define LKMDBG_STOP_REASON_FREEZE 1U
#define LKMDBG_STOP_REASON_BREAKPOINT 2U
#define LKMDBG_STOP_REASON_WATCHPOINT 3U
#define LKMDBG_STOP_REASON_SINGLE_STEP 4U
#define LKMDBG_STOP_REASON_SIGNAL 5U
#define LKMDBG_STOP_REASON_SYSCALL 6U
#define LKMDBG_STOP_REASON_REMOTE_CALL 7U

#define LKMDBG_STOP_FLAG_ACTIVE 0x00000001U
#define LKMDBG_STOP_FLAG_FROZEN 0x00000002U
#define LKMDBG_STOP_FLAG_ASYNC 0x00000004U
#define LKMDBG_STOP_FLAG_REARM_REQUIRED 0x00000008U
#define LKMDBG_STOP_FLAG_REGS_VALID 0x00000010U
#define LKMDBG_STOP_FLAG_SYSCALL_CONTROL 0x00000020U

#define LKMDBG_HWPOINT_TYPE_READ 0x00000001U
#define LKMDBG_HWPOINT_TYPE_WRITE 0x00000002U
#define LKMDBG_HWPOINT_TYPE_READWRITE \
	(LKMDBG_HWPOINT_TYPE_READ | LKMDBG_HWPOINT_TYPE_WRITE)
#define LKMDBG_HWPOINT_TYPE_EXEC 0x00000004U

#define LKMDBG_HWPOINT_FLAG_COUNTER_MODE 0x00000001U
#define LKMDBG_HWPOINT_FLAG_MMU 0x00000002U
#define LKMDBG_HWPOINT_FLAG_MMU_EXEC LKMDBG_HWPOINT_FLAG_MMU

#define LKMDBG_HWPOINT_ACTION_ONESHOT 0x00000001U
#define LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE 0x00000002U

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

#define LKMDBG_REMOTE_CALL_MAX_ARGS 8U

#define LKMDBG_REMOTE_CALL_FLAG_SET_SP 0x00000001U
#define LKMDBG_REMOTE_CALL_FLAG_SET_RETURN_PC 0x00000002U
#define LKMDBG_REMOTE_CALL_FLAG_SET_X8 0x00000004U

#define LKMDBG_SYSCALL_RESOLVE_ACTION_ALLOW 0U
#define LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE 1U
#define LKMDBG_SYSCALL_RESOLVE_ACTION_SKIP 2U
#define LKMDBG_SYSCALL_RESOLVE_FLAG_NR_REWRITE_SUPPORTED 0x00000001U

#define LKMDBG_REMOTE_THREAD_CREATE_FLAG_SET_TLS 0x00000001U

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

#define LKMDBG_VMA_QUERY_FLAG_FULL_PATH 0x00000001U

#define LKMDBG_IMAGE_FLAG_FILE 0x00000001U
#define LKMDBG_IMAGE_FLAG_MAIN_EXE 0x00000002U
#define LKMDBG_IMAGE_FLAG_SHARED 0x00000004U
#define LKMDBG_IMAGE_FLAG_DELETED 0x00000008U

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
#define LKMDBG_PAGE_FLAG_PT_PRESENT 0x00020000U
#define LKMDBG_PAGE_FLAG_PT_HUGE 0x00040000U

#define LKMDBG_PAGE_QUERY_FLAG_LEAF_STEP 0x00000001U

#define LKMDBG_PAGE_PT_FLAG_VALID 0x00000001U
#define LKMDBG_PAGE_PT_FLAG_USER 0x00000002U
#define LKMDBG_PAGE_PT_FLAG_WRITE 0x00000004U
#define LKMDBG_PAGE_PT_FLAG_DIRTY 0x00000008U
#define LKMDBG_PAGE_PT_FLAG_YOUNG 0x00000010U
#define LKMDBG_PAGE_PT_FLAG_EXEC 0x00000020U
#define LKMDBG_PAGE_PT_FLAG_PROTNONE 0x00000040U

#define LKMDBG_PAGE_LEVEL_NONE 0U
#define LKMDBG_PAGE_LEVEL_PTE 1U
#define LKMDBG_PAGE_LEVEL_PMD 2U
#define LKMDBG_PAGE_LEVEL_PUD 3U

#define LKMDBG_PTE_PATCH_FLAG_RAW 0x00000001U

#define LKMDBG_PTE_PATCH_STATE_ACTIVE 0x00000001U
#define LKMDBG_PTE_PATCH_STATE_LOST 0x00000002U
#define LKMDBG_PTE_PATCH_STATE_MUTATED 0x00000004U

#define LKMDBG_PTE_MODE_RAW 0U
#define LKMDBG_PTE_MODE_RO 1U
#define LKMDBG_PTE_MODE_RW 2U
#define LKMDBG_PTE_MODE_RX 3U
#define LKMDBG_PTE_MODE_RWX 4U
#define LKMDBG_PTE_MODE_PROTNONE 5U
#define LKMDBG_PTE_MODE_EXECONLY 6U

#define LKMDBG_REMOTE_MAP_PROT_READ 0x00000001U
#define LKMDBG_REMOTE_MAP_PROT_WRITE 0x00000002U
#define LKMDBG_REMOTE_MAP_PROT_EXEC 0x00000004U

#define LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET 0x00000001U
#define LKMDBG_REMOTE_MAP_FLAG_FIXED_TARGET 0x00000002U
/*
 * Reuse an existing caller-owned shared file-backed VMA as the local view.
 * The caller must keep that VMA alive until the session closes so the module
 * can restore the original PTEs during session teardown.
 */
#define LKMDBG_REMOTE_MAP_FLAG_STEALTH_LOCAL 0x00000004U
/*
 * Reuse an existing target-side VMA as the remote view so no new target VMA is
 * created. The caller must keep the local source pages alive until the mapping
 * is removed or the session closes.
 */
#define LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET 0x00000008U

#define LKMDBG_REMOTE_ALLOC_PROT_READ 0x00000001U
#define LKMDBG_REMOTE_ALLOC_PROT_WRITE 0x00000002U
#define LKMDBG_REMOTE_ALLOC_PROT_EXEC 0x00000004U

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
	__u32 stealth_flags;
	__u32 stealth_supported_flags;
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

struct lkmdbg_phys_op {
	__u64 phys_addr;
	__u64 local_addr;
	__u32 length;
	__u32 flags;
	__u32 bytes_done;
	__u32 reserved0;
	__u64 resolved_phys_addr;
	__u32 page_shift;
	__u32 pt_level;
	__u32 pt_flags;
	__u32 phys_span_length;
};

struct lkmdbg_phys_request {
	__u32 version;
	__u32 size;
	__u64 ops_addr;
	__u32 op_count;
	__u32 flags;
	__u32 ops_done;
	__u32 reserved0;
	__u64 bytes_done;
};

#define LKMDBG_PHYS_OP_FLAG_TARGET_VADDR 0x00000001U
#define LKMDBG_PHYS_OP_FLAG_TRANSLATE_ONLY 0x00000002U

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
	__u32 match_flags_mask;
	__u32 match_flags_value;
	__u32 match_prot_mask;
	__u32 match_prot_value;
	__u64 names_addr;
	__u32 names_size;
	__u32 entries_filled;
	__u32 names_used;
	__u32 done;
	__u32 reserved0;
	__u64 next_addr;
	__u64 generation;
};

struct lkmdbg_image_entry {
	__u64 start_addr;
	__u64 end_addr;
	__u64 base_addr;
	__u64 pgoff;
	__u64 inode;
	__u32 prot;
	__u32 flags;
	__u32 dev_major;
	__u32 dev_minor;
	__u32 segment_count;
	__u32 name_offset;
	__u32 name_size;
	__u32 reserved0;
};

struct lkmdbg_image_query_request {
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
	__u32 features;
	__u32 reserved0;
	__u32 fpsr;
	__u32 fpcr;
	struct {
		__u64 lo;
		__u64 hi;
	} vregs[32];
};

#define LKMDBG_REGS_ARM64_FEATURE_FP 0x00000001U

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
	__u64 trigger_hit_count;
	__u32 action_flags;
	__u32 reserved0;
};

struct lkmdbg_hwpoint_entry {
	__u64 id;
	__u64 addr;
	__u64 hits;
	__u64 trigger_hit_count;
	__s32 tgid;
	__s32 tid;
	__u32 type;
	__u32 len;
	__u32 flags;
	__u32 state;
	__u32 action_flags;
	__u32 reserved0;
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

struct lkmdbg_remote_call_request {
	__u32 version;
	__u32 size;
	__s32 tid;
	__u32 flags;
	__u64 target_pc;
	__u32 arg_count;
	__u32 reserved0;
	__u64 args[LKMDBG_REMOTE_CALL_MAX_ARGS];
	__u64 call_id;
	__u64 stack_ptr;
	__u64 return_pc;
	__u64 x8;
};

struct lkmdbg_syscall_resolve_request {
	__u32 version;
	__u32 size;
	__u64 stop_cookie;
	__u32 action;
	__u32 flags;
	__s32 syscall_nr;
	__u32 reserved0;
	__u64 args[6];
	__s64 retval;
	__u32 backend_flags;
	__u32 reserved1;
};

struct lkmdbg_remote_thread_create_request {
	__u32 version;
	__u32 size;
	__s32 tid;
	__u32 flags;
	__u32 timeout_ms;
	__u32 reserved0;
	__u64 launcher_pc;
	__u64 start_pc;
	__u64 start_arg;
	__u64 stack_top;
	__u64 tls;
	__u64 call_id;
	__u64 return_pc;
	__u64 stop_cookie;
	__s64 result;
	__s32 created_tid;
	__u32 reserved1;
};

struct lkmdbg_page_entry {
	__u64 page_addr;
	__u64 pgoff;
	__u64 inode;
	__u64 vm_flags_raw;
	__u64 pt_entry_raw;
	__u64 phys_addr;
	__u32 flags;
	__u32 dev_major;
	__u32 dev_minor;
	__u32 page_shift;
	__u32 pt_level;
	__u32 pt_flags;
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

struct lkmdbg_pte_patch_request {
	__u32 version;
	__u32 size;
	__u64 id;
	__u64 addr;
	__u64 raw_pte;
	__u64 baseline_pte;
	__u64 expected_pte;
	__u64 current_pte;
	__u64 baseline_vm_flags;
	__u64 current_vm_flags;
	__u32 mode;
	__u32 flags;
	__u32 state;
	__u32 page_shift;
};

struct lkmdbg_pte_patch_entry {
	__u64 id;
	__u64 page_addr;
	__u64 raw_pte;
	__u64 baseline_pte;
	__u64 expected_pte;
	__u64 current_pte;
	__u64 baseline_vm_flags;
	__u64 current_vm_flags;
	__u32 mode;
	__u32 flags;
	__u32 state;
	__u32 page_shift;
};

struct lkmdbg_pte_patch_query_request {
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

struct lkmdbg_remote_map_request {
	__u32 version;
	__u32 size;
	__u64 remote_addr;
	__u64 local_addr;
	__u64 length;
	__u32 prot;
	__u32 flags;
	__u32 timeout_ms;
	__u32 reserved0;
	__u64 map_id;
	__u64 mapped_length;
	__s32 map_fd;
	__u32 reserved1;
};

struct lkmdbg_remote_map_handle_request {
	__u32 version;
	__u32 size;
	__u64 map_id;
	__u64 remote_addr;
	__u64 local_addr;
	__u64 mapped_length;
	__u32 prot;
	__u32 flags;
};

struct lkmdbg_remote_map_entry {
	__u64 map_id;
	__u64 remote_addr;
	__u64 local_addr;
	__u64 mapped_length;
	__u32 prot;
	__u32 flags;
};

struct lkmdbg_remote_map_query_request {
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

struct lkmdbg_remote_alloc_request {
	__u32 version;
	__u32 size;
	__u64 remote_addr;
	__u64 length;
	__u32 prot;
	__u32 flags;
	__u64 alloc_id;
	__u64 mapped_length;
};

struct lkmdbg_remote_alloc_handle_request {
	__u32 version;
	__u32 size;
	__u64 alloc_id;
	__s32 target_tgid;
	__u32 reserved0;
	__u64 remote_addr;
	__u64 mapped_length;
	__u32 prot;
	__u32 flags;
};

struct lkmdbg_remote_alloc_entry {
	__u64 alloc_id;
	__s32 target_tgid;
	__u32 reserved0;
	__u64 remote_addr;
	__u64 mapped_length;
	__u32 prot;
	__u32 flags;
};

struct lkmdbg_remote_alloc_query_request {
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

struct lkmdbg_signal_config_request {
	__u32 version;
	__u32 size;
	__u64 mask_words[2];
	__u32 flags;
	__u32 reserved0;
};

struct lkmdbg_event_config_request {
	__u32 version;
	__u32 size;
	__u64 mask_words[LKMDBG_EVENT_MASK_WORDS];
	__u32 flags;
	__u32 reserved0;
	__u64 supported_mask_words[LKMDBG_EVENT_MASK_WORDS];
};

struct lkmdbg_syscall_trace_request {
	__u32 version;
	__u32 size;
	__s32 tid;
	__s32 syscall_nr;
	__u32 mode;
	__u32 phases;
	__u32 flags;
	__u32 supported_phases;
};

struct lkmdbg_stealth_request {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 supported_flags;
};

struct lkmdbg_input_device_entry {
	__u64 device_id;
	__u32 bustype;
	__u16 vendor;
	__u16 product;
	__u16 version_id;
	__u16 reserved0;
	__u32 flags;
	char name[LKMDBG_INPUT_DEVICE_TEXT_MAX];
	char phys[LKMDBG_INPUT_DEVICE_TEXT_MAX];
	char uniq[LKMDBG_INPUT_DEVICE_TEXT_MAX];
};

struct lkmdbg_input_query_request {
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

struct lkmdbg_input_absinfo {
	__s32 value;
	__s32 minimum;
	__s32 maximum;
	__s32 fuzz;
	__s32 flat;
	__s32 resolution;
};

struct lkmdbg_input_device_info_request {
	__u32 version;
	__u32 size;
	__u64 device_id;
	__u32 flags;
	__u32 supported_channel_flags;
	struct lkmdbg_input_device_entry entry;
	__u64 ev_bits[LKMDBG_INPUT_EV_WORDS];
	__u64 key_bits[LKMDBG_INPUT_KEY_WORDS];
	__u64 rel_bits[LKMDBG_INPUT_REL_WORDS];
	__u64 abs_bits[LKMDBG_INPUT_ABS_WORDS];
	__u64 prop_bits[LKMDBG_INPUT_PROP_WORDS];
	struct lkmdbg_input_absinfo absinfo[LKMDBG_INPUT_ABS_COUNT];
};

struct lkmdbg_input_channel_request {
	__u32 version;
	__u32 size;
	__u64 device_id;
	__u32 flags;
	__s32 channel_fd;
	__u64 channel_id;
	__u32 device_flags;
	__u32 supported_flags;
};

struct lkmdbg_input_event {
	__u64 seq;
	__u64 timestamp_ns;
	__u32 type;
	__u32 code;
	__s32 value;
	__u32 flags;
	__u32 reserved0;
	__u32 reserved1;
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
#define LKMDBG_IOC_QUERY_IMAGES \
	_IOWR(LKMDBG_IOC_MAGIC, 0x21, struct lkmdbg_image_query_request)
#define LKMDBG_IOC_CREATE_REMOTE_MAP \
	_IOWR(LKMDBG_IOC_MAGIC, 0x22, struct lkmdbg_remote_map_request)
#define LKMDBG_IOC_APPLY_PTE_PATCH \
	_IOWR(LKMDBG_IOC_MAGIC, 0x23, struct lkmdbg_pte_patch_request)
#define LKMDBG_IOC_REMOVE_PTE_PATCH \
	_IOWR(LKMDBG_IOC_MAGIC, 0x24, struct lkmdbg_pte_patch_request)
#define LKMDBG_IOC_QUERY_PTE_PATCHES \
	_IOWR(LKMDBG_IOC_MAGIC, 0x25, struct lkmdbg_pte_patch_query_request)
#define LKMDBG_IOC_READ_PHYS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x26, struct lkmdbg_phys_request)
#define LKMDBG_IOC_WRITE_PHYS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x27, struct lkmdbg_phys_request)
#define LKMDBG_IOC_REMOVE_REMOTE_MAP \
	_IOWR(LKMDBG_IOC_MAGIC, 0x28, struct lkmdbg_remote_map_handle_request)
#define LKMDBG_IOC_QUERY_REMOTE_MAPS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x29, struct lkmdbg_remote_map_query_request)
#define LKMDBG_IOC_SET_SIGNAL_CONFIG \
	_IOWR(LKMDBG_IOC_MAGIC, 0x2A, struct lkmdbg_signal_config_request)
#define LKMDBG_IOC_GET_SIGNAL_CONFIG \
	_IOWR(LKMDBG_IOC_MAGIC, 0x2B, struct lkmdbg_signal_config_request)
#define LKMDBG_IOC_CREATE_REMOTE_ALLOC \
	_IOWR(LKMDBG_IOC_MAGIC, 0x2C, struct lkmdbg_remote_alloc_request)
#define LKMDBG_IOC_REMOVE_REMOTE_ALLOC \
	_IOWR(LKMDBG_IOC_MAGIC, 0x2D, struct lkmdbg_remote_alloc_handle_request)
#define LKMDBG_IOC_QUERY_REMOTE_ALLOCS \
	_IOWR(LKMDBG_IOC_MAGIC, 0x2E, struct lkmdbg_remote_alloc_query_request)
#define LKMDBG_IOC_SET_SYSCALL_TRACE \
	_IOWR(LKMDBG_IOC_MAGIC, 0x2F, struct lkmdbg_syscall_trace_request)
#define LKMDBG_IOC_GET_SYSCALL_TRACE \
	_IOWR(LKMDBG_IOC_MAGIC, 0x30, struct lkmdbg_syscall_trace_request)
#define LKMDBG_IOC_SET_STEALTH \
	_IOWR(LKMDBG_IOC_MAGIC, 0x31, struct lkmdbg_stealth_request)
#define LKMDBG_IOC_GET_STEALTH \
	_IOWR(LKMDBG_IOC_MAGIC, 0x32, struct lkmdbg_stealth_request)
#define LKMDBG_IOC_QUERY_INPUT_DEVICES \
	_IOWR(LKMDBG_IOC_MAGIC, 0x33, struct lkmdbg_input_query_request)
#define LKMDBG_IOC_GET_INPUT_DEVICE_INFO \
	_IOWR(LKMDBG_IOC_MAGIC, 0x34, struct lkmdbg_input_device_info_request)
#define LKMDBG_IOC_OPEN_INPUT_CHANNEL \
	_IOWR(LKMDBG_IOC_MAGIC, 0x35, struct lkmdbg_input_channel_request)
#define LKMDBG_IOC_REMOTE_CALL \
	_IOWR(LKMDBG_IOC_MAGIC, 0x36, struct lkmdbg_remote_call_request)
#define LKMDBG_IOC_RESOLVE_SYSCALL \
	_IOWR(LKMDBG_IOC_MAGIC, 0x37, struct lkmdbg_syscall_resolve_request)
#define LKMDBG_IOC_REMOTE_THREAD_CREATE \
	_IOWR(LKMDBG_IOC_MAGIC, 0x38, struct lkmdbg_remote_thread_create_request)
#define LKMDBG_IOC_SET_EVENT_CONFIG \
	_IOWR(LKMDBG_IOC_MAGIC, 0x39, struct lkmdbg_event_config_request)
#define LKMDBG_IOC_GET_EVENT_CONFIG \
	_IOWR(LKMDBG_IOC_MAGIC, 0x3A, struct lkmdbg_event_config_request)

#endif
