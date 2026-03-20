#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "lkmdbg_internal.h"

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

static long lkmdbg_bootstrap_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.bootstrap_ioctl_calls++;
	mutex_unlock(&lkmdbg_state.lock);

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
		mutex_lock(&lkmdbg_state.lock);
		lkmdbg_state.proc_open_successes++;
		mutex_unlock(&lkmdbg_state.lock);
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

int lkmdbg_transport_init(void)
{
	struct file *filp;
	struct inode *inode;

	if (!hook_proc_version)
		return 0;

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

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.proc_version_hook_active = true;
	mutex_unlock(&lkmdbg_state.lock);

	return 0;
}

void lkmdbg_transport_exit(void)
{
	if (proc_version_inode && proc_version_orig_fops)
		WRITE_ONCE(proc_version_inode->i_fop, proc_version_orig_fops);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.proc_version_hook_active = false;
	mutex_unlock(&lkmdbg_state.lock);

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
