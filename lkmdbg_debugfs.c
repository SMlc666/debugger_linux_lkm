#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include "lkmdbg_internal.h"

static int lkmdbg_status_show(struct seq_file *m, void *unused)
{
	u64 reads;
	u64 calls;
	u64 opens;
	u64 sessions;
	u64 sessions_total;
	bool hook_active;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.status_reads++;
	reads = lkmdbg_state.status_reads;
	calls = lkmdbg_state.bootstrap_ioctl_calls;
	opens = lkmdbg_state.proc_open_successes;
	sessions = lkmdbg_state.active_sessions;
	sessions_total = lkmdbg_state.session_opened_total;
	hook_active = lkmdbg_state.proc_version_hook_active;
	mutex_unlock(&lkmdbg_state.lock);

	seq_printf(m, "tag=%s\n", tag);
	seq_printf(m, "load_jiffies=%lu\n", lkmdbg_state.load_jiffies);
	seq_printf(m, "status_reads=%llu\n", (unsigned long long)reads);
	seq_printf(m, "hook_requested=%u\n", hook_proc_version);
	seq_printf(m, "hook_active=%u\n", hook_active);
	seq_printf(m, "bootstrap_ioctl_calls=%llu\n",
		   (unsigned long long)calls);
	seq_printf(m, "proc_open_successes=%llu\n",
		   (unsigned long long)opens);
	seq_printf(m, "active_sessions=%llu\n",
		   (unsigned long long)sessions);
	seq_printf(m, "session_opened_total=%llu\n",
		   (unsigned long long)sessions_total);
	seq_printf(m, "target_path=%s\n", LKMDBG_TARGET_PATH);

	return 0;
}

static int lkmdbg_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, lkmdbg_status_show, inode->i_private);
}

static const struct file_operations lkmdbg_status_fops = {
	.owner = THIS_MODULE,
	.open = lkmdbg_status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int lkmdbg_debugfs_init(void)
{
	lkmdbg_state.debugfs_dir = debugfs_create_dir(LKMDBG_DIR_NAME, NULL);
	if (IS_ERR_OR_NULL(lkmdbg_state.debugfs_dir))
		return -ENOMEM;

	if (!debugfs_create_file("status", 0444, lkmdbg_state.debugfs_dir, NULL,
				 &lkmdbg_status_fops)) {
		debugfs_remove_recursive(lkmdbg_state.debugfs_dir);
		lkmdbg_state.debugfs_dir = NULL;
		return -ENOMEM;
	}

	return 0;
}

void lkmdbg_debugfs_exit(void)
{
	debugfs_remove_recursive(lkmdbg_state.debugfs_dir);
	lkmdbg_state.debugfs_dir = NULL;
}
