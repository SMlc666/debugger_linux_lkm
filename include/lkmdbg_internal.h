#ifndef _LKMDBG_INTERNAL_H
#define _LKMDBG_INTERNAL_H

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "../hook/lkmdbg_hook_internal.h"
#include "lkmdbg_ioctl.h"

#define LKMDBG_DIR_NAME "lkmdbg"
#define LKMDBG_TARGET_PATH "/proc/version"
#define LKMDBG_SESSION_EVENT_CAPACITY 32
#define LKMDBG_HOOK_NAME_MAX 32

struct mm_struct;
struct seq_file;
struct task_struct;
struct lkmdbg_freezer;
struct perf_event;

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
	bool proc_version_hook_active;
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
};

int lkmdbg_disable_kprobe_blacklist(void);
int lkmdbg_cfi_bypass(void);

struct lkmdbg_session {
	struct list_head node;
	struct mutex lock;
	spinlock_t event_lock;
	wait_queue_head_t readq;
	wait_queue_head_t async_waitq;
	u64 session_id;
	u64 ioctl_calls;
	u64 event_seq;
	u32 event_head;
	u32 event_count;
	pid_t owner_tgid;
	pid_t target_tgid;
	pid_t target_tid;
	struct list_head hwpoints;
	u64 next_hwpoint_id;
	pid_t step_tgid;
	pid_t step_tid;
	bool step_armed;
	atomic_t async_refs;
	struct lkmdbg_freezer *freezer;
	struct lkmdbg_event_record events[LKMDBG_SESSION_EVENT_CAPACITY];
};

extern char *tag;
extern bool hook_proc_version;
extern unsigned int hook_selftest_mode;
extern bool hook_seq_read;
extern bool bypass_kprobe_blacklist;
extern bool bypass_cfi;
extern struct lkmdbg_state lkmdbg_state;
extern struct lkmdbg_symbols lkmdbg_symbols;

int lkmdbg_debugfs_init(void);
void lkmdbg_debugfs_exit(void);

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
int lkmdbg_get_target_mm(struct lkmdbg_session *session,
			 struct mm_struct **mm_out);
int lkmdbg_get_target_identity(struct lkmdbg_session *session, pid_t *tgid_out,
			       pid_t *tid_out);
int lkmdbg_get_target_thread(struct lkmdbg_session *session, pid_t tid_override,
			     struct task_struct **task_out);
long lkmdbg_mem_set_target(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_mem_read(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_mem_write(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_vma_query(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_query_threads(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_get_regs(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_set_regs(struct lkmdbg_session *session, void __user *argp);
int lkmdbg_thread_ctrl_init(void);
void lkmdbg_thread_ctrl_exit(void);
void lkmdbg_thread_ctrl_release(struct lkmdbg_session *session);
long lkmdbg_add_hwpoint(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_remove_hwpoint(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_query_hwpoints(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_single_step(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_freeze_threads(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_thaw_threads(struct lkmdbg_session *session, void __user *argp);
void lkmdbg_session_freeze_release(struct lkmdbg_session *session);
int lkmdbg_session_freeze_on_target_change(struct lkmdbg_session *session);
u32 lkmdbg_freeze_thread_flags(struct lkmdbg_session *session, pid_t tid);
#ifdef CONFIG_COMPAT
long lkmdbg_session_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg);
#endif

#endif
