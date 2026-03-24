#ifndef _LKMDBG_INTERNAL_H
#define _LKMDBG_INTERNAL_H

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched/mm.h>
#include <linux/spinlock.h>
#include <linux/task_work.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "lkmdbg_compat.h"
#include "../hook/lkmdbg_hook_internal.h"
#include "lkmdbg_ioctl.h"

#define LKMDBG_DIR_NAME "lkmdbg"
#define LKMDBG_TARGET_PATH "/proc/version"
#define LKMDBG_SESSION_EVENT_CAPACITY 32
#define LKMDBG_INPUT_EVENT_CAPACITY 256
#define LKMDBG_HOOK_NAME_MAX 32

struct mm_struct;
struct seq_file;
struct task_struct;
struct vm_area_struct;
struct lkmdbg_freezer;
struct perf_event;
struct lkmdbg_input_channel;

struct lkmdbg_hook_registry_entry {
	struct list_head node;
	u64 hook_id;
	char name[LKMDBG_HOOK_NAME_MAX];
	u64 target;
	u64 origin;
	u64 replacement;
	u64 trampoline;
	u64 hits;
	int last_ret;
	bool active;
};

struct lkmdbg_state {
	struct dentry *debugfs_dir;
	struct mutex lock;
	unsigned long load_jiffies;
	u64 status_reads;
	u64 bootstrap_ioctl_calls;
	u64 proc_open_successes;
	u64 session_opened_total;
	u64 active_sessions;
	u64 next_session_id;
	bool debugfs_active;
	bool proc_version_hook_active;
	bool module_list_hidden;
	bool hook_selftest_enabled;
	bool hook_selftest_exec_pool_ready;
	bool hook_selftest_exec_allocated;
	bool hook_selftest_exec_ready;
	bool hook_selftest_installed;
	int hook_selftest_ret;
	u64 hook_selftest_expected;
	u64 hook_selftest_actual;
	u64 inline_hook_create_total;
	u64 inline_hook_install_total;
	u64 inline_hook_remove_total;
	u64 inline_hook_active;
	int inline_hook_last_ret;
	u64 inline_hook_last_target;
	u64 inline_hook_last_origin;
	u64 inline_hook_last_replacement;
	u64 inline_hook_last_trampoline;
	bool seq_read_hook_active;
	u64 seq_read_hook_hits;
	int seq_read_hook_last_ret;
	atomic64_t hwpoint_callback_total;
	atomic64_t breakpoint_callback_total;
	atomic64_t watchpoint_callback_total;
	atomic64_t target_stop_event_read_total;
	atomic64_t breakpoint_stop_event_read_total;
	atomic64_t watchpoint_stop_event_read_total;
	atomic64_t event_drop_total;
	pid_t hwpoint_last_tgid;
	pid_t hwpoint_last_tid;
	u32 hwpoint_last_reason;
	u32 hwpoint_last_type;
	u64 hwpoint_last_addr;
	u64 hwpoint_last_ip;
	u32 stealth_supported_flags;
};

struct lkmdbg_symbols {
	unsigned long (*kallsyms_lookup_name)(const char *name);
	struct file *(*filp_open)(const char *filename, int flags, umode_t mode);
	int (*filp_close)(struct file *file, fl_owner_t id);
	int (*aarch64_insn_patch_text_nosync)(void *addr, u32 insn);
	int (*aarch64_insn_write)(void *addr, u32 insn);
	void (*flush_icache_range)(unsigned long start, unsigned long end);
	int (*set_memory_x)(unsigned long addr, int numpages);
	void *(*module_alloc)(unsigned long size);
	void (*module_memfree)(void *region);
	struct mm_struct *init_mm;
	unsigned long task_work_add_sym;
	unsigned long task_work_cancel_match_sym;
	unsigned long task_work_cancel_func_sym;
	unsigned long task_work_cancel_sym;
	unsigned long register_user_step_hook_sym;
	unsigned long unregister_user_step_hook_sym;
	unsigned long user_enable_single_step_sym;
	unsigned long user_disable_single_step_sym;
	unsigned long for_each_kernel_tracepoint_sym;
	unsigned long tracepoint_probe_register_sym;
	unsigned long tracepoint_probe_unregister_sym;
	unsigned long perf_event_disable_local_sym;
	unsigned long do_page_fault_inner_sym;
	unsigned long do_page_fault_sym;
	unsigned long process_vm_rw_sym;
	unsigned long do_sys_process_vm_writev_sym;
	unsigned long access_remote_vm_sym;
	unsigned long access_remote_vm_inner_sym;
};

