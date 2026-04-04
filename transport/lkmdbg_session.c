#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#ifdef CONFIG_ARM64
#include <asm/ptrace.h>
#endif

#include "lkmdbg_internal.h"

#define LKMDBG_SYSCALL_RULE_MAX_ENTRIES 128U
void lkmdbg_session_stop_workfn(struct work_struct *work);

struct lkmdbg_syscall_rule {
	struct list_head node;
	struct lkmdbg_syscall_rule_entry entry;
};

/*
 * Session list ownership, event-mask helpers, status/event-config handlers,
 * and file-ops now live in transport/lkmdbg_session_core.c.
 */
#if 0
static bool lkmdbg_session_owner_active(pid_t owner_tgid)
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

static void lkmdbg_session_supported_event_mask(u64 *mask_words)
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
	lkmdbg_event_mask_set(mask_words, LKMDBG_EVENT_TARGET_SYSCALL_RULE_DETAIL);
}

static bool lkmdbg_session_event_type_enabled(const struct lkmdbg_session *session,
					      u32 type)
{
	u64 mask_words[LKMDBG_EVENT_MASK_WORDS];
	u32 i;

	for (i = 0; i < LKMDBG_EVENT_MASK_WORDS; i++)
		mask_words[i] = READ_ONCE(session->event_mask_words[i]);

	return lkmdbg_event_mask_test(mask_words, type);
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

#endif

#ifdef CONFIG_ARM64
static void lkmdbg_regs_arm64_export_control(struct lkmdbg_regs_arm64 *dst,
					     const struct pt_regs *src)
{
	unsigned int i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < ARRAY_SIZE(dst->regs); i++)
		dst->regs[i] = src->regs[i];
	dst->sp = src->sp;
	dst->pc = src->pc;
	dst->pstate = src->pstate;
}
#endif

static bool lkmdbg_session_stop_matches(const struct lkmdbg_stop_state *stop,
					u32 reason, u64 value1)
{
	return stop && (stop->flags & LKMDBG_STOP_FLAG_ACTIVE) &&
	       stop->reason == reason && stop->value1 == value1;
}

#if 0
static void lkmdbg_session_zero_stop(struct lkmdbg_stop_state *stop)
{
	memset(stop, 0, sizeof(*stop));
}

static bool lkmdbg_session_has_events(struct lkmdbg_session *session)
{
	return READ_ONCE(session->event_count) > 0;
}

static void lkmdbg_session_reset_syscall_control_locked(
	struct lkmdbg_session *session)
{
	memset(&session->syscall_control, 0, sizeof(session->syscall_control));
	init_waitqueue_head(&session->syscall_control.waitq);
}

static bool lkmdbg_session_stop_matches(const struct lkmdbg_stop_state *stop,
					u32 reason, u64 value1)
{
	return stop && (stop->flags & LKMDBG_STOP_FLAG_ACTIVE) &&
	       stop->reason == reason && stop->value1 == value1;
}

