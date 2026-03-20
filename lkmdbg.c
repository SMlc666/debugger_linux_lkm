#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "lkmdbg_ioctl.h"

#define LKMDBG_DIR_NAME "lkmdbg"
#define LKMDBG_TARGET_PATH "/proc/version"

static char *tag = "arm64-gki-smoke";
module_param(tag, charp, 0444);
MODULE_PARM_DESC(tag, "Human-readable tag shown in debugfs output");

static bool hook_proc_version;
module_param(hook_proc_version, bool, 0444);
MODULE_PARM_DESC(hook_proc_version,
		 "Clone and swap /proc/version inode file_operations to add hidden ioctl support");

static struct dentry *lkmdbg_dir;
static DEFINE_MUTEX(lkmdbg_lock);
static unsigned long load_jiffies;
static u64 status_reads;
static u64 bootstrap_ioctl_calls;
static u64 proc_open_successes;
static u64 session_opened_total;
static u64 active_sessions;
static u64 next_session_id;

struct lkmdbg_session {
	struct mutex lock;
	u64 session_id;
	u64 ioctl_calls;
	pid_t owner_tgid;
};

static struct inode *proc_version_inode;
static const struct file_operations *proc_version_orig_fops;
static struct file_operations *proc_version_hook_fops;
static int (*proc_version_orig_open)(struct inode *inode, struct file *file);
static int (*proc_version_orig_release)(struct inode *inode, struct file *file);
static long (*proc_version_orig_ioctl)(struct file *file, unsigned int cmd,
				       unsigned long arg);
#ifdef CONFIG_COMPAT
static long (*proc_version_orig_compat_ioctl)(struct file *file,
					      unsigned int cmd,
					      unsigned long arg);
#endif
static bool proc_version_hook_active;

static int lkmdbg_session_copy_status_to_user(struct lkmdbg_session *session,
					      void __user *argp)
{
	struct lkmdbg_status_reply reply;

	memset(&reply, 0, sizeof(reply));

	mutex_lock(&lkmdbg_lock);
	reply.version = LKMDBG_PROTO_VERSION;
	reply.size = sizeof(reply);
	reply.hook_requested = hook_proc_version;
	reply.hook_active = proc_version_hook_active;
	reply.active_sessions = active_sessions;
	reply.load_jiffies = load_jiffies;
	reply.status_reads = status_reads;
	reply.bootstrap_ioctl_calls = bootstrap_ioctl_calls;
	reply.session_opened_total = session_opened_total;
	reply.open_successes = proc_open_successes;
	mutex_unlock(&lkmdbg_lock);

	mutex_lock(&session->lock);
	reply.owner_tgid = session->owner_tgid;
	reply.session_id = session->session_id;
	reply.session_ioctl_calls = session->ioctl_calls;
	mutex_unlock(&session->lock);

	if (copy_to_user(argp, &reply, sizeof(reply)))
		return -EFAULT;

	return 0;
}

static int lkmdbg_session_release(struct inode *inode, struct file *file)
{
	struct lkmdbg_session *session = file->private_data;

	mutex_lock(&lkmdbg_lock);
	if (active_sessions > 0)
		active_sessions--;
	mutex_unlock(&lkmdbg_lock);

	kfree(session);
	return 0;
}

static long lkmdbg_session_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct lkmdbg_session *session = file->private_data;
	void __user *argp = (void __user *)arg;
	pid_t owner;

	if (!session)
		return -ENXIO;

	mutex_lock(&session->lock);
	session->ioctl_calls++;
	owner = session->owner_tgid;
	mutex_unlock(&session->lock);

	if (current->tgid != owner && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	switch (cmd) {
	case LKMDBG_IOC_GET_STATUS:
		return lkmdbg_session_copy_status_to_user(session, argp);
	case LKMDBG_IOC_RESET_SESSION:
		mutex_lock(&session->lock);
		session->ioctl_calls = 0;
		mutex_unlock(&session->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
static long lkmdbg_session_compat_ioctl(struct file *file, unsigned int cmd,
					unsigned long arg)
{
	return lkmdbg_session_ioctl(file, cmd, arg);
}
#endif

static const struct file_operations lkmdbg_session_fops = {
	.owner = THIS_MODULE,
	.release = lkmdbg_session_release,
	.unlocked_ioctl = lkmdbg_session_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lkmdbg_session_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static int lkmdbg_open_session(void __user *argp)
{
	struct lkmdbg_open_session_request req;
	struct lkmdbg_session *session;
	int fd;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req))
		return -EINVAL;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		return -ENOMEM;

	mutex_init(&session->lock);
	session->owner_tgid = current->tgid;

	mutex_lock(&lkmdbg_lock);
	next_session_id++;
	session->session_id = next_session_id;
	mutex_unlock(&lkmdbg_lock);

	fd = anon_inode_getfd("lkmdbg", &lkmdbg_session_fops, session,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		kfree(session);
		return fd;
	}

	mutex_lock(&lkmdbg_lock);
	session_opened_total++;
	active_sessions++;
	mutex_unlock(&lkmdbg_lock);

	return fd;
}

static long lkmdbg_bootstrap_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	mutex_lock(&lkmdbg_lock);
	bootstrap_ioctl_calls++;
	mutex_unlock(&lkmdbg_lock);

	switch (cmd) {
	case LKMDBG_IOC_OPEN_SESSION:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return lkmdbg_open_session(argp);
	default:
		if (proc_version_orig_ioctl)
			return proc_version_orig_ioctl(file, cmd, arg);
		return -ENOTTY;
	}
}

static int lkmdbg_proc_version_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	if (proc_version_orig_open)
		ret = proc_version_orig_open(inode, file);
	if (!ret) {
		mutex_lock(&lkmdbg_lock);
		proc_open_successes++;
		mutex_unlock(&lkmdbg_lock);
	}

	return ret;
}

