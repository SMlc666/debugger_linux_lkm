#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

static LIST_HEAD(lkmdbg_session_list);
static DEFINE_SPINLOCK(lkmdbg_session_list_lock);
#define LKMDBG_SESSION_READ_BATCH 16U
static void lkmdbg_session_stop_workfn(struct work_struct *work);

static void lkmdbg_session_zero_stop(struct lkmdbg_stop_state *stop)
{
	memset(stop, 0, sizeof(*stop));
}

static bool lkmdbg_session_has_events(struct lkmdbg_session *session)
{
	return READ_ONCE(session->event_count) > 0;
}

static void
lkmdbg_session_note_read_event(const struct lkmdbg_event_record *event)
{
	if (event->type != LKMDBG_EVENT_TARGET_STOP)
		return;

	atomic64_inc(&lkmdbg_state.target_stop_event_read_total);
	if (event->code == LKMDBG_STOP_REASON_BREAKPOINT)
		atomic64_inc(&lkmdbg_state.breakpoint_stop_event_read_total);
	else if (event->code == LKMDBG_STOP_REASON_WATCHPOINT)
		atomic64_inc(&lkmdbg_state.watchpoint_stop_event_read_total);
}

static void lkmdbg_session_queue_event_locked(struct lkmdbg_session *session,
					      u32 type, u32 code, pid_t tgid,
					      pid_t tid, u32 flags,
					      u64 value0, u64 value1)
{
	struct lkmdbg_event_record *event;
	u32 slot;

	slot = (session->event_head + session->event_count) %
	       LKMDBG_SESSION_EVENT_CAPACITY;
	if (session->event_count == LKMDBG_SESSION_EVENT_CAPACITY) {
		session->event_drop_count++;
		session->event_drop_pending++;
		atomic64_inc(&lkmdbg_state.event_drop_total);
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
	event->code = code;
	event->session_id = session->session_id;
	event->seq = session->event_seq;
	event->tgid = tgid;
	event->tid = tid;
	event->flags = flags;
	event->reserved0 = session->event_drop_pending;
	event->value0 = value0;
	event->value1 = value1;
	session->event_drop_pending = 0;
}

void lkmdbg_session_queue_event_ex(struct lkmdbg_session *session, u32 type,
				   u32 code, pid_t tgid, pid_t tid, u32 flags,
				   u64 value0, u64 value1)
{
	unsigned long irqflags;

	spin_lock_irqsave(&session->event_lock, irqflags);
	lkmdbg_session_queue_event_locked(session, type, code, tgid, tid, flags,
					 value0, value1);
	spin_unlock_irqrestore(&session->event_lock, irqflags);
	wake_up_interruptible(&session->readq);
}

struct lkmdbg_session *lkmdbg_session_consume_single_step(pid_t tgid,
							  pid_t tid)
{
	struct lkmdbg_session *session;
	struct lkmdbg_session *matched = NULL;
	unsigned long irqflags;

	spin_lock_irqsave(&lkmdbg_session_list_lock, irqflags);
	list_for_each_entry(session, &lkmdbg_session_list, node) {
		if (!READ_ONCE(session->step_armed))
			continue;
		if (READ_ONCE(session->step_tgid) != tgid ||
		    READ_ONCE(session->step_tid) != tid)
			continue;

		WRITE_ONCE(session->step_armed, false);
		WRITE_ONCE(session->step_tgid, 0);
		WRITE_ONCE(session->step_tid, 0);
		atomic_inc(&session->async_refs);
		matched = session;
		break;
	}
	spin_unlock_irqrestore(&lkmdbg_session_list_lock, irqflags);

	return matched;
}

void lkmdbg_session_async_put(struct lkmdbg_session *session)
{
	if (!session)
		return;

	if (atomic_dec_and_test(&session->async_refs))
		wake_up_all(&session->async_waitq);
}

void lkmdbg_session_broadcast_event(u32 type, u64 value0, u64 value1)
{
	struct lkmdbg_session *session;
	unsigned long irqflags;

	spin_lock_irqsave(&lkmdbg_session_list_lock, irqflags);
	list_for_each_entry(session, &lkmdbg_session_list, node) {
		unsigned long session_irqflags;

		spin_lock_irqsave(&session->event_lock, session_irqflags);
		lkmdbg_session_queue_event_locked(session, type, 0, 0, 0, 0,
						 value0, value1);
		spin_unlock_irqrestore(&session->event_lock, session_irqflags);
		wake_up_interruptible(&session->readq);
	}
	spin_unlock_irqrestore(&lkmdbg_session_list_lock, irqflags);
}

void lkmdbg_session_commit_stop(struct lkmdbg_session *session, u32 reason,
				pid_t tgid, pid_t tid, u32 event_flags,
				u32 stop_flags, u64 value0, u64 value1,
				const struct lkmdbg_regs_arm64 *regs)
{
	struct lkmdbg_stop_state stop;

	lkmdbg_session_zero_stop(&stop);
	stop.reason = reason;
	stop.flags = stop_flags | LKMDBG_STOP_FLAG_ACTIVE;
	stop.tgid = tgid;
	stop.tid = tid;
	stop.event_flags = event_flags;
	stop.value0 = value0;
	stop.value1 = value1;
	if (regs) {
		stop.regs = *regs;
		stop.flags |= LKMDBG_STOP_FLAG_REGS_VALID;
	}

	mutex_lock(&session->lock);
	session->next_stop_cookie++;
	stop.cookie = session->next_stop_cookie;
	session->stop_state = stop;
	mutex_unlock(&session->lock);
}

void lkmdbg_session_clear_stop(struct lkmdbg_session *session)
{
	mutex_lock(&session->lock);
	lkmdbg_session_zero_stop(&session->stop_state);
	mutex_unlock(&session->lock);
}

static void lkmdbg_session_stop_workfn(struct work_struct *work)
{
	struct lkmdbg_session *session =
		container_of(work, struct lkmdbg_session, stop_work);
	struct lkmdbg_stop_state stop;
	struct lkmdbg_freeze_request freeze_req;
	u64 target_gen;
	bool closing;
	int ret;

	lkmdbg_session_zero_stop(&stop);
	memset(&freeze_req, 0, sizeof(freeze_req));

	mutex_lock(&session->lock);
	stop = session->pending_stop;
	target_gen = session->stop_work_target_gen;
	closing = session->closing;
	lkmdbg_session_zero_stop(&session->pending_stop);
	mutex_unlock(&session->lock);

	if (closing || !stop.reason)
		goto out;

	mutex_lock(&session->lock);
	if (session->target_gen != target_gen || session->target_tgid != stop.tgid) {
		mutex_unlock(&session->lock);
		goto out;
	}
	mutex_unlock(&session->lock);

	ret = lkmdbg_session_freeze_target(session, 1000, &freeze_req);
	if (ret) {
		pr_warn("lkmdbg: async stop freeze failed tgid=%d tid=%d reason=%u ret=%d\n",
			stop.tgid, stop.tid, stop.reason, ret);
		goto out;
	}

	lkmdbg_session_commit_stop(session, stop.reason, stop.tgid, stop.tid,
				   stop.event_flags,
				   stop.flags | LKMDBG_STOP_FLAG_FROZEN,
				   stop.value0, stop.value1,
				   (stop.flags & LKMDBG_STOP_FLAG_REGS_VALID) ?
					   &stop.regs :
					   NULL);
	lkmdbg_session_queue_event_ex(session, LKMDBG_EVENT_TARGET_STOP,
				      stop.reason, stop.tgid, stop.tid,
				      stop.event_flags, stop.value0,
				      stop.value1);

out:
	mutex_lock(&session->lock);
	session->stop_work_pending = false;
	mutex_unlock(&session->lock);
	lkmdbg_session_async_put(session);
}

void lkmdbg_session_request_async_stop(struct lkmdbg_session *session,
				       u32 reason, pid_t tgid, pid_t tid,
				       u32 event_flags, u32 stop_flags,
				       u64 value0, u64 value1,
				       const struct lkmdbg_regs_arm64 *regs)
{
	bool scheduled = false;

	if (!session || tgid <= 0)
		return;

	mutex_lock(&session->lock);
	if (!session->closing && session->target_tgid == tgid &&
	    !(session->stop_state.flags & LKMDBG_STOP_FLAG_ACTIVE) &&
	    !session->stop_work_pending && !session->freezer) {
		lkmdbg_session_zero_stop(&session->pending_stop);
		session->pending_stop.reason = reason;
		session->pending_stop.flags =
			stop_flags | LKMDBG_STOP_FLAG_ASYNC;
		session->pending_stop.tgid = tgid;
		session->pending_stop.tid = tid;
		session->pending_stop.event_flags = event_flags;
		session->pending_stop.value0 = value0;
		session->pending_stop.value1 = value1;
		if (regs) {
			session->pending_stop.regs = *regs;
			session->pending_stop.flags |= LKMDBG_STOP_FLAG_REGS_VALID;
		}
		session->stop_work_pending = true;
		session->stop_work_target_gen = session->target_gen;
		atomic_inc(&session->async_refs);
		scheduled = true;
	}
	mutex_unlock(&session->lock);

	if (scheduled)
		schedule_work(&session->stop_work);
}

void lkmdbg_session_broadcast_target_event(pid_t target_tgid, u32 type,
					   u32 code, pid_t tid, u32 flags,
					   u64 value0, u64 value1)
{
	struct lkmdbg_session *session;
	unsigned long irqflags;

	if (target_tgid <= 0)
		return;

	spin_lock_irqsave(&lkmdbg_session_list_lock, irqflags);
	list_for_each_entry(session, &lkmdbg_session_list, node) {
		unsigned long session_irqflags;
		pid_t session_tgid;

		session_tgid = READ_ONCE(session->target_tgid);
		if (session_tgid != target_tgid)
			continue;

		spin_lock_irqsave(&session->event_lock, session_irqflags);
		lkmdbg_session_queue_event_locked(session, type, code, target_tgid,
						 tid, flags, value0, value1);
		spin_unlock_irqrestore(&session->event_lock, session_irqflags);
		wake_up_interruptible(&session->readq);
	}
	spin_unlock_irqrestore(&lkmdbg_session_list_lock, irqflags);
}

static int lkmdbg_session_copy_status_to_user(struct lkmdbg_session *session,
					      void __user *argp)
{
	struct lkmdbg_status_reply reply;
	struct lkmdbg_stop_state stop;
	unsigned long irqflags;

	memset(&reply, 0, sizeof(reply));
	lkmdbg_session_zero_stop(&stop);

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
	reply.target_tid = session->target_tid;
	reply.session_id = session->session_id;
	reply.session_ioctl_calls = session->ioctl_calls;
	stop = session->stop_state;
	mutex_unlock(&session->lock);

	spin_lock_irqsave(&session->event_lock, irqflags);
	reply.event_queue_depth = session->event_count;
	reply.session_event_drops = session->event_drop_count;
	spin_unlock_irqrestore(&session->event_lock, irqflags);

	reply.total_event_drops = atomic64_read(&lkmdbg_state.event_drop_total);
	reply.stop_cookie = stop.cookie;
	reply.stop_reason = stop.reason;
	reply.stop_flags = stop.flags;
	reply.stop_tgid = stop.tgid;
	reply.stop_tid = stop.tid;

	if (copy_to_user(argp, &reply, sizeof(reply)))
		return -EFAULT;

	return 0;
}

static int lkmdbg_validate_stop_query(struct lkmdbg_stop_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	return 0;
}

long lkmdbg_get_stop_state(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_stop_query_request req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (lkmdbg_validate_stop_query(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	req.stop = session->stop_state;
	mutex_unlock(&session->lock);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static int lkmdbg_validate_continue_request(struct lkmdbg_continue_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (req->flags & ~LKMDBG_CONTINUE_FLAG_REARM_HWPOINTS)
		return -EINVAL;
	return 0;
}

long lkmdbg_continue_target(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_continue_request req;
	struct lkmdbg_stop_state stop;
	struct lkmdbg_freeze_request thaw_req;
	long ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_continue_request(&req);
	if (ret)
		return ret;

	lkmdbg_session_zero_stop(&stop);
	memset(&thaw_req, 0, sizeof(thaw_req));

	mutex_lock(&session->lock);
	stop = session->stop_state;
	mutex_unlock(&session->lock);

	if ((stop.flags & LKMDBG_STOP_FLAG_ACTIVE) && req.stop_cookie &&
	    req.stop_cookie != stop.cookie)
		return -ESTALE;

	ret = lkmdbg_prepare_continue_hwpoints(session, &stop, req.flags);
	if (ret)
		return ret;

	lkmdbg_session_clear_stop(session);
	ret = lkmdbg_session_thaw_target(session, req.timeout_ms, &thaw_req);
	if (ret && ret != -ENODEV) {
		if (stop.flags & LKMDBG_STOP_FLAG_ACTIVE) {
			mutex_lock(&session->lock);
			session->stop_state = stop;
			mutex_unlock(&session->lock);
		}
		return ret;
	}

	req.threads_total = thaw_req.threads_total;
	req.threads_settled = thaw_req.threads_settled;
	req.threads_parked = thaw_req.threads_parked;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static int lkmdbg_session_release(struct inode *inode, struct file *file)
{
	struct lkmdbg_session *session = file->private_data;

	(void)inode;

	spin_lock(&lkmdbg_session_list_lock);
	if (!list_empty(&session->node))
		list_del_init(&session->node);
	spin_unlock(&lkmdbg_session_list_lock);

	mutex_lock(&session->lock);
	session->closing = true;
	mutex_unlock(&session->lock);
	flush_work(&session->stop_work);

	lkmdbg_thread_ctrl_release(session);
	lkmdbg_session_freeze_release(session);
	wait_event(session->async_waitq, atomic_read(&session->async_refs) == 0);

	mutex_lock(&lkmdbg_state.lock);
	if (lkmdbg_state.active_sessions > 0)
		lkmdbg_state.active_sessions--;
	mutex_unlock(&lkmdbg_state.lock);

	kfree(session);
	return 0;
}

ssize_t lkmdbg_session_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct lkmdbg_session *session = file->private_data;
	struct lkmdbg_event_record events[LKMDBG_SESSION_READ_BATCH];
	size_t max_events;
	size_t copied = 0;
	size_t bytes;
	int ret;
	u32 i;

	(void)ppos;

	if (!session)
		return -ENXIO;

	max_events = count / sizeof(events[0]);
	if (!max_events)
		return -EINVAL;
	if (max_events > LKMDBG_SESSION_READ_BATCH)
		max_events = LKMDBG_SESSION_READ_BATCH;

	for (;;) {
		spin_lock_irq(&session->event_lock);
		if (session->event_count > 0)
			break;
		spin_unlock_irq(&session->event_lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(session->readq,
					       lkmdbg_session_has_events(session));
		if (ret)
			return ret;
	}

	while (copied < max_events && session->event_count > 0) {
		events[copied] = session->events[session->event_head];
		session->event_head =
			(session->event_head + 1) % LKMDBG_SESSION_EVENT_CAPACITY;
		session->event_count--;
		copied++;
	}
	spin_unlock_irq(&session->event_lock);

	bytes = copied * sizeof(events[0]);
	if (copy_to_user(buf, events, bytes))
		return -EFAULT;

	for (i = 0; i < copied; i++)
		lkmdbg_session_note_read_event(&events[i]);

	return bytes;
}

__poll_t lkmdbg_session_poll(struct file *file, poll_table *wait)
{
	struct lkmdbg_session *session = file->private_data;
	__poll_t mask = 0;

	if (!session)
		return EPOLLERR;

	poll_wait(file, &session->readq, wait);

	spin_lock_irq(&session->event_lock);
	if (session->event_count > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irq(&session->event_lock);

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
		lkmdbg_session_queue_event_ex(session, LKMDBG_EVENT_SESSION_RESET,
					      0, 0, 0, 0, old_calls,
					      current->tgid);
		return 0;
	case LKMDBG_IOC_SET_TARGET:
		if (lkmdbg_session_freeze_on_target_change(session))
			return -EBUSY;
		return lkmdbg_mem_set_target(session, argp);
	case LKMDBG_IOC_READ_MEM:
		return lkmdbg_mem_read(session, argp);
	case LKMDBG_IOC_WRITE_MEM:
		return lkmdbg_mem_write(session, argp);
	case LKMDBG_IOC_QUERY_PAGES:
		return lkmdbg_page_query(session, argp);
	case LKMDBG_IOC_QUERY_VMAS:
		return lkmdbg_vma_query(session, argp);
	case LKMDBG_IOC_QUERY_IMAGES:
		return lkmdbg_image_query(session, argp);
	case LKMDBG_IOC_CREATE_REMOTE_MAP:
		return lkmdbg_create_remote_map(session, argp);
	case LKMDBG_IOC_QUERY_THREADS:
		return lkmdbg_query_threads(session, argp);
	case LKMDBG_IOC_GET_REGS:
		return lkmdbg_get_regs(session, argp);
	case LKMDBG_IOC_SET_REGS:
		return lkmdbg_set_regs(session, argp);
	case LKMDBG_IOC_GET_STOP_STATE:
		return lkmdbg_get_stop_state(session, argp);
	case LKMDBG_IOC_CONTINUE_TARGET:
		return lkmdbg_continue_target(session, argp);
	case LKMDBG_IOC_ADD_HWPOINT:
		return lkmdbg_add_hwpoint(session, argp);
	case LKMDBG_IOC_REMOVE_HWPOINT:
		return lkmdbg_remove_hwpoint(session, argp);
	case LKMDBG_IOC_QUERY_HWPOINTS:
		return lkmdbg_query_hwpoints(session, argp);
	case LKMDBG_IOC_REARM_HWPOINT:
		return lkmdbg_rearm_hwpoint(session, argp);
	case LKMDBG_IOC_SINGLE_STEP:
		return lkmdbg_single_step(session, argp);
	case LKMDBG_IOC_FREEZE_THREADS:
		return lkmdbg_freeze_threads(session, argp);
	case LKMDBG_IOC_THAW_THREADS:
		return lkmdbg_thaw_threads(session, argp);
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
	spin_lock_init(&session->event_lock);
	init_waitqueue_head(&session->readq);
	init_waitqueue_head(&session->async_waitq);
	INIT_LIST_HEAD(&session->node);
	INIT_LIST_HEAD(&session->hwpoints);
	INIT_WORK(&session->stop_work, lkmdbg_session_stop_workfn);
	atomic_set(&session->async_refs, 0);
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

	spin_lock(&lkmdbg_session_list_lock);
	list_add_tail(&session->node, &lkmdbg_session_list);
	spin_unlock(&lkmdbg_session_list_lock);

	lkmdbg_session_queue_event_ex(session, LKMDBG_EVENT_SESSION_OPENED, 0, 0,
				      0, 0, current->tgid, 0);

	return fd;
}
