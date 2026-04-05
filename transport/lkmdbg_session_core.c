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

struct list_head lkmdbg_session_list = LIST_HEAD_INIT(lkmdbg_session_list);
DEFINE_SPINLOCK(lkmdbg_session_list_lock);

#define LKMDBG_SESSION_READ_BATCH 16U

static bool lkmdbg_event_mask_index(u32 type, u32 *word_index_out,
				    u32 *bit_index_out)
{
	u32 bit_index;
	u32 word_index;

	if (!type)
		return false;

	bit_index = type - 1U;
	word_index = bit_index / 64U;
	if (word_index >= LKMDBG_EVENT_MASK_WORDS)
		return false;

	if (word_index_out)
		*word_index_out = word_index;
	if (bit_index_out)
		*bit_index_out = bit_index % 64U;
	return true;
}

static void lkmdbg_event_mask_set(u64 *mask_words, u32 type)
{
	u32 word_index;
	u32 bit_index;

	if (!lkmdbg_event_mask_index(type, &word_index, &bit_index))
		return;

	mask_words[word_index] |= 1ULL << bit_index;
}

static bool lkmdbg_event_mask_test(const u64 *mask_words, u32 type)
{
	u32 word_index;
	u32 bit_index;

	if (!lkmdbg_event_mask_index(type, &word_index, &bit_index))
		return false;

	return !!(mask_words[word_index] & (1ULL << bit_index));
}

static bool lkmdbg_event_mask_supported(const u64 *mask_words,
					const u64 *supported_mask_words)
{
	u32 i;

	for (i = 0; i < LKMDBG_EVENT_MASK_WORDS; i++) {
		if (mask_words[i] & ~supported_mask_words[i])
			return false;
	}

	return true;
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

bool lkmdbg_session_owner_active(pid_t owner_tgid)
{
	struct lkmdbg_session *session;
	bool active = false;

	if (owner_tgid <= 0)
		return false;

	spin_lock(&lkmdbg_session_list_lock);
	list_for_each_entry(session, &lkmdbg_session_list, node) {
		if (READ_ONCE(session->owner_tgid) == owner_tgid) {
			active = true;
			break;
		}
	}
	spin_unlock(&lkmdbg_session_list_lock);
	return active;
}

void lkmdbg_session_supported_event_mask(u64 *mask_words)
{
	memset(mask_words, 0, sizeof(u64) * LKMDBG_EVENT_MASK_WORDS);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_SESSION_OPENED);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_SESSION_RESET);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_INTERNAL_NOTICE);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_HOOK_INSTALLED);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_HOOK_REMOVED);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_HOOK_HIT);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_CLONE);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_EXEC);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_EXIT);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_SIGNAL);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_STOP);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_SYSCALL);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_MMAP);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_MUNMAP);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_MPROTECT);
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_SYSCALL_RULE);
	lkmdbg_event_mask_set(mask_words,
			      LKMDBG_EVENT_TARGET_SYSCALL_RULE_DETAIL);
}

bool lkmdbg_session_event_type_enabled(const struct lkmdbg_session *session,
				       u32 type)
{
	u64 mask_words[LKMDBG_EVENT_MASK_WORDS];
	u32 i;

	for (i = 0; i < LKMDBG_EVENT_MASK_WORDS; i++)
		mask_words[i] = READ_ONCE(session->event_mask_words[i]);

	return lkmdbg_event_mask_test(mask_words, type);
}

void lkmdbg_session_zero_stop(struct lkmdbg_stop_state *stop)
{
	memset(stop, 0, sizeof(*stop));
}

void lkmdbg_session_reset_syscall_control_locked(struct lkmdbg_session *session)
{
	memset(&session->syscall_control, 0, sizeof(session->syscall_control));
	init_waitqueue_head(&session->syscall_control.waitq);
}

int lkmdbg_session_prepare_continue_syscall_control(
	struct lkmdbg_session *session, const struct lkmdbg_stop_state *stop)
{
	int ret = 0;

	mutex_lock(&session->lock);
	if (session->syscall_control.active) {
		if (!stop || stop->reason != LKMDBG_STOP_REASON_SYSCALL ||
		    !(stop->flags & LKMDBG_STOP_FLAG_ACTIVE) ||
		    !(stop->flags & LKMDBG_STOP_FLAG_FROZEN) ||
		    !(stop->flags & LKMDBG_STOP_FLAG_SYSCALL_CONTROL)) {
			ret = -EBUSY;
		} else if (!session->syscall_control.resolved) {
			ret = -EBUSY;
		}
	}
	mutex_unlock(&session->lock);