static int lkmdbg_session_prepare_continue_syscall_control(
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

static void lkmdbg_session_finish_continue_syscall_control(
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

static void lkmdbg_session_fail_syscall_control(struct lkmdbg_session *session)
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

static bool
lkmdbg_session_syscall_control_matches(const struct lkmdbg_session *session,
				       pid_t tid, s32 syscall_nr)
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
#endif

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

bool lkmdbg_session_has_target_event_type(pid_t target_tgid, u32 type)
{
	struct lkmdbg_session *session;
	unsigned long irqflags;
	bool matched = false;

	if (target_tgid <= 0)
		return false;

	spin_lock_irqsave(&lkmdbg_session_list_lock, irqflags);
	list_for_each_entry(session, &lkmdbg_session_list, node) {
		if (READ_ONCE(session->target_tgid) != target_tgid)
			continue;
		if (!lkmdbg_session_event_type_enabled(session, type))
			continue;

		matched = true;
		break;
	}
	spin_unlock_irqrestore(&lkmdbg_session_list_lock, irqflags);

	return matched;
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
	wake_up_all(&session->stop_waitq);
}

void lkmdbg_session_clear_stop(struct lkmdbg_session *session)
{
	mutex_lock(&session->lock);
	lkmdbg_session_zero_stop(&session->stop_state);
	mutex_unlock(&session->lock);
	wake_up_all(&session->stop_waitq);
}

void lkmdbg_session_stop_workfn(struct work_struct *work)
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

	if (closing || !stop.reason) {
		lkmdbg_remote_call_fail_stop(session, &stop);
		lkmdbg_session_fail_syscall_control(session);
		goto out;
	}

	mutex_lock(&session->lock);
	if (session->target_gen != target_gen || session->target_tgid != stop.tgid) {
		mutex_unlock(&session->lock);
		lkmdbg_remote_call_fail_stop(session, &stop);
		lkmdbg_session_fail_syscall_control(session);
		goto out;
	}
	mutex_unlock(&session->lock);

	ret = lkmdbg_session_freeze_target(session, 1000, &freeze_req);
	if (ret) {
		lkmdbg_pr_warn("lkmdbg: async stop freeze failed tgid=%d tid=%d reason=%u ret=%d\n",
			stop.tgid, stop.tid, stop.reason, ret);
		lkmdbg_remote_call_fail_stop(session, &stop);
		lkmdbg_session_fail_syscall_control(session);
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

int lkmdbg_session_request_async_stop(struct lkmdbg_session *session,
				      u32 reason, pid_t tgid, pid_t tid,
				      u32 event_flags, u32 stop_flags,
				      u64 value0, u64 value1,
				      const struct lkmdbg_regs_arm64 *regs)
{
	bool scheduled = false;

	if (!session || tgid <= 0)
		return -EINVAL;

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

	return scheduled ? 0 : -EBUSY;
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

static bool lkmdbg_session_signal_mask_matches(const struct lkmdbg_session *session,
					       u32 sig)
{
	u64 word;
	u32 index;

	if (!sig)
		return false;

	index = (sig - 1U) / 64U;
	if (index >= ARRAY_SIZE(session->signal_mask_words))
		return false;

	word = READ_ONCE(session->signal_mask_words[index]);
	return !!(word & (1ULL << ((sig - 1U) % 64U)));
}

static bool lkmdbg_session_syscall_trace_matches(
	const struct lkmdbg_session *session, pid_t tid, u32 phase,
	s32 syscall_nr, u32 *mode_out)
{
	u32 phases;
	u32 mode;
	s32 filter_tid;
	s32 filter_nr;

	mode = READ_ONCE(session->syscall_trace_mode);
	if (!(mode &
	      (LKMDBG_SYSCALL_TRACE_MODE_EVENT | LKMDBG_SYSCALL_TRACE_MODE_STOP)))
		return false;

	phases = READ_ONCE(session->syscall_trace_phases);
	if (!(phases & phase))
		return false;

	filter_tid = READ_ONCE(session->syscall_trace_tid);
	if (filter_tid > 0 && filter_tid != tid)
		return false;

	filter_nr = READ_ONCE(session->syscall_trace_nr);
	if (filter_nr >= 0 && filter_nr != syscall_nr)
		return false;

	if (mode_out)
		*mode_out = mode;
	return true;
}

static bool lkmdbg_syscall_rule_mode_valid(u32 mode)
{
	return mode == LKMDBG_SYSCALL_RULE_MODE_OBSERVE ||
	       mode == LKMDBG_SYSCALL_RULE_MODE_ENFORCE;
}

static bool lkmdbg_syscall_rule_event_policy_valid(u32 event_policy)
{
	return event_policy == LKMDBG_SYSCALL_RULE_EVENT_RAW_ONLY ||
	       event_policy == LKMDBG_SYSCALL_RULE_EVENT_RAW_AND_RULE ||
	       event_policy == LKMDBG_SYSCALL_RULE_EVENT_RULE_ONLY;
}

static u32 lkmdbg_syscall_rule_supported_mode_mask(void)
{
	return (1U << LKMDBG_SYSCALL_RULE_MODE_OBSERVE) |
	       (1U << LKMDBG_SYSCALL_RULE_MODE_ENFORCE);
}

static u32 lkmdbg_syscall_rule_supported_event_policy_mask(void)
{
	return (1U << LKMDBG_SYSCALL_RULE_EVENT_RAW_ONLY) |
	       (1U << LKMDBG_SYSCALL_RULE_EVENT_RAW_AND_RULE) |
	       (1U << LKMDBG_SYSCALL_RULE_EVENT_RULE_ONLY);
}

static bool lkmdbg_session_syscall_rule_emit_raw(const struct lkmdbg_session *session)
{
	return READ_ONCE(session->syscall_rule_event_policy) !=
	       LKMDBG_SYSCALL_RULE_EVENT_RULE_ONLY;
}

static bool lkmdbg_session_syscall_rule_emit_result(
	const struct lkmdbg_session *session)
{
	return READ_ONCE(session->syscall_rule_event_policy) !=
	       LKMDBG_SYSCALL_RULE_EVENT_RAW_ONLY;
}

static bool lkmdbg_syscall_rule_args_match(
	const struct lkmdbg_syscall_rule_entry *entry, const struct pt_regs *regs)
{
	u32 i;

	if (!entry->arg_match_mask)
		return true;

#ifdef CONFIG_ARM64
	if (!regs)
		return false;

	for (i = 0; i < LKMDBG_SYSCALL_RULE_ARG_MAX; i++) {
		u64 mask;
		u64 expected;
		u64 actual;

		if (!(entry->arg_match_mask & (1U << i)))
			continue;
		mask = entry->arg_value_masks[i];
		expected = entry->arg_values[i] & mask;
		actual = regs->regs[i] & mask;
		if (actual != expected)
			return false;
	}

	return true;
#else
	(void)regs;
	return false;
#endif
}

static void lkmdbg_syscall_rule_apply_rewrite_args(
	const struct lkmdbg_syscall_rule_entry *entry, struct pt_regs *regs)
{
#ifdef CONFIG_ARM64
	u32 i;

	if (!regs || !entry->rewrite_mask)
		return;

	for (i = 0; i < LKMDBG_SYSCALL_RULE_ARG_MAX; i++) {
		if (entry->rewrite_mask & (1U << i))
			regs->regs[i] = entry->rewrite_args[i];
	}
#else
	(void)entry;
	(void)regs;
#endif
}

static bool lkmdbg_syscall_rule_apply_rewrite_nr(
	const struct lkmdbg_syscall_rule_entry *entry, bool rewrite_supported,
	struct pt_regs *regs, s32 *syscall_nr_io)
{
#ifdef CONFIG_ARM64
	s32 nr;

	if (!entry || !regs || !syscall_nr_io)
		return false;
	if (!rewrite_supported || entry->rewrite_syscall_nr < 0)
		return false;

	nr = entry->rewrite_syscall_nr;
	regs->regs[8] = (u64)(u32)nr;
	*syscall_nr_io = nr;
	return true;
#else
	(void)entry;
	(void)rewrite_supported;
	(void)regs;
	(void)syscall_nr_io;
	return false;
#endif
}

static struct lkmdbg_syscall_rule *
lkmdbg_find_syscall_rule_locked(struct lkmdbg_session *session, u64 rule_id)
{
	struct lkmdbg_syscall_rule *rule;

	list_for_each_entry(rule, &session->syscall_rules, node) {
		if (rule->entry.rule_id == rule_id)
			return rule;
	}

	return NULL;
}

void lkmdbg_release_syscall_rules_locked(struct lkmdbg_session *session)
{
	struct lkmdbg_syscall_rule *rule;
	struct lkmdbg_syscall_rule *tmp;

	list_for_each_entry_safe(rule, tmp, &session->syscall_rules, node) {
		list_del_init(&rule->node);
		kfree(rule);
	}
	session->syscall_rule_count = 0;
	session->next_syscall_rule_id = 0;
}

static bool lkmdbg_session_syscall_rule_entry_matches(
	const struct lkmdbg_syscall_rule_entry *entry, pid_t tid, s32 syscall_nr,
	u32 phase, const struct pt_regs *regs)
{
	if (!(entry->flags & LKMDBG_SYSCALL_RULE_FLAG_ENABLED))
		return false;
	if (!(entry->phases & phase))
		return false;
	if (entry->tid > 0 && entry->tid != tid)
		return false;
	if (entry->syscall_nr >= 0 && entry->syscall_nr != syscall_nr)
		return false;
	if (phase == LKMDBG_SYSCALL_TRACE_PHASE_ENTER &&
	    !lkmdbg_syscall_rule_args_match(entry, regs))
		return false;
	return true;
}

static bool lkmdbg_session_pick_syscall_rule_locked(
	struct lkmdbg_session *session, pid_t tid, s32 syscall_nr, u32 phase,
	const struct pt_regs *regs, struct lkmdbg_syscall_rule **rule_out)
{
	struct lkmdbg_syscall_rule *rule;
	struct lkmdbg_syscall_rule *best = NULL;

	list_for_each_entry(rule, &session->syscall_rules, node) {
		if (!lkmdbg_session_syscall_rule_entry_matches(&rule->entry, tid,
							      syscall_nr, phase,
							      regs))
			continue;
		if (!best || rule->entry.priority > best->entry.priority ||
		    (rule->entry.priority == best->entry.priority &&
		     rule->entry.rule_id < best->entry.rule_id))
			best = rule;
	}

	if (!best)
		return false;

	if (rule_out)
		*rule_out = best;
	return true;
}

void lkmdbg_session_broadcast_signal_event(pid_t target_tgid, u32 sig,
					   pid_t tid, u32 flags,
					   u64 siginfo_code, int result)
{
	struct lkmdbg_session *session;
	struct lkmdbg_session *stop_sessions[64];
	u32 stop_count = 0;
	unsigned long irqflags;

	if (target_tgid <= 0 || !sig)
		return;

	spin_lock_irqsave(&lkmdbg_session_list_lock, irqflags);
	list_for_each_entry(session, &lkmdbg_session_list, node) {
		unsigned long session_irqflags;
		pid_t session_tgid;
		u32 signal_flags;

		session_tgid = READ_ONCE(session->target_tgid);
		if (session_tgid != target_tgid)
			continue;

		spin_lock_irqsave(&session->event_lock, session_irqflags);
		lkmdbg_session_queue_event_locked(session, LKMDBG_EVENT_TARGET_SIGNAL,
						  sig, target_tgid, tid, flags,
						  siginfo_code, (u64)result);
		spin_unlock_irqrestore(&session->event_lock, session_irqflags);
		wake_up_interruptible(&session->readq);

		signal_flags = READ_ONCE(session->signal_flags);
		if (!(signal_flags & LKMDBG_SIGNAL_CONFIG_STOP) || result ||
		    !lkmdbg_session_signal_mask_matches(session, sig))
			continue;

		atomic_inc(&session->async_refs);
		if (stop_count < ARRAY_SIZE(stop_sessions))
			stop_sessions[stop_count++] = session;
		else
			lkmdbg_session_async_put(session);
	}
	spin_unlock_irqrestore(&lkmdbg_session_list_lock, irqflags);

	while (stop_count > 0) {
		session = stop_sessions[--stop_count];
		lkmdbg_session_request_async_stop(session, LKMDBG_STOP_REASON_SIGNAL,
						  target_tgid, tid, flags, 0,
						  (u64)sig, siginfo_code, NULL);
		lkmdbg_session_async_put(session);
	}
}

void lkmdbg_session_broadcast_syscall_event(
	pid_t target_tgid, pid_t tid, u32 phase, s32 syscall_nr, s64 retval,
	const struct lkmdbg_regs_arm64 *regs)
{
	struct lkmdbg_session *session;
	struct lkmdbg_session *stop_sessions[64];
	u32 stop_count = 0;
	unsigned long irqflags;

	if (target_tgid <= 0 || tid <= 0 || syscall_nr < 0)
		return;

	spin_lock_irqsave(&lkmdbg_session_list_lock, irqflags);
	list_for_each_entry(session, &lkmdbg_session_list, node) {
		unsigned long session_irqflags;
		pid_t session_tgid;
		u32 mode;

		session_tgid = READ_ONCE(session->target_tgid);
		if (session_tgid != target_tgid)
			continue;
		if (!lkmdbg_session_syscall_trace_matches(session, tid, phase,
							  syscall_nr, &mode))
			continue;

		if ((mode & LKMDBG_SYSCALL_TRACE_MODE_EVENT) &&
		    lkmdbg_session_syscall_rule_emit_raw(session)) {
			spin_lock_irqsave(&session->event_lock, session_irqflags);
			lkmdbg_session_queue_event_locked(
				session, LKMDBG_EVENT_TARGET_SYSCALL, 0,
				target_tgid, tid, phase, (u64)(u32)syscall_nr,
				(u64)retval);
			spin_unlock_irqrestore(&session->event_lock,
					       session_irqflags);
			wake_up_interruptible(&session->readq);
		}

		if (!(mode & LKMDBG_SYSCALL_TRACE_MODE_STOP))
			continue;

		atomic_inc(&session->async_refs);
		if (stop_count < ARRAY_SIZE(stop_sessions))
			stop_sessions[stop_count++] = session;
		else
			lkmdbg_session_async_put(session);
	}
	spin_unlock_irqrestore(&lkmdbg_session_list_lock, irqflags);

	while (stop_count > 0) {
		session = stop_sessions[--stop_count];
		lkmdbg_session_request_async_stop(
			session, LKMDBG_STOP_REASON_SYSCALL, target_tgid, tid,
			phase, 0, (u64)(u32)syscall_nr, (u64)retval, regs);
		lkmdbg_session_async_put(session);
	}
}

static void lkmdbg_session_queue_syscall_rule_detail_event(
	struct lkmdbg_session *session, pid_t tgid, pid_t tid, u32 phase,
	u32 applied_actions, u64 value0, u64 value1)
{
	if (!session)
		return;

	lkmdbg_session_queue_event_ex(session,
				      LKMDBG_EVENT_TARGET_SYSCALL_RULE_DETAIL,
				      applied_actions, tgid, tid, phase, value0,
				      value1);
}

int lkmdbg_control_syscall_entry(struct pt_regs *regs, s32 *syscall_nr_io,
				 bool nr_rewrite_supported, bool *skip_out,
				 s64 *retval_out)
{
#ifndef CONFIG_ARM64
	(void)regs;
	(void)syscall_nr_io;
	(void)nr_rewrite_supported;
	(void)skip_out;
	(void)retval_out;
	return 0;
#else
	struct lkmdbg_session *candidate;
	struct lkmdbg_session *session = NULL;
	struct lkmdbg_syscall_control_state snapshot;
	struct lkmdbg_syscall_rule *rule = NULL;
	struct lkmdbg_regs_arm64 stop_regs;
	unsigned long irqflags;
	s32 syscall_nr_before;
	s32 syscall_nr_effective;
	u32 requested_actions = 0;
	u32 applied_actions = 0;
	u64 rule_id = 0;
	bool matched_rule = false;
	bool emit_rule_event = false;
	bool request_rule_stop = false;
	bool control_match = false;
	bool rule_candidate = false;
	bool skip = false;
	s64 retval = 0;
	int ret;

	if (!regs || !syscall_nr_io || current->tgid <= 0 || current->pid <= 0 ||
	    *syscall_nr_io < 0)
		return 0;

	syscall_nr_before = *syscall_nr_io;
	syscall_nr_effective = syscall_nr_before;

	spin_lock_irqsave(&lkmdbg_session_list_lock, irqflags);
	list_for_each_entry(candidate, &lkmdbg_session_list, node) {
		if (READ_ONCE(candidate->target_tgid) != current->tgid)
			continue;
		control_match = lkmdbg_session_syscall_control_matches(
			candidate, current->pid, syscall_nr_before);
		rule_candidate =
			READ_ONCE(candidate->syscall_rule_count) > 0 &&
			(READ_ONCE(candidate->syscall_trace_mode) &
			 LKMDBG_SYSCALL_TRACE_MODE_CONTROL) &&
			(READ_ONCE(candidate->syscall_trace_phases) &
			 LKMDBG_SYSCALL_TRACE_PHASE_ENTER);
		if (!control_match && !rule_candidate)
			continue;
		atomic_inc(&candidate->async_refs);
		session = candidate;
		break;
	}
	spin_unlock_irqrestore(&lkmdbg_session_list_lock, irqflags);

	if (!session)
		return 0;

	lkmdbg_regs_arm64_export_control(&stop_regs, regs);

	mutex_lock(&session->lock);
	if (session->closing || session->target_tgid != current->tgid ||
	    session->freezer) {
		mutex_unlock(&session->lock);
		lkmdbg_session_async_put(session);
		return 0;
	}

	if (session->syscall_rule_count > 0) {
		matched_rule = lkmdbg_session_pick_syscall_rule_locked(
			session, current->pid, syscall_nr_before,
			LKMDBG_SYSCALL_TRACE_PHASE_ENTER, regs, &rule);
		if (matched_rule) {
			requested_actions = rule->entry.actions;
			rule_id = rule->entry.rule_id;
			rule->entry.hits++;

			if (session->syscall_rule_mode ==
			    LKMDBG_SYSCALL_RULE_MODE_ENFORCE) {
				if (requested_actions &
				    LKMDBG_SYSCALL_RULE_ACTION_REWRITE_NR) {
					if (lkmdbg_syscall_rule_apply_rewrite_nr(
						    &rule->entry,
						    nr_rewrite_supported, regs,
						    &syscall_nr_effective))
						applied_actions |=
							LKMDBG_SYSCALL_RULE_ACTION_REWRITE_NR;
				}
				if (requested_actions &
				    LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS) {
					lkmdbg_syscall_rule_apply_rewrite_args(
						&rule->entry, regs);
					applied_actions |=
						LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS;
				}
				if (requested_actions &
				    LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN) {
					skip = true;
					retval = rule->entry.retval;
					regs->regs[0] = (u64)retval;
					applied_actions |=
						LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN;
				}
				if (requested_actions &
				    LKMDBG_SYSCALL_RULE_ACTION_STOP) {
					request_rule_stop = true;
					applied_actions |=
						LKMDBG_SYSCALL_RULE_ACTION_STOP;
				}
			}

			if (rule->entry.flags & LKMDBG_SYSCALL_RULE_FLAG_ONESHOT)
				rule->entry.flags &=
					~LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
			lkmdbg_regs_arm64_export_control(&stop_regs, regs);
		}

		emit_rule_event =
			matched_rule && lkmdbg_session_syscall_rule_emit_result(session);
		mutex_unlock(&session->lock);

		if (emit_rule_event)
			lkmdbg_session_queue_event_ex(
				session, LKMDBG_EVENT_TARGET_SYSCALL_RULE,
				applied_actions, current->tgid, current->pid,
				LKMDBG_SYSCALL_TRACE_PHASE_ENTER,
				(u64)(u32)syscall_nr_effective, rule_id);

		if (emit_rule_event)
			lkmdbg_session_queue_syscall_rule_detail_event(
				session, current->tgid, current->pid,
				LKMDBG_SYSCALL_TRACE_PHASE_ENTER,
				applied_actions, (u64)(u32)syscall_nr_before,
				(u64)(u32)syscall_nr_effective);

		if (request_rule_stop) {
			(void)lkmdbg_session_request_async_stop(
				session, LKMDBG_STOP_REASON_SYSCALL, current->tgid,
				current->pid, LKMDBG_SYSCALL_TRACE_PHASE_ENTER, 0,
				(u64)(u32)syscall_nr_effective, rule_id,
				&stop_regs);
		}

		lkmdbg_session_async_put(session);
		*syscall_nr_io = syscall_nr_effective;
		if (skip_out)
			*skip_out = skip;
		if (retval_out)
			*retval_out = retval;
		return 0;
	}

	if ((session->stop_state.flags & LKMDBG_STOP_FLAG_ACTIVE) ||
	    session->stop_work_pending || session->syscall_control.active) {
		mutex_unlock(&session->lock);
		lkmdbg_session_async_put(session);
		return 0;
	}

	session->syscall_control.active = true;
	session->syscall_control.resolved = false;
	session->syscall_control.resume = false;
	session->syscall_control.abort = false;
	session->syscall_control.action = LKMDBG_SYSCALL_RESOLVE_ACTION_ALLOW;
	session->syscall_control.backend_flags =
		nr_rewrite_supported ?
			LKMDBG_SYSCALL_RESOLVE_FLAG_NR_REWRITE_SUPPORTED :
			0;
	session->syscall_control.tgid = current->tgid;
	session->syscall_control.tid = current->pid;
	session->syscall_control.syscall_nr = syscall_nr_before;
	session->syscall_control.retval = 0;
	session->syscall_control.regs = stop_regs;
	memcpy(session->syscall_control.args, stop_regs.regs,
	       sizeof(session->syscall_control.args));
	mutex_unlock(&session->lock);

	ret = lkmdbg_session_request_async_stop(
		session, LKMDBG_STOP_REASON_SYSCALL, current->tgid, current->pid,
		LKMDBG_SYSCALL_TRACE_PHASE_ENTER,
		LKMDBG_STOP_FLAG_SYSCALL_CONTROL, (u64)(u32)syscall_nr_before, 0,
		&stop_regs);
	if (ret) {
		lkmdbg_session_fail_syscall_control(session);
		mutex_lock(&session->lock);
		lkmdbg_session_reset_syscall_control_locked(session);
		mutex_unlock(&session->lock);
		lkmdbg_session_async_put(session);
		return 0;
	}

	wait_event(session->syscall_control.waitq,
		   ({
			   bool done;

			   mutex_lock(&session->lock);
			   done = session->closing ||
				  session->syscall_control.abort ||
				  (!session->syscall_control.active) ||
				  (session->syscall_control.resolved &&
				   session->syscall_control.resume);
			   if (done)
				   snapshot = session->syscall_control;
			   mutex_unlock(&session->lock);
			   done;
		   }));

	if (!snapshot.abort && snapshot.active &&
	    snapshot.action == LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE) {
		memcpy(regs->regs, snapshot.args, sizeof(snapshot.args));
		if (snapshot.syscall_nr >= 0 &&
		    snapshot.syscall_nr != *syscall_nr_io &&
		    (snapshot.backend_flags &
		     LKMDBG_SYSCALL_RESOLVE_FLAG_NR_REWRITE_SUPPORTED)) {
			regs->regs[8] = (u64)(u32)snapshot.syscall_nr;
			*syscall_nr_io = snapshot.syscall_nr;
		}
	} else if (!snapshot.abort &&
		   snapshot.action == LKMDBG_SYSCALL_RESOLVE_ACTION_SKIP) {
		skip = true;
		retval = snapshot.retval;
		regs->regs[0] = (u64)retval;
	}

	mutex_lock(&session->lock);
	lkmdbg_session_reset_syscall_control_locked(session);
	mutex_unlock(&session->lock);
	wake_up_all(&session->syscall_control.waitq);
	lkmdbg_session_async_put(session);

	if (skip_out)
		*skip_out = skip;
	if (retval_out)
		*retval_out = retval;

	return 0;
#endif
}

int lkmdbg_control_syscall_exit(struct pt_regs *regs, s32 syscall_nr,
				s64 *retval_io)
{
#ifndef CONFIG_ARM64
	(void)regs;
	(void)syscall_nr;
	(void)retval_io;
	return 0;
#else
	struct lkmdbg_session *candidate;
	struct lkmdbg_session *session = NULL;
	struct lkmdbg_syscall_rule *rule = NULL;
	struct lkmdbg_regs_arm64 stop_regs;
	unsigned long irqflags;
	u32 requested_actions = 0;
	u32 applied_actions = 0;
	u64 rule_id = 0;
	bool matched_rule = false;
	bool emit_rule_event = false;
	bool request_rule_stop = false;
	s64 retval_before = retval_io ? *retval_io : 0;
	s64 retval = retval_io ? *retval_io : 0;

	if (!regs || current->tgid <= 0 || current->pid <= 0 || syscall_nr < 0)
		return 0;

	spin_lock_irqsave(&lkmdbg_session_list_lock, irqflags);
	list_for_each_entry(candidate, &lkmdbg_session_list, node) {
		if (READ_ONCE(candidate->target_tgid) != current->tgid)
			continue;
		if (!(READ_ONCE(candidate->syscall_trace_mode) &
		      LKMDBG_SYSCALL_TRACE_MODE_CONTROL))
			continue;
		if (!(READ_ONCE(candidate->syscall_trace_phases) &
		      LKMDBG_SYSCALL_TRACE_PHASE_EXIT))
			continue;
		if (READ_ONCE(candidate->syscall_rule_count) == 0)
			continue;
		atomic_inc(&candidate->async_refs);
		session = candidate;
		break;
	}
	spin_unlock_irqrestore(&lkmdbg_session_list_lock, irqflags);

	if (!session)
		return 0;

	lkmdbg_regs_arm64_export_control(&stop_regs, regs);

	mutex_lock(&session->lock);
	if (session->closing || session->target_tgid != current->tgid ||
	    session->freezer) {
		mutex_unlock(&session->lock);
		lkmdbg_session_async_put(session);
		return 0;
	}

	matched_rule = lkmdbg_session_pick_syscall_rule_locked(
		session, current->pid, syscall_nr, LKMDBG_SYSCALL_TRACE_PHASE_EXIT,
		regs, &rule);
	if (matched_rule) {
		requested_actions = rule->entry.actions;
		rule_id = rule->entry.rule_id;
		rule->entry.hits++;

		if (session->syscall_rule_mode == LKMDBG_SYSCALL_RULE_MODE_ENFORCE) {
			if (requested_actions & LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN) {
				retval = rule->entry.retval;
				regs->regs[0] = (u64)retval;
				applied_actions |=
					LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN;
			}
			if (requested_actions &
			    LKMDBG_SYSCALL_RULE_ACTION_REWRITE_RETVAL) {
				retval = rule->entry.retval;
				regs->regs[0] = (u64)retval;
				applied_actions |=
					LKMDBG_SYSCALL_RULE_ACTION_REWRITE_RETVAL;
			}
			if (requested_actions & LKMDBG_SYSCALL_RULE_ACTION_STOP) {
				request_rule_stop = true;
				applied_actions |=
					LKMDBG_SYSCALL_RULE_ACTION_STOP;
			}
		}

		if (rule->entry.flags & LKMDBG_SYSCALL_RULE_FLAG_ONESHOT)
			rule->entry.flags &= ~LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
		lkmdbg_regs_arm64_export_control(&stop_regs, regs);
	}

	emit_rule_event =
		matched_rule && lkmdbg_session_syscall_rule_emit_result(session);
	mutex_unlock(&session->lock);

	if (emit_rule_event)
		lkmdbg_session_queue_event_ex(session,
					      LKMDBG_EVENT_TARGET_SYSCALL_RULE,
					      applied_actions, current->tgid,
					      current->pid,
					      LKMDBG_SYSCALL_TRACE_PHASE_EXIT,
					      (u64)(u32)syscall_nr, rule_id);

	if (emit_rule_event)
		lkmdbg_session_queue_syscall_rule_detail_event(
			session, current->tgid, current->pid,
			LKMDBG_SYSCALL_TRACE_PHASE_EXIT, applied_actions,
			(u64)retval_before, (u64)retval);

	if (request_rule_stop) {
		(void)lkmdbg_session_request_async_stop(
			session, LKMDBG_STOP_REASON_SYSCALL, current->tgid,
			current->pid, LKMDBG_SYSCALL_TRACE_PHASE_EXIT, 0,
			(u64)(u32)syscall_nr, rule_id, &stop_regs);
	}

	lkmdbg_session_async_put(session);
	if (retval_io)
		*retval_io = retval;
	return 0;
#endif
}

#if 0
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

#endif

static int lkmdbg_validate_continue_request(struct lkmdbg_continue_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (req->flags & ~LKMDBG_CONTINUE_FLAG_REARM_HWPOINTS)
		return -EINVAL;
	return 0;
}

static int lkmdbg_validate_syscall_resolve_request(
	struct lkmdbg_syscall_resolve_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (req->action > LKMDBG_SYSCALL_RESOLVE_ACTION_SKIP)
		return -EINVAL;
	if (req->action == LKMDBG_SYSCALL_RESOLVE_ACTION_ALLOW &&
	    (req->syscall_nr != -1 || req->retval))
		return -EINVAL;
	return 0;
}

static int lkmdbg_validate_syscall_rule_config_request(
	struct lkmdbg_syscall_rule_config_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!lkmdbg_syscall_rule_mode_valid(req->mode))
		return -EINVAL;
	if (!lkmdbg_syscall_rule_event_policy_valid(req->event_policy))
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	return 0;
}

long lkmdbg_set_syscall_rule_config(struct lkmdbg_session *session,
				    void __user *argp)
{
	struct lkmdbg_syscall_rule_config_request req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (lkmdbg_validate_syscall_rule_config_request(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	session->syscall_rule_mode = req.mode;
	session->syscall_rule_event_policy = req.event_policy;
	mutex_unlock(&session->lock);

	req.supported_mode_mask = lkmdbg_syscall_rule_supported_mode_mask();
	req.supported_event_policy_mask =
		lkmdbg_syscall_rule_supported_event_policy_mask();

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_get_syscall_rule_config(struct lkmdbg_session *session,
				    void __user *argp)
{
	struct lkmdbg_syscall_rule_config_request req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req) ||
	    req.flags)
		return -EINVAL;

	mutex_lock(&session->lock);
	req.mode = session->syscall_rule_mode;
	req.event_policy = session->syscall_rule_event_policy;
	mutex_unlock(&session->lock);

	req.supported_mode_mask = lkmdbg_syscall_rule_supported_mode_mask();
	req.supported_event_policy_mask =
		lkmdbg_syscall_rule_supported_event_policy_mask();

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static int lkmdbg_validate_syscall_rule_request(
	struct lkmdbg_syscall_rule_request *req)
{
	u32 valid_actions = LKMDBG_SYSCALL_RULE_ACTION_SET_RETURN |
			    LKMDBG_SYSCALL_RULE_ACTION_STOP |
			    LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS |
			    LKMDBG_SYSCALL_RULE_ACTION_REWRITE_NR |
			    LKMDBG_SYSCALL_RULE_ACTION_REWRITE_RETVAL;
	u32 valid_flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED |
			  LKMDBG_SYSCALL_RULE_FLAG_ONESHOT;
	u32 i;

	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (req->rule.tid < 0 || req->rule.syscall_nr < -1)
		return -EINVAL;
	if (!req->rule.phases ||
	    (req->rule.phases &
	     ~(LKMDBG_SYSCALL_TRACE_PHASE_ENTER |
	       LKMDBG_SYSCALL_TRACE_PHASE_EXIT)))
		return -EINVAL;
	if (req->rule.actions & ~valid_actions)
		return -EINVAL;
	if (req->rule.flags & ~valid_flags)
		return -EINVAL;
	if (req->rule.arg_match_mask & ~LKMDBG_SYSCALL_RULE_ARG_MASK_ALL)
		return -EINVAL;
	if (req->rule.rewrite_mask & ~LKMDBG_SYSCALL_RULE_ARG_MASK_ALL)
		return -EINVAL;
	if ((req->rule.arg_match_mask || req->rule.rewrite_mask ||
	     (req->rule.actions & LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS)) &&
	    !(req->rule.phases & LKMDBG_SYSCALL_TRACE_PHASE_ENTER))
		return -EINVAL;
	if ((req->rule.actions & LKMDBG_SYSCALL_RULE_ACTION_REWRITE_NR) &&
	    !(req->rule.phases & LKMDBG_SYSCALL_TRACE_PHASE_ENTER))
		return -EINVAL;
	if ((req->rule.actions & LKMDBG_SYSCALL_RULE_ACTION_REWRITE_RETVAL) &&
	    !(req->rule.phases & LKMDBG_SYSCALL_TRACE_PHASE_EXIT))
		return -EINVAL;
	if (!(req->rule.actions & LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS) &&
	    req->rule.rewrite_mask)
		return -EINVAL;
	if ((req->rule.actions & LKMDBG_SYSCALL_RULE_ACTION_REWRITE_ARGS) &&
	    !req->rule.rewrite_mask)
		return -EINVAL;
	if ((req->rule.actions & LKMDBG_SYSCALL_RULE_ACTION_REWRITE_NR) &&
	    req->rule.rewrite_syscall_nr < 0)
		return -EINVAL;
	for (i = 0; i < LKMDBG_SYSCALL_RULE_ARG_MAX; i++) {
		if ((req->rule.arg_match_mask & (1U << i)) &&
		    !req->rule.arg_value_masks[i])
			return -EINVAL;
	}
	return 0;
}

long lkmdbg_upsert_syscall_rule(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_syscall_rule_request req;
	struct lkmdbg_syscall_rule *rule = NULL;
	struct lkmdbg_syscall_rule *new_rule = NULL;
	struct lkmdbg_syscall_rule_entry old_entry;
	long ret = 0;
	bool is_new = false;

	memset(&old_entry, 0, sizeof(old_entry));

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_syscall_rule_request(&req);
	if (ret)
		return ret;

	if (!req.rule.rule_id) {
		new_rule = kzalloc(sizeof(*new_rule), GFP_KERNEL);
		if (!new_rule)
			return -ENOMEM;
	}

	mutex_lock(&session->lock);
	if (!(session->syscall_trace_mode & LKMDBG_SYSCALL_TRACE_MODE_CONTROL)) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	if (req.rule.phases & ~session->syscall_trace_phases) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	if (req.rule.rule_id) {
		rule = lkmdbg_find_syscall_rule_locked(session, req.rule.rule_id);
		if (!rule) {
			ret = -ENOENT;
			goto out_unlock;
		}
		old_entry = rule->entry;
	} else {
		if (session->syscall_rule_count >= LKMDBG_SYSCALL_RULE_MAX_ENTRIES) {
			ret = -E2BIG;
			goto out_unlock;
		}
		rule = new_rule;
		new_rule = NULL;
		session->next_syscall_rule_id++;
		if (!session->next_syscall_rule_id)
			session->next_syscall_rule_id++;
		rule->entry.rule_id = session->next_syscall_rule_id;
		list_add_tail(&rule->node, &session->syscall_rules);
		session->syscall_rule_count++;
		is_new = true;
	}

	req.rule.rule_id = rule->entry.rule_id;
	if (!req.rule.flags && is_new)
		req.rule.flags = LKMDBG_SYSCALL_RULE_FLAG_ENABLED;
	req.rule.hits = is_new ? 0 : old_entry.hits;
	rule->entry = req.rule;
	mutex_unlock(&session->lock);

	kfree(new_rule);
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;

out_unlock:
	mutex_unlock(&session->lock);
	kfree(new_rule);
	return ret;
}

static int lkmdbg_validate_syscall_rule_handle_request(
	struct lkmdbg_syscall_rule_handle_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->rule_id || req->flags)
		return -EINVAL;
	return 0;
}

long lkmdbg_remove_syscall_rule(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_syscall_rule_handle_request req;
	struct lkmdbg_syscall_rule *rule;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (lkmdbg_validate_syscall_rule_handle_request(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	rule = lkmdbg_find_syscall_rule_locked(session, req.rule_id);
	if (!rule) {
		mutex_unlock(&session->lock);
		return -ENOENT;
	}
	list_del_init(&rule->node);
	if (session->syscall_rule_count > 0)
		session->syscall_rule_count--;
	mutex_unlock(&session->lock);

	kfree(rule);
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static int lkmdbg_validate_syscall_rule_query_request(
	struct lkmdbg_syscall_rule_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_SYSCALL_RULE_MAX_ENTRIES)
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	return 0;
}

long lkmdbg_query_syscall_rules(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_syscall_rule_query_request req;
	struct lkmdbg_syscall_rule_entry *entries;
	struct lkmdbg_syscall_rule *rule;
	u32 filled = 0;
	bool done = true;
	u64 next_id = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (lkmdbg_validate_syscall_rule_query_request(&req))
		return -EINVAL;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	mutex_lock(&session->lock);
	list_for_each_entry(rule, &session->syscall_rules, node) {
		if (rule->entry.rule_id < req.start_id)
			continue;
		if (filled >= req.max_entries) {
			done = false;
			next_id = rule->entry.rule_id;
			break;
		}
		entries[filled++] = rule->entry;
	}
	mutex_unlock(&session->lock);

	req.entries_filled = filled;
	req.done = done ? 1U : 0U;
	req.next_id = done ? 0 : next_id;

	if (copy_to_user(u64_to_user_ptr(req.entries_addr), entries,
			 sizeof(*entries) * filled)) {
		kfree(entries);
		return -EFAULT;
	}

	kfree(entries);
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_resolve_syscall(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_stop_state stop;
	struct lkmdbg_syscall_resolve_request req;
	long ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_syscall_resolve_request(&req);
	if (ret)
		return ret;

	mutex_lock(&session->lock);
	stop = session->stop_state;
	if (!(stop.flags & LKMDBG_STOP_FLAG_ACTIVE) ||
	    stop.reason != LKMDBG_STOP_REASON_SYSCALL ||
	    !(stop.flags & LKMDBG_STOP_FLAG_SYSCALL_CONTROL)) {
		mutex_unlock(&session->lock);
		return -ENOENT;
	}
	if (req.stop_cookie != stop.cookie) {
		mutex_unlock(&session->lock);
		return -ESTALE;
	}
	if (!session->syscall_control.active || session->syscall_control.abort) {
		mutex_unlock(&session->lock);
		return -EBUSY;
	}

	req.backend_flags = session->syscall_control.backend_flags;
	if (req.action == LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE &&
	    req.syscall_nr >= 0 &&
	    req.syscall_nr != session->syscall_control.syscall_nr &&
	    !(req.backend_flags &
	      LKMDBG_SYSCALL_RESOLVE_FLAG_NR_REWRITE_SUPPORTED)) {
		mutex_unlock(&session->lock);
		return -EOPNOTSUPP;
	}
	if (req.action != LKMDBG_SYSCALL_RESOLVE_ACTION_REWRITE)
		req.syscall_nr = session->syscall_control.syscall_nr;

	session->syscall_control.action = req.action;
	session->syscall_control.syscall_nr = req.syscall_nr;
	session->syscall_control.retval = req.retval;
	memcpy(session->syscall_control.args, req.args,
	       sizeof(session->syscall_control.args));
	session->syscall_control.resolved = true;
	mutex_unlock(&session->lock);

	wake_up_all(&session->syscall_control.waitq);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}
int lkmdbg_wait_for_stop_state(struct lkmdbg_session *session, u32 reason,
			       u64 value1, u32 timeout_ms,
			       struct lkmdbg_stop_state *stop_out)
{
	unsigned long timeout;
	long waited;
	struct lkmdbg_stop_state stop;

	timeout = timeout_ms ? msecs_to_jiffies(timeout_ms) :
			       msecs_to_jiffies(1000);
	waited = wait_event_timeout(
		session->stop_waitq,
		({
			bool matched;

			mutex_lock(&session->lock);
			stop = session->stop_state;
			matched = lkmdbg_session_stop_matches(&stop, reason, value1);
			mutex_unlock(&session->lock);
			matched;
		}),
		timeout);
	if (!waited)
		return -ETIMEDOUT;

	if (stop_out)
		*stop_out = stop;
	return 0;
}

int lkmdbg_continue_target_internal(struct lkmdbg_session *session,
				    u64 stop_cookie, u32 timeout_ms, u32 flags,
				    struct lkmdbg_continue_request *reply_out)
{
	struct lkmdbg_stop_state stop;
	struct lkmdbg_freeze_request thaw_req;
	long ret = 0;

	lkmdbg_session_zero_stop(&stop);
	memset(&thaw_req, 0, sizeof(thaw_req));

	mutex_lock(&session->lock);
	stop = session->stop_state;
	mutex_unlock(&session->lock);

	if ((stop.flags & LKMDBG_STOP_FLAG_ACTIVE) && stop_cookie &&
	    stop_cookie != stop.cookie)
		return -ESTALE;

	ret = lkmdbg_remote_call_prepare_continue(session, &stop);
	if (ret)
		return ret;

	ret = lkmdbg_session_prepare_continue_syscall_control(session, &stop);
	if (ret) {
		lkmdbg_remote_call_rollback_continue(session, &stop);
		return ret;
	}

	ret = lkmdbg_prepare_continue_hwpoints(session, &stop, flags);
	if (ret) {
		lkmdbg_remote_call_rollback_continue(session, &stop);
		return ret;
	}

	lkmdbg_session_clear_stop(session);
	ret = lkmdbg_session_thaw_target(session, timeout_ms, &thaw_req);
	if (ret && ret != -ENODEV) {
		if (stop.flags & LKMDBG_STOP_FLAG_ACTIVE) {
			mutex_lock(&session->lock);
			session->stop_state = stop;
			mutex_unlock(&session->lock);
			wake_up_all(&session->stop_waitq);
		}
		lkmdbg_remote_call_rollback_continue(session, &stop);
		return ret;
	}
	if (ret == -ENODEV) {
		lkmdbg_remote_call_release(session);
		lkmdbg_session_fail_syscall_control(session);
	}

	ret = lkmdbg_remote_call_finish_continue(session, &stop);
	if (ret)
		return ret;

	lkmdbg_session_finish_continue_syscall_control(session, &stop);

	if (reply_out) {
		memset(reply_out, 0, sizeof(*reply_out));
		reply_out->version = LKMDBG_PROTO_VERSION;
		reply_out->size = sizeof(*reply_out);
		reply_out->flags = flags;
		reply_out->timeout_ms = timeout_ms;
		reply_out->stop_cookie = stop_cookie;
		reply_out->threads_total = thaw_req.threads_total;
		reply_out->threads_settled = thaw_req.threads_settled;
		reply_out->threads_parked = thaw_req.threads_parked;
	}

	return 0;
}

long lkmdbg_continue_target(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_continue_request req;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_continue_request(&req);
	if (ret)
		return ret;

	ret = lkmdbg_continue_target_internal(session, req.stop_cookie,
					      req.timeout_ms, req.flags, &req);
	if (ret)
		return ret;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

#if 0
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
		old_calls = !!(session->stop_state.flags & LKMDBG_STOP_FLAG_ACTIVE) ||
			    session->syscall_control.active;
		mutex_unlock(&session->lock);
		if (old_calls)
			return -EBUSY;
		if (lkmdbg_remote_call_blocks_target_change(session))
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
	INIT_LIST_HEAD(&session->input_channels);
	INIT_LIST_HEAD(&session->syscall_rules);
	INIT_WORK(&session->stop_work, lkmdbg_session_stop_workfn);
	atomic_set(&session->async_refs, 0);
	session->owner_tgid = current->tgid;
	session->syscall_trace_nr = -1;
	session->syscall_rule_mode = LKMDBG_SYSCALL_RULE_MODE_ENFORCE;
	session->syscall_rule_event_policy = LKMDBG_SYSCALL_RULE_EVENT_RAW_AND_RULE;
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
#endif
