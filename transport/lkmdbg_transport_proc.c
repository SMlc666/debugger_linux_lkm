#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/slab.h>

#include "lkmdbg_internal.h"

static struct inode *proc_version_inode;
static struct path proc_version_path;
static bool proc_version_path_valid;
static const struct file_operations *proc_version_orig_fops;
static struct file_operations *proc_version_hook_fops;
static struct lkmdbg_inline_hook *proc_version_open_hook;
static struct lkmdbg_hook_registry_entry *proc_version_open_registry;
static struct lkmdbg_inline_hook *proc_version_ioctl_hook;
static struct lkmdbg_hook_registry_entry *proc_version_ioctl_registry;
static int (*proc_version_orig_open)(struct inode *inode, struct file *file);
static int (*proc_version_orig_release)(struct inode *inode, struct file *file);
static long (*proc_version_orig_ioctl)(struct file *file, unsigned int cmd,
				       unsigned long arg);
static atomic_t proc_version_open_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(proc_version_open_waitq);
#ifdef CONFIG_COMPAT
static struct lkmdbg_inline_hook *proc_version_compat_ioctl_hook;
static struct lkmdbg_hook_registry_entry *proc_version_compat_ioctl_registry;
static long (*proc_version_orig_compat_ioctl)(struct file *file,
					      unsigned int cmd,
					      unsigned long arg);
#endif

static bool lkmdbg_proc_version_matches(struct inode *inode,
					const struct file *file)
{
	const struct dentry *target_dentry;
	const struct dentry *file_dentry;

	if (!proc_version_inode || !inode)
		return false;

	if (inode == proc_version_inode)
		return true;

	if (inode->i_sb == proc_version_inode->i_sb &&
	    inode->i_ino == proc_version_inode->i_ino)
		return true;

	if (!file || !proc_version_path_valid)
		return false;

	target_dentry = proc_version_path.dentry;
	file_dentry = file->f_path.dentry;
	if (!target_dentry || !file_dentry)
		return false;

	if (file_dentry == target_dentry)
		return true;

	return file_dentry->d_sb == target_dentry->d_sb &&
	       file_dentry->d_inode &&
	       file_dentry->d_inode->i_ino == target_dentry->d_inode->i_ino;
}

static long lkmdbg_bootstrap_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	long ret;

	(void)file;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.bootstrap_ioctl_calls++;
	mutex_unlock(&lkmdbg_state.lock);

	if (cmd != LKMDBG_IOC_OPEN_SESSION)
		return -ENOTTY;

	if (!capable(CAP_SYS_ADMIN))
		return -ENOTTY;

	ret = lkmdbg_open_session(argp);
	if (ret == -EPERM || ret == -EINVAL || ret == -EFAULT)
		return -ENOTTY;

	return ret;
}

static long __nocfi lkmdbg_proc_version_direct_ioctl(struct file *file,
						     unsigned int cmd,
						     unsigned long arg)
{
	struct lkmdbg_hook_registry_entry *registry;

	if (lkmdbg_proc_version_matches(file_inode(file), file)) {
		registry = READ_ONCE(proc_version_ioctl_registry);
		if (registry)
			lkmdbg_hook_registry_note_hit(registry);
		if (_IOC_TYPE(cmd) == LKMDBG_IOC_MAGIC)
			return lkmdbg_bootstrap_ioctl(file, cmd, arg);
	}

	if (proc_version_orig_ioctl)
		return proc_version_orig_ioctl(file, cmd, arg);

	return -ENOTTY;
}

static int __nocfi lkmdbg_proc_version_open(struct inode *inode,
					    struct file *file)
{
	struct lkmdbg_hook_registry_entry *registry;
	int (*orig_open)(struct inode *inode, struct file *file);
	struct file_operations *hook_fops;
	const struct file_operations *new_fops;
	const struct file_operations *old_fops;
	int ret = 0;

	atomic_inc(&proc_version_open_inflight);

	orig_open = READ_ONCE(proc_version_orig_open);
	if (orig_open)
		ret = orig_open(inode, file);
	if (ret)
		goto out;

	if (!lkmdbg_proc_version_matches(inode, file))
		goto out;

	registry = READ_ONCE(proc_version_open_registry);
	if (registry)
		lkmdbg_hook_registry_note_hit(registry);

	hook_fops = READ_ONCE(proc_version_hook_fops);
	if (!hook_fops) {
		ret = -ENOENT;
		goto out;
	}