struct lkmdbg_target_vma_info {
	u64 start_addr;
	u64 end_addr;
	u64 pgoff;
	u64 inode;
	u64 vm_flags_raw;
	u32 prot;
	u32 flags;
	u32 dev_major;
	u32 dev_minor;
};

#define LKMDBG_TARGET_PT_FLAG_PRESENT 0x00000001U
#define LKMDBG_TARGET_PT_FLAG_HUGE 0x00000002U

struct lkmdbg_target_pt_info {
	u64 entry_raw;
	u64 phys_addr;
	u32 level;
	u32 page_shift;
	u32 flags;
	u32 pt_flags;
};

enum lkmdbg_remote_call_phase {
	LKMDBG_REMOTE_CALL_NONE = 0,
	LKMDBG_REMOTE_CALL_PREPARED = 1,
	LKMDBG_REMOTE_CALL_RUNNING = 2,
	LKMDBG_REMOTE_CALL_PARKED = 3,
};

struct lkmdbg_remote_call_state {
	struct lkmdbg_session *session;
	struct perf_event *return_event;
	struct callback_head park_work;
	struct lkmdbg_regs_arm64 saved_regs;
	u64 call_id;
	u64 target_pc;
	u64 return_pc;
	u64 return_value;
	pid_t tgid;
	pid_t tid;
	u32 phase;
	bool parked;
	bool resume;
	bool park_work_queued;
};

int lkmdbg_disable_kprobe_blacklist(void);
int lkmdbg_cfi_bypass(void);

struct lkmdbg_session {
	struct list_head node;
	struct mutex lock;
	spinlock_t event_lock;
	wait_queue_head_t readq;
	wait_queue_head_t async_waitq;
	wait_queue_head_t remote_call_waitq;
	u64 session_id;
	u64 ioctl_calls;
	u64 event_seq;
	u32 event_head;
	u32 event_count;
	u32 event_drop_pending;
	pid_t owner_tgid;
	pid_t target_tgid;
	pid_t target_tid;
	u64 target_gen;
	u64 event_drop_count;
	struct list_head hwpoints;
	u64 next_hwpoint_id;
	struct list_head pte_patches;
	u64 next_pte_patch_id;
	struct list_head remote_maps;
	u64 next_remote_map_id;
	struct list_head remote_allocs;
	u64 next_remote_alloc_id;
	struct list_head input_channels;
	u64 next_input_channel_id;
	u64 signal_mask_words[2];
	u32 signal_flags;
	s32 syscall_trace_tid;
	s32 syscall_trace_nr;
	u32 syscall_trace_mode;
	u32 syscall_trace_phases;
	pid_t step_tgid;
	pid_t step_tid;
	bool step_armed;
	bool closing;
	atomic_t async_refs;
	struct lkmdbg_freezer *freezer;
	struct work_struct stop_work;
	bool stop_work_pending;
	u64 stop_work_target_gen;
	u64 next_stop_cookie;
	u64 next_remote_call_id;
	struct lkmdbg_stop_state stop_state;
	struct lkmdbg_stop_state pending_stop;
	struct lkmdbg_remote_call_state remote_call;
	struct lkmdbg_event_record events[LKMDBG_SESSION_EVENT_CAPACITY];
};

extern char *tag;
extern bool hook_proc_version;
extern unsigned int hook_selftest_mode;
extern bool hook_seq_read;
extern bool enable_debugfs;
extern bool bypass_kprobe_blacklist;
extern bool bypass_cfi;
extern struct lkmdbg_state lkmdbg_state;
extern struct lkmdbg_symbols lkmdbg_symbols;

int lkmdbg_debugfs_init(void);
void lkmdbg_debugfs_exit(void);
int lkmdbg_debugfs_set_visible(bool visible);
bool lkmdbg_debugfs_is_active(void);

