#ifndef _LKMDBG_INTERNAL_H
#define _LKMDBG_INTERNAL_H

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

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
};

struct lkmdbg_session {
	struct mutex lock;
	wait_queue_head_t readq;
	u64 session_id;
	u64 ioctl_calls;
	u64 event_seq;
	u32 event_head;
	u32 event_count;
	pid_t owner_tgid;
	struct lkmdbg_event_record events[LKMDBG_SESSION_EVENT_CAPACITY];
};

extern char *tag;
extern bool hook_proc_version;
extern struct lkmdbg_state lkmdbg_state;

int lkmdbg_debugfs_init(void);
void lkmdbg_debugfs_exit(void);

int lkmdbg_transport_init(void);
void lkmdbg_transport_exit(void);

int lkmdbg_open_session(void __user *argp);
long lkmdbg_session_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg);
ssize_t lkmdbg_session_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos);
__poll_t lkmdbg_session_poll(struct file *file, poll_table *wait);
#ifdef CONFIG_COMPAT
long lkmdbg_session_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg);
#endif

#endif