	new_fops = fops_get(hook_fops);
	if (!new_fops) {
		ret = -ENOENT;
		goto out;
	}

	old_fops = file->f_op;
	WRITE_ONCE(file->f_op, new_fops);
	fops_put(old_fops);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.proc_open_successes++;
	mutex_unlock(&lkmdbg_state.lock);

out:
	if (atomic_dec_and_test(&proc_version_open_inflight))
		wake_up_all(&proc_version_open_waitq);

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
	if (_IOC_TYPE(cmd) == LKMDBG_IOC_MAGIC)
		return lkmdbg_bootstrap_ioctl(file, cmd, arg);

	if (proc_version_orig_ioctl)
		return proc_version_orig_ioctl(file, cmd, arg);

	return -ENOTTY;
}

#ifdef CONFIG_COMPAT
static long __nocfi lkmdbg_proc_version_direct_compat_ioctl(
	struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lkmdbg_hook_registry_entry *registry;

	if (lkmdbg_proc_version_matches(file_inode(file), file)) {
		registry = READ_ONCE(proc_version_compat_ioctl_registry);
		if (registry)
			lkmdbg_hook_registry_note_hit(registry);
		if (_IOC_TYPE(cmd) == LKMDBG_IOC_MAGIC)
			return lkmdbg_bootstrap_ioctl(file, cmd, arg);
	}

	if (proc_version_orig_compat_ioctl)
		return proc_version_orig_compat_ioctl(file, cmd, arg);

	if (proc_version_orig_ioctl)
		return proc_version_orig_ioctl(file, cmd, arg);

	return -ENOTTY;
}

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

static void lkmdbg_transport_clear_proc_version_state(void)
{
	proc_version_inode = NULL;
	proc_version_orig_fops = NULL;
	proc_version_hook_fops = NULL;
	proc_version_orig_open = NULL;
	proc_version_orig_release = NULL;
	proc_version_orig_ioctl = NULL;
	proc_version_path_valid = false;
#ifdef CONFIG_COMPAT
	proc_version_orig_compat_ioctl = NULL;
#endif
}

