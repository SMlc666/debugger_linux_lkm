#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

static int lkmdbg_session_copy_status_to_user(struct lkmdbg_session *session,
					      void __user *argp)
{
	struct lkmdbg_status_reply reply;

	memset(&reply, 0, sizeof(reply));

	mutex_lock(&lkmdbg_state.lock);
	reply.version = LKMDBG_PROTO_VERSION;
	reply.size = sizeof(reply);
	reply.hook_requested = hook_proc_version;
	reply.hook_active = lkmdbg_state.proc_version_hook_active;
	reply.active_sessions = lkmdbg_state.active_sessions;
	reply.load_jiffies = lkmdbg_state.load_jiffies;
	reply.status_reads = lkmdbg_state.status_reads;
	reply.bootstrap_ioctl_calls = lkmdbg_state.bootstrap_ioctl_calls;
	reply.session_opened_total = lkmdbg_state.session_opened_total;
	reply.open_successes = lkmdbg_state.proc_open_successes;
	mutex_unlock(&lkmdbg_state.lock);

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

	mutex_lock(&lkmdbg_state.lock);
	if (lkmdbg_state.active_sessions > 0)
		lkmdbg_state.active_sessions--;
	mutex_unlock(&lkmdbg_state.lock);

	kfree(session);
	return 0;
}

long lkmdbg_session_ioctl(struct file *file, unsigned int cmd,
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
long lkmdbg_session_compat_ioctl(struct file *file, unsigned int cmd,
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

int lkmdbg_open_session(void __user *argp)
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

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.next_session_id++;
	session->session_id = lkmdbg_state.next_session_id;
	mutex_unlock(&lkmdbg_state.lock);

	fd = anon_inode_getfd("lkmdbg", &lkmdbg_session_fops, session,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		kfree(session);
		return fd;
	}

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.session_opened_total++;
	lkmdbg_state.active_sessions++;
	mutex_unlock(&lkmdbg_state.lock);

	return fd;
}