static int lkmdbg_proc_version_release(struct inode *inode, struct file *file)
{
	if (proc_version_orig_release)
		return proc_version_orig_release(inode, file);

	return 0;
}

static long lkmdbg_proc_version_ioctl(struct file *file, unsigned int cmd,
				      unsigned long arg)
{
	return lkmdbg_bootstrap_ioctl(file, cmd, arg);
}

#ifdef CONFIG_COMPAT
static long lkmdbg_proc_version_compat_ioctl(struct file *file,
					     unsigned int cmd,
					     unsigned long arg)
{
	if (_IOC_TYPE(cmd) == LKMDBG_IOC_MAGIC)
		return lkmdbg_bootstrap_ioctl(file, cmd, arg);

	if (proc_version_orig_compat_ioctl)
		return proc_version_orig_compat_ioctl(file, cmd, arg);

	if (proc_version_orig_ioctl)
		return proc_version_orig_ioctl(file, cmd, arg);

	return -ENOTTY;
}
#endif

static int lkmdbg_install_proc_version_hook(void)
{
	struct file *filp;
	struct inode *inode;

	filp = filp_open(LKMDBG_TARGET_PATH, O_RDONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	inode = file_inode(filp);
	if (!inode || !inode->i_fop) {
		filp_close(filp, NULL);
		return -ENOENT;
	}

	proc_version_inode = igrab(inode);
	if (!proc_version_inode) {
		filp_close(filp, NULL);
		return -ENOENT;
	}

	proc_version_orig_fops = inode->i_fop;
	proc_version_hook_fops = kmemdup(proc_version_orig_fops,
					 sizeof(*proc_version_hook_fops),
					 GFP_KERNEL);
	filp_close(filp, NULL);
	if (!proc_version_hook_fops) {
		iput(proc_version_inode);
		proc_version_inode = NULL;
		return -ENOMEM;
	}

	proc_version_orig_open = proc_version_orig_fops->open;
	proc_version_orig_release = proc_version_orig_fops->release;
	proc_version_orig_ioctl = proc_version_orig_fops->unlocked_ioctl;
#ifdef CONFIG_COMPAT
	proc_version_orig_compat_ioctl = proc_version_orig_fops->compat_ioctl;
#endif

	proc_version_hook_fops->owner = THIS_MODULE;
	proc_version_hook_fops->open = lkmdbg_proc_version_open;
	proc_version_hook_fops->release = lkmdbg_proc_version_release;
	proc_version_hook_fops->unlocked_ioctl = lkmdbg_proc_version_ioctl;
#ifdef CONFIG_COMPAT
	proc_version_hook_fops->compat_ioctl = lkmdbg_proc_version_compat_ioctl;
#endif

	WRITE_ONCE(proc_version_inode->i_fop, proc_version_hook_fops);
	proc_version_hook_active = true;

	return 0;
}

static void lkmdbg_remove_proc_version_hook(void)
{
	if (proc_version_inode && proc_version_orig_fops)
		WRITE_ONCE(proc_version_inode->i_fop, proc_version_orig_fops);

	proc_version_hook_active = false;
	kfree(proc_version_hook_fops);
	proc_version_hook_fops = NULL;

	if (proc_version_inode) {
		iput(proc_version_inode);
		proc_version_inode = NULL;
	}

	proc_version_orig_fops = NULL;
	proc_version_orig_open = NULL;
	proc_version_orig_release = NULL;
	proc_version_orig_ioctl = NULL;
#ifdef CONFIG_COMPAT
	proc_version_orig_compat_ioctl = NULL;
#endif
}

static int lkmdbg_status_show(struct seq_file *m, void *unused)
{
	u64 reads;
	u64 calls;
	u64 opens;
	u64 sessions;
	u64 sessions_total;
	bool hook_active;

	mutex_lock(&lkmdbg_lock);
	status_reads++;
	reads = status_reads;
	calls = bootstrap_ioctl_calls;
	opens = proc_open_successes;
	sessions = active_sessions;
	sessions_total = session_opened_total;
	hook_active = proc_version_hook_active;
	mutex_unlock(&lkmdbg_lock);

	seq_printf(m, "tag=%s\n", tag);
	seq_printf(m, "load_jiffies=%lu\n", load_jiffies);
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

static int __init lkmdbg_init(void)
{
	int ret;

	load_jiffies = jiffies;

	lkmdbg_dir = debugfs_create_dir(LKMDBG_DIR_NAME, NULL);
	if (IS_ERR_OR_NULL(lkmdbg_dir))
		return -ENOMEM;

	if (!debugfs_create_file("status", 0444, lkmdbg_dir, NULL,
				 &lkmdbg_status_fops)) {
		debugfs_remove_recursive(lkmdbg_dir);
		return -ENOMEM;
	}

	if (hook_proc_version) {
		ret = lkmdbg_install_proc_version_hook();
		if (ret) {
			debugfs_remove_recursive(lkmdbg_dir);
			return ret;
		}
	}

	pr_info("lkmdbg: loaded tag=%s hook_proc_version=%u\n", tag,
		hook_proc_version);
	return 0;
}

static void __exit lkmdbg_exit(void)
{
	lkmdbg_remove_proc_version_hook();
	debugfs_remove_recursive(lkmdbg_dir);
	pr_info("lkmdbg: unloaded\n");
}

MODULE_AUTHOR("OpenAI Codex");
MODULE_DESCRIPTION("Minimal Linux kernel module scaffold for arm64 Android GKI debugger work");
MODULE_LICENSE("GPL");

module_init(lkmdbg_init);
module_exit(lkmdbg_exit);