int lkmdbg_stealth_init(void);
void lkmdbg_stealth_exit(void);
u32 lkmdbg_stealth_current_flags(void);
u32 lkmdbg_stealth_supported_flags(void);
long lkmdbg_set_stealth(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_get_stealth(struct lkmdbg_session *session, void __user *argp);

int lkmdbg_symbols_init(void);
void lkmdbg_symbols_exit(void);

int lkmdbg_hook_registry_debugfs_show(struct seq_file *m);
struct lkmdbg_hook_registry_entry *
lkmdbg_hook_registry_register(const char *name, void *target,
			      void *replacement);
void lkmdbg_hook_registry_mark_installed(
	struct lkmdbg_hook_registry_entry *entry, void *origin,
	void *trampoline, int ret);
void lkmdbg_hook_registry_note_hit(struct lkmdbg_hook_registry_entry *entry);
void lkmdbg_hook_registry_unregister(struct lkmdbg_hook_registry_entry *entry,
					 int ret);

int lkmdbg_runtime_hooks_init(void);
void lkmdbg_runtime_hooks_exit(void);

int lkmdbg_transport_init(void);
void lkmdbg_transport_exit(void);

int lkmdbg_input_init(void);
void lkmdbg_input_exit(void);
void lkmdbg_input_release_session(struct lkmdbg_session *session);
long lkmdbg_query_input_devices(struct lkmdbg_session *session,
				void __user *argp);
long lkmdbg_get_input_device_info(struct lkmdbg_session *session,
				 void __user *argp);
long lkmdbg_open_input_channel(struct lkmdbg_session *session,
			       void __user *argp);

int lkmdbg_open_session(void __user *argp);
long lkmdbg_session_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg);
ssize_t lkmdbg_session_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos);
__poll_t lkmdbg_session_poll(struct file *file, poll_table *wait);
void lkmdbg_session_queue_event_ex(struct lkmdbg_session *session, u32 type,
				   u32 code, pid_t tgid, pid_t tid, u32 flags,
				   u64 value0, u64 value1);
struct lkmdbg_session *lkmdbg_session_consume_single_step(pid_t tgid,
							  pid_t tid);
void lkmdbg_session_async_put(struct lkmdbg_session *session);
void lkmdbg_session_broadcast_event(u32 type, u64 value0, u64 value1);
void lkmdbg_session_broadcast_target_event(pid_t target_tgid, u32 type,
					   u32 code, pid_t tid, u32 flags,
					   u64 value0, u64 value1);
void lkmdbg_session_broadcast_signal_event(pid_t target_tgid, u32 sig,
					   pid_t tid, u32 flags,
					   u64 siginfo_code, int result);
void lkmdbg_session_broadcast_syscall_event(
	pid_t target_tgid, pid_t tid, u32 phase, s32 syscall_nr, s64 retval,
	const struct lkmdbg_regs_arm64 *regs);
int lkmdbg_get_target_mm(struct lkmdbg_session *session,
			 struct mm_struct **mm_out);
int lkmdbg_get_target_identity(struct lkmdbg_session *session, pid_t *tgid_out,
			       pid_t *tid_out);
int lkmdbg_get_target_thread(struct lkmdbg_session *session, pid_t tid_override,
			     struct task_struct **task_out);
u32 lkmdbg_target_vm_prot_bits(u64 vm_flags);
const char *lkmdbg_target_vma_special_name(struct mm_struct *mm,
					   struct vm_area_struct *vma,
					   u32 *flags);
void lkmdbg_target_vma_fill_info(struct mm_struct *mm,
				 struct vm_area_struct *vma,
				 struct lkmdbg_target_vma_info *info);
int lkmdbg_target_vma_lookup_locked(struct mm_struct *mm, u64 addr, u64 length,
				    struct vm_area_struct **vma_out);
int lkmdbg_target_pt_lookup_locked(struct mm_struct *mm, unsigned long addr,
				   struct lkmdbg_target_pt_info *info);
bool lkmdbg_pte_allows_access(pte_t pte, unsigned long vm_flags, u32 type);
pte_t lkmdbg_pte_set_exec(pte_t pte, bool executable);
pte_t lkmdbg_pte_set_user_read(pte_t pte, bool readable);
pte_t lkmdbg_pte_make_exec_only(pte_t pte);
pte_t lkmdbg_pte_make_protnone(pte_t pte);
pte_t lkmdbg_pte_make_writable(pte_t pte);
pte_t lkmdbg_pte_build_alias_pte(struct page *page, pte_t template, u32 prot);
bool lkmdbg_pte_equivalent(pte_t current_pte, pte_t expected_pte);
int lkmdbg_pte_lookup_locked(struct mm_struct *mm, unsigned long addr,
			     pte_t **ptep_out, spinlock_t **ptl_out);
int lkmdbg_pte_rewrite_locked(struct mm_struct *mm, unsigned long addr,
			      pte_t new_pte, pte_t *old_pte_out,
			      unsigned long *vm_flags_out);
int lkmdbg_pte_read_locked(struct mm_struct *mm, unsigned long addr,
			   pte_t *pte_out, unsigned long *vm_flags_out);
int lkmdbg_pte_capture(struct mm_struct *mm, unsigned long addr,
		       pte_t *pte_out, unsigned long *vm_flags_out);