	return ret;
}

void lkmdbg_session_finish_continue_syscall_control(
	struct lkmdbg_session *session, const struct lkmdbg_stop_state *stop)
{
	bool wake = false;

	mutex_lock(&session->lock);
	if (session->syscall_control.active && stop &&
	    stop->reason == LKMDBG_STOP_REASON_SYSCALL &&
	    (stop->flags & LKMDBG_STOP_FLAG_SYSCALL_CONTROL)) {
		session->syscall_control.resume = true;
		wake = true;
	}
	mutex_unlock(&session->lock);

	if (wake)
		wake_up_all(&session->syscall_control.waitq);
}

void lkmdbg_session_fail_syscall_control(struct lkmdbg_session *session)
{
	bool wake = false;

	mutex_lock(&session->lock);
	if (session->syscall_control.active) {
		session->syscall_control.abort = true;
		session->syscall_control.resume = true;
		wake = true;
	}
	mutex_unlock(&session->lock);

	if (wake)
		wake_up_all(&session->syscall_control.waitq);
}

bool lkmdbg_session_syscall_control_matches(
	const struct lkmdbg_session *session, pid_t tid, s32 syscall_nr)
{
	u32 phases;
	u32 mode;
	s32 filter_tid;
	s32 filter_nr;

	mode = READ_ONCE(session->syscall_trace_mode);
	if (!(mode & LKMDBG_SYSCALL_TRACE_MODE_CONTROL))
		return false;

	phases = READ_ONCE(session->syscall_trace_phases);
	if (!(phases & LKMDBG_SYSCALL_TRACE_PHASE_ENTER))
		return false;

	filter_tid = READ_ONCE(session->syscall_trace_tid);
	if (filter_tid > 0 && filter_tid != tid)
		return false;

	filter_nr = READ_ONCE(session->syscall_trace_nr);
	if (filter_nr >= 0 && filter_nr != syscall_nr)
		return false;

	return READ_ONCE(session->syscall_trace_hook_fallback);
}

void lkmdbg_session_queue_event_locked(struct lkmdbg_session *session, u32 type,
				       u32 code, pid_t tgid, pid_t tid,
				       u32 flags, u64 value0, u64 value1)
{
	struct lkmdbg_event_record *event;
	u32 slot;

	if (!lkmdbg_session_event_type_enabled(session, type))
		return;

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

long lkmdbg_set_event_config(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_event_config_request req;
	u64 supported_mask_words[LKMDBG_EVENT_MASK_WORDS];
	u32 i;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req))
		return -EINVAL;
	if (req.flags || req.reserved0)
		return -EINVAL;

	lkmdbg_session_supported_event_mask(supported_mask_words);
	if (!lkmdbg_event_mask_supported(req.mask_words, supported_mask_words))
		return -EINVAL;

	mutex_lock(&session->lock);
	for (i = 0; i < LKMDBG_EVENT_MASK_WORDS; i++)
		WRITE_ONCE(session->event_mask_words[i], req.mask_words[i]);
	mutex_unlock(&session->lock);

	for (i = 0; i < LKMDBG_EVENT_MASK_WORDS; i++)
		req.supported_mask_words[i] = supported_mask_words[i];

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_get_event_config(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_event_config_request req;
	u32 i;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req))
		return -EINVAL;
	if (req.flags || req.reserved0)
		return -EINVAL;

	mutex_lock(&session->lock);
	for (i = 0; i < LKMDBG_EVENT_MASK_WORDS; i++)
		req.mask_words[i] = READ_ONCE(session->event_mask_words[i]);
	mutex_unlock(&session->lock);

	req.flags = 0;
	req.reserved0 = 0;
	lkmdbg_session_supported_event_mask(req.supported_mask_words);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
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
	reply.stealth_flags = lkmdbg_stealth_current_flags();
	reply.stealth_supported_flags = lkmdbg_stealth_supported_flags();

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

