#ifndef _LKMDBG_INTERNAL_H
#define _LKMDBG_INTERNAL_H

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "lkmdbg_hook.h"
#include "lkmdbg_ioctl.h"

#define LKMDBG_DIR_NAME "lkmdbg"
#define LKMDBG_TARGET_PATH "/proc/version"
#define LKMDBG_SESSION_EVENT_CAPACITY 32

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
	bool hook_selftest_installed;
	int hook_selftest_ret;
	u64 hook_selftest_expected;
	u64 hook_selftest_actual;
};

struct lkmdbg_symbols {
	unsigned long (*kallsyms_lookup_name)(const char *name);
	struct file *(*filp_open)(const char *filename, int flags, umode_t mode);
	int (*filp_close)(struct file *file, fl_owner_t id);
	int (*access_process_vm)(struct task_struct *tsk, unsigned long addr,
				 void *buf, int len, unsigned int gup_flags);
	int (*aarch64_insn_write)(void *addr, u32 insn);
	void (*flush_icache_range)(unsigned long start, unsigned long end);
	void *(*module_alloc)(unsigned long size);
	void (*module_memfree)(void *region);
};

int lkmdbg_disable_kprobe_blacklist(void);
int lkmdbg_cfi_bypass(void);

struct lkmdbg_session {
	struct mutex lock;
	wait_queue_head_t readq;
	u64 session_id;
	u64 ioctl_calls;
	u64 event_seq;
	u32 event_head;
	u32 event_count;
	pid_t owner_tgid;
	pid_t target_tgid;
	struct lkmdbg_event_record events[LKMDBG_SESSION_EVENT_CAPACITY];
};

extern char *tag;
extern bool hook_proc_version;
extern bool hook_selftest;
extern struct lkmdbg_state lkmdbg_state;
extern struct lkmdbg_symbols lkmdbg_symbols;

int lkmdbg_debugfs_init(void);
void lkmdbg_debugfs_exit(void);

int lkmdbg_symbols_init(void);
void lkmdbg_symbols_exit(void);

int lkmdbg_transport_init(void);
void lkmdbg_transport_exit(void);

int lkmdbg_open_session(void __user *argp);
long lkmdbg_session_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg);
ssize_t lkmdbg_session_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos);
__poll_t lkmdbg_session_poll(struct file *file, poll_table *wait);
long lkmdbg_mem_set_target(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_mem_read(struct lkmdbg_session *session, void __user *argp);
long lkmdbg_mem_write(struct lkmdbg_session *session, void __user *argp);
#ifdef CONFIG_COMPAT
long lkmdbg_session_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg);
#endif

#endif
