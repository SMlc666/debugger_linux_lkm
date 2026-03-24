#include <linux/errno.h>
#include <linux/hw_breakpoint.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

#define LKMDBG_TASK_WORK_NOTIFY_RESUME 1U

typedef int (*lkmdbg_task_work_add_fn)(struct task_struct *task,
				       struct callback_head *work,
				       unsigned int notify);
typedef void (*lkmdbg_perf_event_disable_local_fn)(struct perf_event *event);

static void lkmdbg_regs_arm64_export(struct lkmdbg_regs_arm64 *dst,
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

static void lkmdbg_regs_arm64_import(struct pt_regs *dst,
				     const struct lkmdbg_regs_arm64 *src)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(src->regs); i++)
		dst->regs[i] = src->regs[i];
	dst->sp = src->sp;
	dst->pc = src->pc;
	dst->pstate = src->pstate;
}

static void lkmdbg_remote_call_reset_locked(struct lkmdbg_session *session)
{
	struct lkmdbg_remote_call_state *state = &session->remote_call;

	memset(state, 0, sizeof(*state));
	state->session = session;
}

static void lkmdbg_remote_call_reset_if_idle(struct lkmdbg_session *session)
{
	mutex_lock(&session->lock);
	if (!session->remote_call.return_event &&
	    !session->remote_call.park_work_queued &&
	    session->remote_call.phase == LKMDBG_REMOTE_CALL_NONE)
		lkmdbg_remote_call_reset_locked(session);
	mutex_unlock(&session->lock);
}

static bool lkmdbg_remote_call_phase_active(u32 phase)
{
	return phase == LKMDBG_REMOTE_CALL_PREPARED ||
	       phase == LKMDBG_REMOTE_CALL_RUNNING ||
	       phase == LKMDBG_REMOTE_CALL_PARKED;
}

static void lkmdbg_remote_call_wait_park_quiesced(
	struct lkmdbg_session *session)
{
	long timeout;

	timeout = wait_event_timeout(
		session->remote_call_waitq,
		!READ_ONCE(session->remote_call.park_work_queued),
		msecs_to_jiffies(1000));
	if (!timeout)
		pr_warn("lkmdbg: remote call park work did not quiesce in time\n");
}