static int lkmdbg_session_release(struct inode *inode, struct file *file)
{
	struct lkmdbg_session *session = file->private_data;
	pid_t owner_tgid;

	(void)inode;

	spin_lock(&lkmdbg_session_list_lock);
	if (!list_empty(&session->node))
		list_del_init(&session->node);
	spin_unlock(&lkmdbg_session_list_lock);

	mutex_lock(&session->lock);
	session->closing = true;
	mutex_unlock(&session->lock);
	lkmdbg_session_fail_syscall_control(session);
	flush_work(&session->stop_work);

	lkmdbg_pte_patch_release(session);
	lkmdbg_remote_map_release_session(session);
	lkmdbg_remote_alloc_release_session(session);
	lkmdbg_input_release_session(session);
	lkmdbg_remote_call_release(session);
	lkmdbg_thread_ctrl_release(session);
	lkmdbg_session_freeze_release(session);
	mutex_lock(&session->lock);
	lkmdbg_release_syscall_rules_locked(session);
	mutex_unlock(&session->lock);
	wait_event(session->async_waitq, atomic_read(&session->async_refs) == 0);
	owner_tgid = session->owner_tgid;
	if (!lkmdbg_session_owner_active(owner_tgid))
		lkmdbg_stealth_session_release(owner_tgid);

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
		mutex_lock(&session->lock);
		old_calls = !!(session->stop_state.flags &
			       LKMDBG_STOP_FLAG_ACTIVE) ||
			    session->syscall_control.active;
		mutex_unlock(&session->lock);
			if (old_calls)
				return -EBUSY;
			if (lkmdbg_remote_call_blocks_target_change(session))
				return -EBUSY;
			if (lkmdbg_view_region_blocks_target_change(session))
				return -EBUSY;
			if (lkmdbg_session_freeze_on_target_change(session))
				return -EBUSY;
			return lkmdbg_mem_set_target(session, argp);
	case LKMDBG_IOC_READ_MEM:
		return lkmdbg_mem_read(session, argp);
	case LKMDBG_IOC_WRITE_MEM:
		return lkmdbg_mem_write(session, argp);
	case LKMDBG_IOC_READ_PHYS:
		return lkmdbg_phys_read(session, argp);
	case LKMDBG_IOC_WRITE_PHYS:
		return lkmdbg_phys_write(session, argp);
	case LKMDBG_IOC_QUERY_PAGES:
		return lkmdbg_page_query(session, argp);
	case LKMDBG_IOC_QUERY_VMAS:
		return lkmdbg_vma_query(session, argp);
	case LKMDBG_IOC_QUERY_IMAGES:
		return lkmdbg_image_query(session, argp);
	case LKMDBG_IOC_CREATE_REMOTE_MAP:
		return lkmdbg_create_remote_map(session, argp);
	case LKMDBG_IOC_REMOVE_REMOTE_MAP:
		return lkmdbg_remove_remote_map(session, argp);
	case LKMDBG_IOC_QUERY_REMOTE_MAPS:
		return lkmdbg_query_remote_maps(session, argp);
	case LKMDBG_IOC_CREATE_REMOTE_ALLOC:
		return lkmdbg_create_remote_alloc(session, argp);
	case LKMDBG_IOC_REMOVE_REMOTE_ALLOC:
		return lkmdbg_remove_remote_alloc(session, argp);
	case LKMDBG_IOC_QUERY_REMOTE_ALLOCS:
		return lkmdbg_query_remote_allocs(session, argp);
	case LKMDBG_IOC_APPLY_PTE_PATCH:
		return lkmdbg_apply_pte_patch(session, argp);
	case LKMDBG_IOC_REMOVE_PTE_PATCH:
		return lkmdbg_remove_pte_patch(session, argp);
	case LKMDBG_IOC_QUERY_PTE_PATCHES:
		return lkmdbg_query_pte_patches(session, argp);
	case LKMDBG_IOC_QUERY_THREADS:
		return lkmdbg_query_threads(session, argp);
	case LKMDBG_IOC_GET_REGS:
		return lkmdbg_get_regs(session, argp);
	case LKMDBG_IOC_SET_REGS:
		return lkmdbg_set_regs(session, argp);
	case LKMDBG_IOC_SET_SIGNAL_CONFIG:
		return lkmdbg_set_signal_config(session, argp);
	case LKMDBG_IOC_GET_SIGNAL_CONFIG:
		return lkmdbg_get_signal_config(session, argp);
	case LKMDBG_IOC_SET_EVENT_CONFIG:
		return lkmdbg_set_event_config(session, argp);
	case LKMDBG_IOC_GET_EVENT_CONFIG:
		return lkmdbg_get_event_config(session, argp);
	case LKMDBG_IOC_SET_SYSCALL_TRACE:
		return lkmdbg_set_syscall_trace(session, argp);
	case LKMDBG_IOC_GET_SYSCALL_TRACE:
		return lkmdbg_get_syscall_trace(session, argp);
	case LKMDBG_IOC_SET_SYSCALL_RULE_CONFIG:
		return lkmdbg_set_syscall_rule_config(session, argp);
	case LKMDBG_IOC_GET_SYSCALL_RULE_CONFIG:
		return lkmdbg_get_syscall_rule_config(session, argp);
	case LKMDBG_IOC_UPSERT_SYSCALL_RULE:
		return lkmdbg_upsert_syscall_rule(session, argp);
	case LKMDBG_IOC_REMOVE_SYSCALL_RULE:
		return lkmdbg_remove_syscall_rule(session, argp);
	case LKMDBG_IOC_QUERY_SYSCALL_RULES:
		return lkmdbg_query_syscall_rules(session, argp);
	case LKMDBG_IOC_RESOLVE_SYSCALL:
		return lkmdbg_resolve_syscall(session, argp);
	case LKMDBG_IOC_SET_STEALTH:
		return lkmdbg_set_stealth(session, argp);
	case LKMDBG_IOC_GET_STEALTH:
		return lkmdbg_get_stealth(session, argp);
	case LKMDBG_IOC_QUERY_INPUT_DEVICES:
		return lkmdbg_query_input_devices(session, argp);
	case LKMDBG_IOC_GET_INPUT_DEVICE_INFO:
		return lkmdbg_get_input_device_info(session, argp);
	case LKMDBG_IOC_OPEN_INPUT_CHANNEL:
		return lkmdbg_open_input_channel(session, argp);
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
	case LKMDBG_IOC_REMOTE_CALL:
		return lkmdbg_remote_call(session, argp);
	case LKMDBG_IOC_REMOTE_THREAD_CREATE:
		return lkmdbg_remote_thread_create(session, argp);
	case LKMDBG_IOC_CREATE_VIEW_REGION:
		return lkmdbg_create_view_region(session, argp);
	case LKMDBG_IOC_REMOVE_VIEW_REGION:
		return lkmdbg_remove_view_region(session, argp);
	case LKMDBG_IOC_SET_VIEW_BACKING:
		return lkmdbg_set_view_backing(session, argp);
	case LKMDBG_IOC_SET_VIEW_POLICY:
		return lkmdbg_set_view_policy(session, argp);
	case LKMDBG_IOC_QUERY_VIEW_REGIONS:
		return lkmdbg_query_view_regions(session, argp);
	case LKMDBG_IOC_FREEZE_THREADS:
		return lkmdbg_freeze_threads(session, argp);
	case LKMDBG_IOC_THAW_THREADS:
		mutex_lock(&session->lock);
		old_calls = session->syscall_control.active;
		mutex_unlock(&session->lock);
		if (old_calls)
			return -EBUSY;
		if (lkmdbg_remote_call_blocks_manual_thaw(session))
			return -EBUSY;
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
	.llseek = noop_llseek,
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
	init_waitqueue_head(&session->remote_call_waitq);
	init_waitqueue_head(&session->stop_waitq);
	INIT_LIST_HEAD(&session->node);
	INIT_LIST_HEAD(&session->hwpoints);
		INIT_LIST_HEAD(&session->pte_patches);
		INIT_LIST_HEAD(&session->remote_maps);
		INIT_LIST_HEAD(&session->remote_allocs);
		INIT_LIST_HEAD(&session->view_regions);
		INIT_LIST_HEAD(&session->input_channels);
		INIT_LIST_HEAD(&session->syscall_rules);
	INIT_WORK(&session->stop_work, lkmdbg_session_stop_workfn);
	atomic_set(&session->async_refs, 0);
	session->owner_tgid = current->tgid;
	session->syscall_trace_nr = -1;
	session->syscall_rule_mode = LKMDBG_SYSCALL_RULE_MODE_ENFORCE;
	session->syscall_rule_event_policy =
		LKMDBG_SYSCALL_RULE_EVENT_RAW_AND_RULE;
	session->remote_call.session = session;
	lkmdbg_session_supported_event_mask(session->event_mask_words);
	lkmdbg_session_reset_syscall_control_locked(session);

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
