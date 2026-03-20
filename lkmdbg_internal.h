#ifndef _LKMDBG_INTERNAL_H
#define _LKMDBG_INTERNAL_H

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "lkmdbg_ioctl.h"

#define LKMDBG_DIR_NAME "lkmdbg"
#define LKMDBG_TARGET_PATH "/proc/version"

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
	u64 session_id;
	u64 ioctl_calls;
	pid_t owner_tgid;
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
#ifdef CONFIG_COMPAT
long lkmdbg_session_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg);
#endif

#endif