static long lkmdbg_remote_call_copy_reply(void __user *argp,
					  struct lkmdbg_remote_call_request *req,
					  long ret)
{
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

static int lkmdbg_validate_remote_call_request(
	struct lkmdbg_remote_call_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (req->tid <= 0 || !req->target_pc || req->arg_count > ARRAY_SIZE(req->args))
		return -EINVAL;

	if (req->target_pc >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	return 0;
}

static void lkmdbg_remote_call_park_work(struct callback_head *work)
{
	struct lkmdbg_remote_call_state *state =
		container_of(work, struct lkmdbg_remote_call_state, park_work);
	struct lkmdbg_session *session = state->session;

	WRITE_ONCE(state->parked, true);
	wake_up_all(&session->remote_call_waitq);

	wait_event(session->remote_call_waitq,
		   READ_ONCE(state->resume) || READ_ONCE(session->closing) ||
			   READ_ONCE(state->phase) != LKMDBG_REMOTE_CALL_PARKED);

	WRITE_ONCE(state->parked, false);
	WRITE_ONCE(state->park_work_queued, false);
	wake_up_all(&session->remote_call_waitq);
}

static int lkmdbg_remote_call_queue_park(struct lkmdbg_session *session)
{
	lkmdbg_task_work_add_fn add_fn;

	add_fn = (lkmdbg_task_work_add_fn)lkmdbg_symbols.task_work_add_sym;
	if (!add_fn)
		return -EOPNOTSUPP;

	init_task_work(&session->remote_call.park_work,
		       lkmdbg_remote_call_park_work);
	return add_fn(current, &session->remote_call.park_work,
		      LKMDBG_TASK_WORK_NOTIFY_RESUME);
}

static void lkmdbg_remote_call_disable_breakpoint(struct perf_event *event)
{
	lkmdbg_perf_event_disable_local_fn disable_fn;

	if (!event)
		return;

	disable_fn = (lkmdbg_perf_event_disable_local_fn)
		lkmdbg_symbols.perf_event_disable_local_sym;
	if (disable_fn)
		disable_fn(event);
}

static void lkmdbg_remote_call_breakpoint(struct perf_event *bp,
					  struct perf_sample_data *data,
					  struct pt_regs *regs)
{
	struct lkmdbg_session *session;
	struct lkmdbg_remote_call_state *state;
	struct lkmdbg_regs_arm64 stop_regs;
	u64 return_value;
	u64 call_id;
	pid_t tgid;
	pid_t tid;
	int ret;

	(void)data;

	session = bp ? bp->overflow_handler_context : NULL;
	if (!session || !regs)
		return;

	state = &session->remote_call;
	lkmdbg_remote_call_disable_breakpoint(bp);

	mutex_lock(&session->lock);
	if (state->phase != LKMDBG_REMOTE_CALL_RUNNING ||
	    state->return_event != bp || state->tgid != current->tgid ||
	    state->tid != current->pid) {
		mutex_unlock(&session->lock);
		return;
	}

	return_value = regs->regs[0];
	stop_regs = state->saved_regs;
	call_id = state->call_id;
	tgid = state->tgid;
	tid = state->tid;
	state->return_value = return_value;
	state->phase = LKMDBG_REMOTE_CALL_PARKED;
	state->resume = false;
	state->parked = false;
	state->park_work_queued = true;
	mutex_unlock(&session->lock);

	lkmdbg_regs_arm64_import(regs, &stop_regs);

	ret = lkmdbg_remote_call_queue_park(session);
	if (ret) {
		mutex_lock(&session->lock);
		state->park_work_queued = false;
		mutex_unlock(&session->lock);
		wake_up_all(&session->remote_call_waitq);
		pr_warn("lkmdbg: remote call park queue failed tid=%d ret=%d\n",
			tid, ret);
	}

	lkmdbg_session_request_async_stop(session, LKMDBG_STOP_REASON_REMOTE_CALL,
					  tgid, tid, 0, 0, return_value, call_id,
					  &stop_regs);
}

static void lkmdbg_remote_call_restore_prepared_regs(
	const struct lkmdbg_remote_call_state *snapshot)
{
	struct task_struct *task;
	struct pt_regs *regs;

	if (!snapshot || snapshot->phase != LKMDBG_REMOTE_CALL_PREPARED ||
	    snapshot->tid <= 0)
		return;

	task = get_pid_task(find_vpid(snapshot->tid), PIDTYPE_PID);
	if (!task)
		return;

	if (task->tgid != snapshot->tgid) {
		put_task_struct(task);
		return;
	}

	regs = task_pt_regs(task);
	if (regs)
		lkmdbg_regs_arm64_import(regs, &snapshot->saved_regs);
	put_task_struct(task);
}

static void lkmdbg_remote_call_disarm_snapshot(
	struct lkmdbg_remote_call_state *snapshot)
{
	if (!snapshot || !snapshot->return_event)
		return;

	unregister_hw_breakpoint(snapshot->return_event);
	snapshot->return_event = NULL;
}

static int lkmdbg_remote_call_arm_breakpoint(struct lkmdbg_session *session,
					     struct task_struct *task,
					     struct perf_event **event_out)
{
	struct perf_event_attr attr;
	struct perf_event *event;
	int ret;

	ptrace_breakpoint_init(&attr);
	attr.bp_addr = session->remote_call.return_pc;
	attr.bp_len = HW_BREAKPOINT_LEN_4;
	attr.bp_type = HW_BREAKPOINT_X;
	attr.disabled = 1;

	event = register_user_hw_breakpoint(&attr,
					    lkmdbg_remote_call_breakpoint,
					    session, task);
	if (!event) {
		*event_out = NULL;
		return -EOPNOTSUPP;
	}
	if (IS_ERR(event))
		return PTR_ERR(event);

	attr.disabled = 0;
	ret = modify_user_hw_breakpoint(event, &attr);
	if (ret) {
		unregister_hw_breakpoint(event);
		return ret;
	}

	*event_out = event;
	return 0;
}

long lkmdbg_remote_call(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_remote_call_request req;
	struct lkmdbg_remote_call_state snapshot;
	struct lkmdbg_stop_state stop;
	struct task_struct *task = NULL;
	struct pt_regs *regs;
	struct perf_event *event = NULL;
	struct lkmdbg_regs_arm64 prepared;
	u32 freeze_flags;
	long ret;
	u32 i;

#ifndef CONFIG_ARM64
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	return lkmdbg_remote_call_copy_reply(argp, &req, -EOPNOTSUPP);
#else
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_remote_call_request(&req);
	if (ret)
		return ret;

	if (!lkmdbg_symbols.task_work_add_sym ||
	    !lkmdbg_symbols.perf_event_disable_local_sym)
		return lkmdbg_remote_call_copy_reply(argp, &req, -EOPNOTSUPP);

	ret = lkmdbg_get_target_thread(session, req.tid, &task);
	if (ret)
		return lkmdbg_remote_call_copy_reply(argp, &req, ret);

	freeze_flags = lkmdbg_freeze_thread_flags(session, task->pid);
	if (!(freeze_flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED)) {
		put_task_struct(task);
		return lkmdbg_remote_call_copy_reply(argp, &req, -EBUSY);
	}

	mutex_lock(&session->lock);
	stop = session->stop_state;
	if (!stop.reason || !(stop.flags & LKMDBG_STOP_FLAG_ACTIVE) ||
	    !(stop.flags & LKMDBG_STOP_FLAG_FROZEN) ||
	    lkmdbg_remote_call_phase_active(session->remote_call.phase) ||
	    session->step_armed) {
		mutex_unlock(&session->lock);
		put_task_struct(task);
		return lkmdbg_remote_call_copy_reply(argp, &req, -EBUSY);
	}

	regs = task_pt_regs(task);
	if (!regs) {
		mutex_unlock(&session->lock);
		put_task_struct(task);
		return lkmdbg_remote_call_copy_reply(argp, &req, -ESRCH);
	}

	lkmdbg_regs_arm64_export(&session->remote_call.saved_regs, regs);
	session->next_remote_call_id++;
	session->remote_call.call_id = session->next_remote_call_id;
	session->remote_call.tgid = task->tgid;
	session->remote_call.tid = task->pid;
	session->remote_call.target_pc = req.target_pc;
	session->remote_call.return_pc = session->remote_call.saved_regs.pc;
	session->remote_call.phase = LKMDBG_REMOTE_CALL_PREPARED;
	session->remote_call.resume = false;
	session->remote_call.parked = false;
	session->remote_call.park_work_queued = false;
	req.call_id = session->remote_call.call_id;
	req.tid = task->pid;
	req.return_pc = session->remote_call.return_pc;
	mutex_unlock(&session->lock);

	ret = lkmdbg_remote_call_arm_breakpoint(session, task, &event);
	if (ret)
		goto out_restore;

	mutex_lock(&session->lock);
	session->remote_call.return_event = event;
	prepared = session->remote_call.saved_regs;
	mutex_unlock(&session->lock);

	for (i = 0; i < req.arg_count; i++)
		prepared.regs[i] = req.args[i];
	prepared.pc = req.target_pc;
	prepared.regs[30] = req.return_pc;

	regs = task_pt_regs(task);
	if (!regs) {
		ret = -ESRCH;
		goto out_abort;
	}
	lkmdbg_regs_arm64_import(regs, &prepared);
	put_task_struct(task);
	return lkmdbg_remote_call_copy_reply(argp, &req, 0);

out_abort:
	mutex_lock(&session->lock);
	snapshot = session->remote_call;
	lkmdbg_remote_call_reset_locked(session);
	mutex_unlock(&session->lock);
	lkmdbg_remote_call_disarm_snapshot(&snapshot);
	goto out_reply;

out_restore:
	mutex_lock(&session->lock);
	snapshot = session->remote_call;
	lkmdbg_remote_call_reset_locked(session);
	mutex_unlock(&session->lock);
	lkmdbg_remote_call_restore_prepared_regs(&snapshot);

out_reply:
	put_task_struct(task);
	return lkmdbg_remote_call_copy_reply(argp, &req, ret);
#endif
}

u32 lkmdbg_remote_call_thread_flags(struct lkmdbg_session *session, pid_t tid)
{
	struct lkmdbg_remote_call_state snapshot;

	if (!session || tid <= 0)
		return 0;

	mutex_lock(&session->lock);
	snapshot = session->remote_call;
	mutex_unlock(&session->lock);

	if (snapshot.phase != LKMDBG_REMOTE_CALL_PARKED || snapshot.tid != tid)
		return 0;

	return LKMDBG_THREAD_FLAG_FREEZE_TRACKED |
	       LKMDBG_THREAD_FLAG_FREEZE_SETTLED |
	       LKMDBG_THREAD_FLAG_FREEZE_PARKED;
}

int lkmdbg_remote_call_prepare_continue(struct lkmdbg_session *session,
					const struct lkmdbg_stop_state *stop)
{
	int ret = 0;

	mutex_lock(&session->lock);
	switch (session->remote_call.phase) {
	case LKMDBG_REMOTE_CALL_NONE:
		break;
	case LKMDBG_REMOTE_CALL_PREPARED:
		if (!stop || !(stop->flags & LKMDBG_STOP_FLAG_ACTIVE) ||
		    !(stop->flags & LKMDBG_STOP_FLAG_FROZEN))
			ret = -EBUSY;
		else
			session->remote_call.phase = LKMDBG_REMOTE_CALL_RUNNING;
		break;
	case LKMDBG_REMOTE_CALL_PARKED:
		if (!stop || stop->reason != LKMDBG_STOP_REASON_REMOTE_CALL ||
		    !(stop->flags & LKMDBG_STOP_FLAG_ACTIVE) ||
		    !(stop->flags & LKMDBG_STOP_FLAG_FROZEN))
			ret = -EBUSY;
		break;
	default:
		ret = -EBUSY;
		break;
	}
	mutex_unlock(&session->lock);

	return ret;
}

void lkmdbg_remote_call_rollback_continue(
	struct lkmdbg_session *session, const struct lkmdbg_stop_state *stop)
{
	mutex_lock(&session->lock);
	if (session->remote_call.phase == LKMDBG_REMOTE_CALL_RUNNING &&
	    stop && stop->reason != LKMDBG_STOP_REASON_REMOTE_CALL)
		session->remote_call.phase = LKMDBG_REMOTE_CALL_PREPARED;
	mutex_unlock(&session->lock);
}

int lkmdbg_remote_call_finish_continue(struct lkmdbg_session *session,
				       const struct lkmdbg_stop_state *stop)
{
	struct perf_event *event = NULL;
	bool wake_parked = false;

	mutex_lock(&session->lock);
	if (stop && stop->reason == LKMDBG_STOP_REASON_REMOTE_CALL &&
	    session->remote_call.phase == LKMDBG_REMOTE_CALL_PARKED) {
		event = session->remote_call.return_event;
		session->remote_call.return_event = NULL;
		session->remote_call.resume = true;
		session->remote_call.phase = LKMDBG_REMOTE_CALL_NONE;
		wake_parked = true;
	}
	mutex_unlock(&session->lock);

	if (event)
		unregister_hw_breakpoint(event);
	if (wake_parked) {
		wake_up_all(&session->remote_call_waitq);
		lkmdbg_remote_call_wait_park_quiesced(session);
	}
	lkmdbg_remote_call_reset_if_idle(session);

	return 0;
}

bool lkmdbg_remote_call_blocks_target_change(struct lkmdbg_session *session)
{
	u32 phase;

	mutex_lock(&session->lock);
	phase = session->remote_call.phase;
	mutex_unlock(&session->lock);

	return lkmdbg_remote_call_phase_active(phase);
}

bool lkmdbg_remote_call_blocks_manual_thaw(struct lkmdbg_session *session)
{
	u32 phase;

	mutex_lock(&session->lock);
	phase = session->remote_call.phase;
	mutex_unlock(&session->lock);

	return phase == LKMDBG_REMOTE_CALL_PREPARED ||
	       phase == LKMDBG_REMOTE_CALL_PARKED;
}

void lkmdbg_remote_call_fail_stop(struct lkmdbg_session *session,
				  const struct lkmdbg_stop_state *stop)
{
	struct perf_event *event = NULL;
	bool wait_park = false;

	if (!stop || stop->reason != LKMDBG_STOP_REASON_REMOTE_CALL)
		return;

	mutex_lock(&session->lock);
	event = session->remote_call.return_event;
	session->remote_call.return_event = NULL;
	if (session->remote_call.phase == LKMDBG_REMOTE_CALL_PARKED) {
		session->remote_call.resume = true;
		wait_park = session->remote_call.park_work_queued;
	}
	session->remote_call.phase = LKMDBG_REMOTE_CALL_NONE;
	mutex_unlock(&session->lock);

	if (event)
		unregister_hw_breakpoint(event);
	if (wait_park) {
		wake_up_all(&session->remote_call_waitq);
		lkmdbg_remote_call_wait_park_quiesced(session);
	}
	lkmdbg_remote_call_reset_if_idle(session);
}

void lkmdbg_remote_call_release(struct lkmdbg_session *session)
{
	struct lkmdbg_remote_call_state snapshot;
	bool wait_park = false;

	if (!session)
		return;

	mutex_lock(&session->lock);
	snapshot = session->remote_call;
	if (snapshot.park_work_queued) {
		session->remote_call.resume = true;
		wait_park = true;
	}
	session->remote_call.return_event = NULL;
	session->remote_call.phase = LKMDBG_REMOTE_CALL_NONE;
	mutex_unlock(&session->lock);

	lkmdbg_remote_call_disarm_snapshot(&snapshot);
	lkmdbg_remote_call_restore_prepared_regs(&snapshot);
	if (wait_park) {
		wake_up_all(&session->remote_call_waitq);
		lkmdbg_remote_call_wait_park_quiesced(session);
	}
	lkmdbg_remote_call_reset_if_idle(session);
}