static int lkmdbg_transport_install_proc_version_ioctl_hook(void)
{
	void *orig_ioctl = NULL;
	int ret;

	if (!proc_version_orig_ioctl)
		return 0;

	proc_version_ioctl_registry = lkmdbg_hook_registry_register(
		"proc_version_ioctl", proc_version_orig_ioctl,
		lkmdbg_proc_version_direct_ioctl);
	if (!proc_version_ioctl_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install((void *)proc_version_orig_ioctl,
				  lkmdbg_proc_version_direct_ioctl,
				  &proc_version_ioctl_hook, &orig_ioctl);
	if (ret) {
		lkmdbg_hook_registry_unregister(proc_version_ioctl_registry, ret);
		proc_version_ioctl_registry = NULL;
		return ret;
	}

	proc_version_orig_ioctl = orig_ioctl;
	lkmdbg_hook_registry_mark_installed(proc_version_ioctl_registry,
					    proc_version_orig_fops->unlocked_ioctl,
					    orig_ioctl, 0);
	return 0;
}

#ifdef CONFIG_COMPAT
static int lkmdbg_transport_install_proc_version_compat_ioctl_hook(void)
{
	void *orig_ioctl = NULL;
	int ret;

	if (!proc_version_orig_compat_ioctl ||
	    proc_version_orig_compat_ioctl == proc_version_orig_fops->unlocked_ioctl)
		return 0;

	proc_version_compat_ioctl_registry = lkmdbg_hook_registry_register(
		"proc_version_compat_ioctl", proc_version_orig_compat_ioctl,
		lkmdbg_proc_version_direct_compat_ioctl);
	if (!proc_version_compat_ioctl_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install((void *)proc_version_orig_compat_ioctl,
				  lkmdbg_proc_version_direct_compat_ioctl,
				  &proc_version_compat_ioctl_hook, &orig_ioctl);
	if (ret) {
		lkmdbg_hook_registry_unregister(proc_version_compat_ioctl_registry,
						ret);
		proc_version_compat_ioctl_registry = NULL;
		return ret;
	}

	proc_version_orig_compat_ioctl = orig_ioctl;
	lkmdbg_hook_registry_mark_installed(proc_version_compat_ioctl_registry,
					    proc_version_orig_fops->compat_ioctl,
					    orig_ioctl, 0);
	return 0;
}
#endif

int lkmdbg_transport_init(void)
{
	struct inode *inode;
	void *orig_open = NULL;
	int ret;

	if (!hook_proc_version)
		return 0;

	atomic_set(&proc_version_open_inflight, 0);
	proc_version_path_valid = false;

	ret = lkmdbg_kern_path_runtime(LKMDBG_TARGET_PATH, LOOKUP_FOLLOW,
				       &proc_version_path);
	if (ret)
		return ret;

	proc_version_path_valid = true;
	inode = d_inode(proc_version_path.dentry);
	if (!inode || !inode->i_fop) {
		lkmdbg_path_put_runtime(&proc_version_path);
		proc_version_path_valid = false;
		return -ENOENT;
	}

	proc_version_inode = inode;
	proc_version_orig_fops = inode->i_fop;
	proc_version_hook_fops = kmalloc(sizeof(*proc_version_hook_fops),
					 GFP_KERNEL);
	if (!proc_version_hook_fops) {
		lkmdbg_path_put_runtime(&proc_version_path);
		proc_version_path_valid = false;
		proc_version_inode = NULL;
		return -ENOMEM;
	}

	memcpy(proc_version_hook_fops, proc_version_orig_fops,
	       sizeof(*proc_version_hook_fops));

	proc_version_orig_open = proc_version_orig_fops->open;
	proc_version_orig_release = proc_version_orig_fops->release;
	proc_version_orig_ioctl = proc_version_orig_fops->unlocked_ioctl;
#ifdef CONFIG_COMPAT
	proc_version_orig_compat_ioctl = proc_version_orig_fops->compat_ioctl;
#endif

	ret = lkmdbg_transport_install_proc_version_ioctl_hook();
	if (ret) {
		kfree(proc_version_hook_fops);
		lkmdbg_path_put_runtime(&proc_version_path);
		lkmdbg_transport_clear_proc_version_state();
		return ret;
	}
#ifdef CONFIG_COMPAT
	ret = lkmdbg_transport_install_proc_version_compat_ioctl_hook();
	if (ret) {
		if (proc_version_ioctl_hook) {
			lkmdbg_hook_destroy(proc_version_ioctl_hook);
			proc_version_ioctl_hook = NULL;
		}
		if (proc_version_ioctl_registry) {
			lkmdbg_hook_registry_unregister(proc_version_ioctl_registry,
						       ret);
			proc_version_ioctl_registry = NULL;
		}
		kfree(proc_version_hook_fops);
		lkmdbg_path_put_runtime(&proc_version_path);
		lkmdbg_transport_clear_proc_version_state();
		return ret;
	}
#endif

	if (!proc_version_orig_open) {
		if (proc_version_compat_ioctl_hook) {
			lkmdbg_hook_destroy(proc_version_compat_ioctl_hook);
			proc_version_compat_ioctl_hook = NULL;
		}
		if (proc_version_compat_ioctl_registry) {
			lkmdbg_hook_registry_unregister(proc_version_compat_ioctl_registry,
						       -EOPNOTSUPP);
			proc_version_compat_ioctl_registry = NULL;
		}
		if (proc_version_ioctl_hook) {
			lkmdbg_hook_destroy(proc_version_ioctl_hook);
			proc_version_ioctl_hook = NULL;
		}
		if (proc_version_ioctl_registry) {
			lkmdbg_hook_registry_unregister(proc_version_ioctl_registry,
						       -EOPNOTSUPP);
			proc_version_ioctl_registry = NULL;
		}
		kfree(proc_version_hook_fops);
		lkmdbg_path_put_runtime(&proc_version_path);
		lkmdbg_transport_clear_proc_version_state();
		return -EOPNOTSUPP;
	}

	proc_version_hook_fops->owner = THIS_MODULE;
	proc_version_hook_fops->open = proc_version_orig_open;
	proc_version_hook_fops->release = lkmdbg_proc_version_release;
	proc_version_hook_fops->unlocked_ioctl = lkmdbg_proc_version_ioctl;
#ifdef CONFIG_COMPAT
	proc_version_hook_fops->compat_ioctl = lkmdbg_proc_version_compat_ioctl;
#endif

	proc_version_open_registry = lkmdbg_hook_registry_register(
		"proc_version_open", proc_version_orig_open, lkmdbg_proc_version_open);
	if (!proc_version_open_registry) {
		if (proc_version_compat_ioctl_hook) {
			lkmdbg_hook_destroy(proc_version_compat_ioctl_hook);
			proc_version_compat_ioctl_hook = NULL;
		}
		if (proc_version_compat_ioctl_registry) {
			lkmdbg_hook_registry_unregister(proc_version_compat_ioctl_registry,
						       -ENOMEM);
			proc_version_compat_ioctl_registry = NULL;
		}
		if (proc_version_ioctl_hook) {
			lkmdbg_hook_destroy(proc_version_ioctl_hook);
			proc_version_ioctl_hook = NULL;
		}
		if (proc_version_ioctl_registry) {
			lkmdbg_hook_registry_unregister(proc_version_ioctl_registry,
						       -ENOMEM);
			proc_version_ioctl_registry = NULL;
		}
		kfree(proc_version_hook_fops);
		lkmdbg_path_put_runtime(&proc_version_path);
		lkmdbg_transport_clear_proc_version_state();
		return -ENOMEM;
	}

	ret = lkmdbg_hook_install((void *)proc_version_orig_open,
				  lkmdbg_proc_version_open,
				  &proc_version_open_hook, &orig_open);
	if (ret) {
		lkmdbg_hook_registry_unregister(proc_version_open_registry, ret);
		proc_version_open_registry = NULL;
		if (proc_version_compat_ioctl_hook) {
			lkmdbg_hook_destroy(proc_version_compat_ioctl_hook);
			proc_version_compat_ioctl_hook = NULL;
		}
		if (proc_version_compat_ioctl_registry) {
			lkmdbg_hook_registry_unregister(proc_version_compat_ioctl_registry,
						       ret);
			proc_version_compat_ioctl_registry = NULL;
		}
		if (proc_version_ioctl_hook) {
			lkmdbg_hook_destroy(proc_version_ioctl_hook);
			proc_version_ioctl_hook = NULL;
		}
		if (proc_version_ioctl_registry) {
			lkmdbg_hook_registry_unregister(proc_version_ioctl_registry,
						       ret);
			proc_version_ioctl_registry = NULL;
		}
		kfree(proc_version_hook_fops);
		lkmdbg_path_put_runtime(&proc_version_path);
		lkmdbg_transport_clear_proc_version_state();
		return ret;
	}

	proc_version_orig_open = orig_open;
	lkmdbg_hook_registry_mark_installed(proc_version_open_registry,
						proc_version_orig_fops->open,
						orig_open, 0);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.proc_version_hook_active = true;
	mutex_unlock(&lkmdbg_state.lock);

	return 0;
}

void lkmdbg_transport_exit(void)
{
	long remaining = 1;

	if (proc_version_compat_ioctl_hook) {
		lkmdbg_hook_destroy(proc_version_compat_ioctl_hook);
		proc_version_compat_ioctl_hook = NULL;
	}
	if (proc_version_compat_ioctl_registry) {
		lkmdbg_hook_registry_unregister(proc_version_compat_ioctl_registry,
					       0);
		proc_version_compat_ioctl_registry = NULL;
	}
	if (proc_version_ioctl_hook) {
		lkmdbg_hook_destroy(proc_version_ioctl_hook);
		proc_version_ioctl_hook = NULL;
	}
	if (proc_version_ioctl_registry) {
		lkmdbg_hook_registry_unregister(proc_version_ioctl_registry, 0);
		proc_version_ioctl_registry = NULL;
	}

	if (proc_version_open_hook) {
		if (!lkmdbg_hook_deactivate(proc_version_open_hook)) {
			remaining = wait_event_timeout(
				proc_version_open_waitq,
				atomic_read(&proc_version_open_inflight) == 0,
				msecs_to_jiffies(1000));
			if (!remaining)
				lkmdbg_pr_warn("lkmdbg: proc_version_open hook drain timed out inflight=%d\n",
					atomic_read(&proc_version_open_inflight));
		}

		lkmdbg_hook_destroy(proc_version_open_hook);
		proc_version_open_hook = NULL;
	}
	if (proc_version_open_registry) {
		lkmdbg_hook_registry_unregister(proc_version_open_registry, 0);
		proc_version_open_registry = NULL;
	}

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.proc_version_hook_active = false;
	mutex_unlock(&lkmdbg_state.lock);

	kfree(proc_version_hook_fops);
	proc_version_hook_fops = NULL;

	if (proc_version_path_valid) {
		lkmdbg_path_put_runtime(&proc_version_path);
	}

	lkmdbg_transport_clear_proc_version_state();
}
