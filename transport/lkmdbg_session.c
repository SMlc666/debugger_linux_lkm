#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

static LIST_HEAD(lkmdbg_session_list);
static DEFINE_MUTEX(lkmdbg_session_list_lock);

static bool lkmdbg_session_has_events(struct lkmdbg_session *session)
{
	return READ_ONCE(session->event_count) > 0;
}

static void lkmdbg_session_queue_event_locked(struct lkmdbg_session *session,
					      u32 type, u64 value0,
					      u64 value1)
{
	struct lkmdbg_event_record *event;
	u32 slot;

	slot = (session->event_head + session->event_count) %
	       LKMDBG_SESSION_EVENT_CAPACITY;
	if (session->event_count == LKMDBG_SESSION_EVENT_CAPACITY) {
		session->event_head =
			(session->event_head + 1) % LKMDBG_SESSION_EVENT_CAPACITY;
		slot = (session->event_head + session->event_count - 1) %
		       LKMDBG_SESSION_EVENT_CAPACITY;
	} else {
		session->event_count++;
	}

	session->event_seq++;
	event = &session->events[slot];
	memset(event, 0, sizeof(*event));
	event->version = LKMDBG_EVENT_VERSION;
	event->type = type;
	event->size = sizeof(*event);
	event->session_id = session->session_id;
	event->seq = session->event_seq;
	event->value0 = value0;
	event->value1 = value1;
}

static void lkmdbg_session_queue_event(struct lkmdbg_session *session, u32 type,
				       u64 value0, u64 value1)
{
	mutex_lock(&session->lock);
	lkmdbg_session_queue_event_locked(session, type, value0, value1);

	mutex_unlock(&session->lock);
	wake_up_interruptible(&session->readq);
}

void lkmdbg_session_broadcast_event(u32 type, u64 value0, u64 value1)
{
	struct lkmdbg_session *session;

	mutex_lock(&lkmdbg_session_list_lock);
	list_for_each_entry(session, &lkmdbg_session_list, node) {
		mutex_lock(&session->lock);
		lkmdbg_session_queue_event_locked(session, type, value0, value1);
		mutex_unlock(&session->lock);
		wake_up_interruptible(&session->readq);
	}
	mutex_unlock(&lkmdbg_session_list_lock);
}

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
	reply.target_tgid = session->target_tgid;
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

	(void)inode;

	mutex_lock(&lkmdbg_state.lock);
	if (lkmdbg_state.active_sessions > 0)
		lkmdbg_state.active_sessions--;
	mutex_unlock(&lkmdbg_state.lock);

	mutex_lock(&lkmdbg_session_list_lock);
	if (!list_empty(&session->node))
		list_del_init(&session->node);
	mutex_unlock(&lkmdbg_session_list_lock);

	kfree(session);
	return 0;
}

ssize_t lkmdbg_session_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct lkmdbg_session *session = file->private_data;
	struct lkmdbg_event_record event;
	int ret;

	(void)ppos;

	if (!session)
		return -ENXIO;

	if (count < sizeof(event))
		return -EINVAL;

	for (;;) {
		mutex_lock(&session->lock);
		if (session->event_count > 0)
			break;
		mutex_unlock(&session->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(session->readq,
					       lkmdbg_session_has_events(session));
		if (ret)
			return ret;
	}

	event = session->events[session->event_head];
	session->event_head =
		(session->event_head + 1) % LKMDBG_SESSION_EVENT_CAPACITY;
	session->event_count--;
	mutex_unlock(&session->lock);

	if (copy_to_user(buf, &event, sizeof(event)))
		return -EFAULT;

	return sizeof(event);
}

__poll_t lkmdbg_session_poll(struct file *file, poll_table *wait)
{
	struct lkmdbg_session *session = file->private_data;
	__poll_t mask = 0;

	if (!session)
		return EPOLLERR;

	poll_wait(file, &session->readq, wait);

	mutex_lock(&session->lock);
	if (session->event_count > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&session->lock);

	return mask;
}

long lkmdbg_session_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct lkmdbg_session *session = file->private_data;
	void __user *argp = (void __user *)arg;
	pid_t owner;
	u64 old_calls;

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
		old_calls = session->ioctl_calls;
		session->ioctl_calls = 0;
		mutex_unlock(&session->lock);
		lkmdbg_session_queue_event(session, LKMDBG_EVENT_SESSION_RESET,
					   old_calls, current->tgid);
		return 0;
	case LKMDBG_IOC_SET_TARGET:
		return lkmdbg_mem_set_target(session, argp);
	case LKMDBG_IOC_READ_MEM:
		return lkmdbg_mem_read(session, argp);
	case LKMDBG_IOC_WRITE_MEM:
		return lkmdbg_mem_write(session, argp);
	case LKMDBG_IOC_QUERY_VMAS:
		return lkmdbg_vma_query(session, argp);
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
	.read = lkmdbg_session_read,
	.poll = lkmdbg_session_poll,
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
	init_waitqueue_head(&session->readq);
	INIT_LIST_HEAD(&session->node);
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

	mutex_lock(&lkmdbg_session_list_lock);
	list_add_tail(&session->node, &lkmdbg_session_list);
	mutex_unlock(&lkmdbg_session_list_lock);

	lkmdbg_session_queue_event(session, LKMDBG_EVENT_SESSION_OPENED,
				   current->tgid, 0);

	return fd;
}