long lkmdbg_mem_set_target(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_mem_read(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_mem_write(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_phys_read(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_phys_write(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_page_query(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_vma_query(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_image_query(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_create_remote_map(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_remove_remote_map(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_query_remote_maps(struct lkmdbg_session *session, void __user *argp);
void lkmdbg_remote_map_release_session(struct lkmdbg_session *session);
long lkmdbg_create_remote_alloc(struct lkmdbg_session *session,
				void __user *argp);
long lkmdbg_remove_remote_alloc(struct lkmdbg_session *session,
				void __user *argp);
long lkmdbg_query_remote_allocs(struct lkmdbg_session *session,
				void __user *argp);
void lkmdbg_remote_alloc_release_session(struct lkmdbg_session *session);
bool lkmdbg_remote_alloc_has_overlap_locked(struct lkmdbg_session *session,
					    unsigned long start,
					    unsigned long length);
long lkmdbg_apply_pte_patch(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_remove_pte_patch(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_query_pte_patches(struct lkmdbg_session *session, void __user *argp);
void lkmdbg_pte_patch_release(struct lkmdbg_session *session);
int lkmdbg_pte_patch_on_target_change(struct lkmdbg_session *session);
bool lkmdbg_pte_patch_has_overlap_locked(struct lkmdbg_session *session,
					 unsigned long start,
					 unsigned long length);
long lkmdbg_query_threads(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_get_regs(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_set_regs(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_set_signal_config(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_get_signal_config(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_set_syscall_trace(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_get_syscall_trace(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_get_stop_state(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_continue_target(struct lkmdbg_session *session, void __user *argp);
void lkmdbg_session_commit_stop(struct lkmdbg_session *session, u32 reason,
				pid_t tgid, pid_t tid, u32 event_flags,
				u32 stop_flags, u64 value0, u64 value1,
				const struct lkmdbg_regs_arm64 *regs);
void lkmdbg_session_request_async_stop(struct lkmdbg_session *session,
				       u32 reason, pid_t tgid, pid_t tid,
				       u32 event_flags, u32 stop_flags,
				       u64 value0, u64 value1,
				       const struct lkmdbg_regs_arm64 *regs);
void lkmdbg_session_clear_stop(struct lkmdbg_session *session);
int lkmdbg_thread_ctrl_init(void);
void lkmdbg_thread_ctrl_exit(void);
void lkmdbg_thread_ctrl_release(struct lkmdbg_session *session);
long lkmdbg_add_hwpoint(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_remove_hwpoint(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_query_hwpoints(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_rearm_hwpoint(struct lkmdbg_session *session, void __user *argp);
int lkmdbg_rearm_all_hwpoints(struct lkmdbg_session *session);
int lkmdbg_prepare_continue_hwpoints(struct lkmdbg_session *session,
				     const struct lkmdbg_stop_state *stop,
				     u32 flags);
long lkmdbg_single_step(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_remote_call(struct lkmdbg_session *session, void __user *argp);
u32 lkmdbg_remote_call_thread_flags(struct lkmdbg_session *session, pid_t tid);
int lkmdbg_remote_call_prepare_continue(struct lkmdbg_session *session,
					const struct lkmdbg_stop_state *stop);
void lkmdbg_remote_call_rollback_continue(
	struct lkmdbg_session *session,
	const struct lkmdbg_stop_state *stop);
int lkmdbg_remote_call_finish_continue(struct lkmdbg_session *session,
				       const struct lkmdbg_stop_state *stop);
bool lkmdbg_remote_call_blocks_target_change(struct lkmdbg_session *session);
bool lkmdbg_remote_call_blocks_manual_thaw(struct lkmdbg_session *session);
void lkmdbg_remote_call_fail_stop(struct lkmdbg_session *session,
				  const struct lkmdbg_stop_state *stop);
void lkmdbg_remote_call_release(struct lkmdbg_session *session);
long lkmdbg_freeze_threads(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_thaw_threads(struct lkmdbg_session *session, void __user *argp);
int lkmdbg_session_freeze_target(struct lkmdbg_session *session,
				 u32 timeout_ms,
				 struct lkmdbg_freeze_request *req_out);
int lkmdbg_session_thaw_target(struct lkmdbg_session *session, u32 timeout_ms,
			       struct lkmdbg_freeze_request *req_out);
void lkmdbg_session_freeze_release(struct lkmdbg_session *session);
int lkmdbg_session_freeze_on_target_change(struct lkmdbg_session *session);
u32 lkmdbg_freeze_thread_flags(struct lkmdbg_session *session, pid_t tid);
#ifdef CONFIG_COMPAT
long lkmdbg_session_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg);
#endif

#endif
