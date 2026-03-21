#include <linux/errno.h>
#include <linux/hw_breakpoint.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/perf_event.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>

#ifdef CONFIG_ARM64
#include <asm/debug-monitors.h>
#endif

#include "lkmdbg_internal.h"

#define LKMDBG_HWPOINT_MAX_ENTRIES 64U

struct linux_binprm;
struct kernel_siginfo;

struct lkmdbg_hwpoint {
	struct list_head node;
	struct lkmdbg_session *session;
	struct perf_event *event;
	u64 id;
	u64 addr;
	pid_t tgid;
	pid_t tid;
	u32 type;
	u32 len;
	u32 flags;
};

#ifdef CONFIG_ARM64
typedef void (*lkmdbg_register_user_step_hook_fn)(struct step_hook *hook);
typedef void (*lkmdbg_unregister_user_step_hook_fn)(struct step_hook *hook);
typedef void (*lkmdbg_user_single_step_fn)(struct task_struct *task);
#endif
typedef void (*lkmdbg_for_each_kernel_tracepoint_fn)(
	void (*fct)(struct tracepoint *tp, void *priv), void *priv);
typedef int (*lkmdbg_tracepoint_probe_register_fn)(struct tracepoint *tp,
						   void *probe, void *data);
typedef int (*lkmdbg_tracepoint_probe_unregister_fn)(struct tracepoint *tp,
						     void *probe, void *data);

static bool lkmdbg_trace_fork_registered;
static bool lkmdbg_trace_exec_registered;
static bool lkmdbg_trace_exit_registered;
static bool lkmdbg_trace_signal_registered;
static struct tracepoint *lkmdbg_trace_fork_tp;
static struct tracepoint *lkmdbg_trace_exec_tp;
static struct tracepoint *lkmdbg_trace_exit_tp;
static struct tracepoint *lkmdbg_trace_signal_tp;

#ifdef CONFIG_ARM64
static int lkmdbg_user_step_handler(struct pt_regs *regs, unsigned long esr);

static struct step_hook lkmdbg_user_step_hook = {
	.fn = lkmdbg_user_step_handler,
};
static bool lkmdbg_user_step_hook_registered;
#endif

static bool lkmdbg_hwpoint_type_valid(u32 type)
{
	switch (type) {
	case LKMDBG_HWPOINT_TYPE_READ:
	case LKMDBG_HWPOINT_TYPE_WRITE:
	case LKMDBG_HWPOINT_TYPE_READWRITE:
	case LKMDBG_HWPOINT_TYPE_EXEC:
		return true;
	default:
		return false;
	}
}

static bool lkmdbg_hwpoint_len_valid(u32 len)
{
	return len >= 1 && len <= 8;
}

static int lkmdbg_validate_hwpoint_request(struct lkmdbg_hwpoint_request *req,
					   bool remove)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (remove)
		return req->id ? 0 : -EINVAL;

	if (!req->addr || req->addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (!lkmdbg_hwpoint_type_valid(req->type) ||
	    !lkmdbg_hwpoint_len_valid(req->len))
		return -EINVAL;

	if (req->tid < 0)
		return -EINVAL;

	return 0;
}

static int lkmdbg_validate_hwpoint_query(
	struct lkmdbg_hwpoint_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_HWPOINT_MAX_ENTRIES)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	return 0;
}

static int lkmdbg_validate_single_step_request(
	struct lkmdbg_single_step_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (req->tid < 0)
		return -EINVAL;

	return 0;
}

static struct lkmdbg_hwpoint *
lkmdbg_find_hwpoint_locked(struct lkmdbg_session *session, u64 id)
{
	struct lkmdbg_hwpoint *entry;

	list_for_each_entry(entry, &session->hwpoints, node) {
		if (entry->id == id)
			return entry;
	}

	return NULL;
}

static long lkmdbg_hwpoint_copy_reply(void __user *argp,
				      struct lkmdbg_hwpoint_request *req,
				      long ret)
{
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

static long lkmdbg_hwpoint_query_copy_reply(
	void __user *argp, struct lkmdbg_hwpoint_query_request *req,
	struct lkmdbg_hwpoint_entry *entries, size_t entries_bytes)
{
	if (copy_to_user(u64_to_user_ptr(req->entries_addr), entries,
			 entries_bytes))
		return -EFAULT;

	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return 0;
}

static long lkmdbg_single_step_copy_reply(void __user *argp,
					  struct lkmdbg_single_step_request *req,
					  long ret)
{
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

static void lkmdbg_hwpoint_fill_entry(struct lkmdbg_hwpoint_entry *dst,
				      const struct lkmdbg_hwpoint *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->id = src->id;
	dst->addr = src->addr;
	dst->tgid = src->tgid;
	dst->tid = src->tid;
	dst->type = src->type;
	dst->len = src->len;
	dst->flags = src->flags;
}

static void lkmdbg_hwpoint_event(struct perf_event *bp,
				 struct perf_sample_data *data,
				 struct pt_regs *regs)
{
	struct lkmdbg_hwpoint *entry = bp->overflow_handler_context;
	u32 reason;
	u64 addr;
	u64 ip = 0;

	(void)data;

	if (!entry || !entry->session)
		return;

	reason = entry->type == LKMDBG_HWPOINT_TYPE_EXEC ?
		 LKMDBG_STOP_REASON_BREAKPOINT :
		 LKMDBG_STOP_REASON_WATCHPOINT;
	addr = hw_breakpoint_addr(bp);
	if (regs)
		ip = instruction_pointer(regs);

	atomic64_inc(&lkmdbg_state.hwpoint_callback_total);
	if (reason == LKMDBG_STOP_REASON_BREAKPOINT)
		atomic64_inc(&lkmdbg_state.breakpoint_callback_total);
	else if (reason == LKMDBG_STOP_REASON_WATCHPOINT)
		atomic64_inc(&lkmdbg_state.watchpoint_callback_total);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tgid, entry->tgid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tid, current->pid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_reason, reason);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_type, entry->type);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_addr, addr);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_ip, ip);

	lkmdbg_session_queue_event_ex(entry->session, LKMDBG_EVENT_TARGET_STOP,
				      reason, entry->tgid, current->pid,
				      entry->type, addr, ip ? ip : entry->id);
}

static int lkmdbg_register_hwpoint(struct lkmdbg_session *session,
				   struct lkmdbg_hwpoint_request *req)
{
	struct perf_event_attr attr;
	struct lkmdbg_hwpoint *entry;
	struct task_struct *task = NULL;
	u64 id;
	int ret;

	ret = lkmdbg_get_target_thread(session, req->tid, &task);
	if (ret)
		return ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		put_task_struct(task);
		return -ENOMEM;
	}

	ptrace_breakpoint_init(&attr);
	attr.bp_addr = req->addr;
	attr.bp_len = req->len;
	attr.bp_type = req->type;
	attr.disabled = 1;

	mutex_lock(&session->lock);
	session->next_hwpoint_id++;
	id = session->next_hwpoint_id;
	mutex_unlock(&session->lock);

	entry->event = register_user_hw_breakpoint(&attr, lkmdbg_hwpoint_event,
						     entry, task);
	entry->session = session;
	entry->id = id;
	entry->addr = req->addr;
	entry->tgid = task->tgid;
	entry->tid = task->pid;
	entry->type = req->type;
	entry->len = req->len;
	entry->flags = req->flags;
	put_task_struct(task);

	if (!entry->event) {
		kfree(entry);
		return -EOPNOTSUPP;
	}

	if (IS_ERR(entry->event)) {
		ret = PTR_ERR(entry->event);
		kfree(entry);
		return ret;
	}

	attr.disabled = 0;
	ret = modify_user_hw_breakpoint(entry->event, &attr);
	if (ret) {
		unregister_hw_breakpoint(entry->event);
		kfree(entry);
		return ret;
	}

	mutex_lock(&session->lock);
	list_add_tail(&entry->node, &session->hwpoints);
	mutex_unlock(&session->lock);

	req->id = entry->id;
	req->tid = entry->tid;
	return 0;
}

static int lkmdbg_unregister_hwpoint(struct lkmdbg_session *session, u64 id)
{
	struct lkmdbg_hwpoint *entry;

	mutex_lock(&session->lock);
	entry = lkmdbg_find_hwpoint_locked(session, id);
	if (!entry) {
		mutex_unlock(&session->lock);
		return -ENOENT;
	}

	list_del_init(&entry->node);
	mutex_unlock(&session->lock);

	unregister_hw_breakpoint(entry->event);
	kfree(entry);
	return 0;
}

static void lkmdbg_trace_sched_process_fork(void *data,
					    struct task_struct *parent,
					    struct task_struct *child)
{
	(void)data;

	if (!parent || !child)
		return;

	if (parent->tgid == child->tgid) {
		lkmdbg_session_broadcast_target_event(
			child->tgid, LKMDBG_EVENT_TARGET_CLONE,
			LKMDBG_TARGET_CLONE_THREAD, child->pid, 0,
			child->pid, parent->pid);
		return;
	}

	lkmdbg_session_broadcast_target_event(parent->tgid,
					      LKMDBG_EVENT_TARGET_CLONE,
					      LKMDBG_TARGET_CLONE_PROCESS,
					      child->pid, 0, child->tgid,
					      parent->pid);
}

static void lkmdbg_trace_sched_process_exec(void *data, struct task_struct *p,
					    pid_t old_pid,
					    struct linux_binprm *bprm)
{
	(void)data;
	(void)bprm;

	if (!p)
		return;

	lkmdbg_session_broadcast_target_event(p->tgid, LKMDBG_EVENT_TARGET_EXEC,
					      0, p->pid, 0, old_pid, 0);
}

static void lkmdbg_trace_sched_process_exit(void *data, struct task_struct *p)
{
	(void)data;

	if (!p)
		return;

	lkmdbg_session_broadcast_target_event(p->tgid, LKMDBG_EVENT_TARGET_EXIT,
					      0, p->pid, 0,
					      (u64)(unsigned int)p->exit_code,
					      0);
}

static void lkmdbg_trace_signal_generate(void *data, int sig,
					 struct kernel_siginfo *info,
					 struct task_struct *task, int group,
					 int result)
{
	u64 siginfo_code = 0;

	(void)data;

	if (!task)
		return;

	if (info && info != SEND_SIG_NOINFO && info != SEND_SIG_PRIV)
		siginfo_code = (u64)(u32)info->si_code;

	lkmdbg_session_broadcast_target_event(
		task->tgid, LKMDBG_EVENT_TARGET_SIGNAL, sig, task->pid,
		group ? LKMDBG_SIGNAL_EVENT_GROUP : 0, siginfo_code, result);
}

struct lkmdbg_tracepoint_lookup {
	const char *name;
	struct tracepoint *match;
};

static void lkmdbg_tracepoint_find_cb(struct tracepoint *tp, void *priv)
{
	struct lkmdbg_tracepoint_lookup *lookup = priv;

	if (lookup->match)
		return;
	if (strcmp(tp->name, lookup->name) == 0)
		lookup->match = tp;
}

static struct tracepoint *lkmdbg_find_tracepoint(const char *name)
{
	struct lkmdbg_tracepoint_lookup lookup = {
		.name = name,
	};
	lkmdbg_for_each_kernel_tracepoint_fn for_each_fn;

	for_each_fn = (lkmdbg_for_each_kernel_tracepoint_fn)
		lkmdbg_symbols.for_each_kernel_tracepoint_sym;
	if (!for_each_fn)
		return NULL;

	for_each_fn(lkmdbg_tracepoint_find_cb, &lookup);
	return lookup.match;
}

#ifdef CONFIG_ARM64
static int lkmdbg_user_step_handler(struct pt_regs *regs, unsigned long esr)
{
	struct lkmdbg_session *matched;
	pid_t tid = current->pid;
	pid_t tgid = current->tgid;
	lkmdbg_user_single_step_fn disable_fn;

	(void)esr;

	matched = lkmdbg_session_consume_single_step(tgid, tid);
	if (!matched)
		return DBG_HOOK_ERROR;

	disable_fn =
		(lkmdbg_user_single_step_fn)lkmdbg_symbols.user_disable_single_step_sym;
	if (disable_fn)
		disable_fn(current);

	lkmdbg_session_queue_event_ex(matched, LKMDBG_EVENT_TARGET_STOP,
				      LKMDBG_STOP_REASON_SINGLE_STEP, tgid, tid,
				      0, instruction_pointer(regs), 0);
	lkmdbg_session_async_put(matched);
	return DBG_HOOK_HANDLED;
}
#endif

static int lkmdbg_register_trace_hooks(void)
{
	lkmdbg_tracepoint_probe_register_fn register_fn;
	int ret;

	register_fn = (lkmdbg_tracepoint_probe_register_fn)
		lkmdbg_symbols.tracepoint_probe_register_sym;
	if (!register_fn) {
		pr_info("lkmdbg: tracepoint register helper unavailable\n");
		return 0;
	}

	lkmdbg_trace_fork_tp = lkmdbg_find_tracepoint("sched_process_fork");
	if (lkmdbg_trace_fork_tp) {
		ret = register_fn(lkmdbg_trace_fork_tp,
				  (void *)lkmdbg_trace_sched_process_fork, NULL);
		if (!ret)
			lkmdbg_trace_fork_registered = true;
		else
			pr_warn("lkmdbg: sched_process_fork trace hook failed ret=%d\n",
				ret);
	} else {
		pr_info("lkmdbg: sched_process_fork tracepoint unavailable\n");
	}

	lkmdbg_trace_exec_tp = lkmdbg_find_tracepoint("sched_process_exec");
	if (lkmdbg_trace_exec_tp) {
		ret = register_fn(lkmdbg_trace_exec_tp,
				  (void *)lkmdbg_trace_sched_process_exec, NULL);
		if (!ret)
			lkmdbg_trace_exec_registered = true;
		else
			pr_warn("lkmdbg: sched_process_exec trace hook failed ret=%d\n",
				ret);
	} else {
		pr_info("lkmdbg: sched_process_exec tracepoint unavailable\n");
	}

	lkmdbg_trace_exit_tp = lkmdbg_find_tracepoint("sched_process_exit");
	if (lkmdbg_trace_exit_tp) {
		ret = register_fn(lkmdbg_trace_exit_tp,
				  (void *)lkmdbg_trace_sched_process_exit, NULL);
		if (!ret)
			lkmdbg_trace_exit_registered = true;
		else
			pr_warn("lkmdbg: sched_process_exit trace hook failed ret=%d\n",
				ret);
	} else {
		pr_info("lkmdbg: sched_process_exit tracepoint unavailable\n");
	}

	lkmdbg_trace_signal_tp = lkmdbg_find_tracepoint("signal_generate");
	if (lkmdbg_trace_signal_tp) {
		ret = register_fn(lkmdbg_trace_signal_tp,
				  (void *)lkmdbg_trace_signal_generate, NULL);
		if (!ret)
			lkmdbg_trace_signal_registered = true;
		else
			pr_warn("lkmdbg: signal_generate trace hook failed ret=%d\n",
				ret);
	} else {
		pr_info("lkmdbg: signal_generate tracepoint unavailable\n");
	}

	return 0;
}

int lkmdbg_thread_ctrl_init(void)
{
#ifdef CONFIG_ARM64
	if (lkmdbg_symbols.register_user_step_hook_sym &&
	    lkmdbg_symbols.unregister_user_step_hook_sym &&
	    lkmdbg_symbols.user_enable_single_step_sym &&
	    lkmdbg_symbols.user_disable_single_step_sym) {
		((lkmdbg_register_user_step_hook_fn)
			 lkmdbg_symbols.register_user_step_hook_sym)(
			&lkmdbg_user_step_hook);
		lkmdbg_user_step_hook_registered = true;
	}
#endif

	return lkmdbg_register_trace_hooks();
}

void lkmdbg_thread_ctrl_exit(void)
{
	lkmdbg_tracepoint_probe_unregister_fn unregister_fn;

	unregister_fn = (lkmdbg_tracepoint_probe_unregister_fn)
		lkmdbg_symbols.tracepoint_probe_unregister_sym;

	if (unregister_fn && lkmdbg_trace_signal_registered &&
	    lkmdbg_trace_signal_tp) {
		unregister_fn(lkmdbg_trace_signal_tp,
			      (void *)lkmdbg_trace_signal_generate, NULL);
		lkmdbg_trace_signal_registered = false;
	}
	lkmdbg_trace_signal_tp = NULL;

	if (unregister_fn && lkmdbg_trace_exit_registered && lkmdbg_trace_exit_tp) {
		unregister_fn(lkmdbg_trace_exit_tp,
			      (void *)lkmdbg_trace_sched_process_exit, NULL);
		lkmdbg_trace_exit_registered = false;
	}
	lkmdbg_trace_exit_tp = NULL;

	if (unregister_fn && lkmdbg_trace_exec_registered && lkmdbg_trace_exec_tp) {
		unregister_fn(lkmdbg_trace_exec_tp,
			      (void *)lkmdbg_trace_sched_process_exec, NULL);
		lkmdbg_trace_exec_registered = false;
	}
	lkmdbg_trace_exec_tp = NULL;

	if (unregister_fn && lkmdbg_trace_fork_registered && lkmdbg_trace_fork_tp) {
		unregister_fn(lkmdbg_trace_fork_tp,
			      (void *)lkmdbg_trace_sched_process_fork, NULL);
		lkmdbg_trace_fork_registered = false;
	}
	lkmdbg_trace_fork_tp = NULL;

#ifdef CONFIG_ARM64
	if (lkmdbg_user_step_hook_registered) {
		((lkmdbg_unregister_user_step_hook_fn)
			 lkmdbg_symbols.unregister_user_step_hook_sym)(
			&lkmdbg_user_step_hook);
		lkmdbg_user_step_hook_registered = false;
	}
#endif
}

void lkmdbg_thread_ctrl_release(struct lkmdbg_session *session)
{
	struct lkmdbg_hwpoint *entry;
	struct lkmdbg_hwpoint *tmp;
#ifdef CONFIG_ARM64
	lkmdbg_user_single_step_fn disable_fn;
	pid_t step_tid;
#endif

	if (!session)
		return;

	mutex_lock(&session->lock);
#ifdef CONFIG_ARM64
	step_tid = session->step_tid;
	WRITE_ONCE(session->step_armed, false);
	WRITE_ONCE(session->step_tgid, 0);
	WRITE_ONCE(session->step_tid, 0);
#endif
	list_for_each_entry_safe(entry, tmp, &session->hwpoints, node) {
		list_del_init(&entry->node);
		mutex_unlock(&session->lock);
		unregister_hw_breakpoint(entry->event);
		kfree(entry);
		mutex_lock(&session->lock);
	}
	mutex_unlock(&session->lock);

#ifdef CONFIG_ARM64
	if (!step_tid || !lkmdbg_symbols.user_disable_single_step_sym)
		return;

	disable_fn =
		(lkmdbg_user_single_step_fn)lkmdbg_symbols.user_disable_single_step_sym;
	if (disable_fn) {
		struct task_struct *task;

		task = get_pid_task(find_vpid(step_tid), PIDTYPE_PID);
		if (task) {
			disable_fn(task);
			put_task_struct(task);
		}
	}
#endif
}

long lkmdbg_add_hwpoint(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_hwpoint_request req;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_hwpoint_request(&req, false);
	if (ret)
		return ret;

	ret = lkmdbg_register_hwpoint(session, &req);
	return lkmdbg_hwpoint_copy_reply(argp, &req, ret);
}

long lkmdbg_remove_hwpoint(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_hwpoint_request req;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_hwpoint_request(&req, true);
	if (ret)
		return ret;

	ret = lkmdbg_unregister_hwpoint(session, req.id);
	return lkmdbg_hwpoint_copy_reply(argp, &req, ret);
}

long lkmdbg_query_hwpoints(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_hwpoint_query_request req;
	struct lkmdbg_hwpoint_entry *entries;
	struct lkmdbg_hwpoint *entry;
	u32 filled = 0;
	long ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_hwpoint_query(&req);
	if (ret)
		return ret;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	req.done = 1;
	req.next_id = 0;

	mutex_lock(&session->lock);
	list_for_each_entry(entry, &session->hwpoints, node) {
		if (entry->id <= req.start_id)
			continue;
		if (filled == req.max_entries) {
			req.done = 0;
			req.next_id = entries[filled - 1].id;
			break;
		}

		lkmdbg_hwpoint_fill_entry(&entries[filled], entry);
		filled++;
	}
	mutex_unlock(&session->lock);

	req.entries_filled = filled;
	ret = lkmdbg_hwpoint_query_copy_reply(
		argp, &req, entries, req.max_entries * sizeof(*entries));
	kfree(entries);
	return ret;
}

long lkmdbg_single_step(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_single_step_request req;
	struct task_struct *task = NULL;
	pid_t tid;
	long ret;
#ifdef CONFIG_ARM64
	lkmdbg_user_single_step_fn enable_fn;
	u32 freeze_flags;
#endif

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_single_step_request(&req);
	if (ret)
		return ret;

#ifndef CONFIG_ARM64
	return lkmdbg_single_step_copy_reply(argp, &req, -EOPNOTSUPP);
#else
	if (!lkmdbg_user_step_hook_registered ||
	    !lkmdbg_symbols.user_enable_single_step_sym)
		return lkmdbg_single_step_copy_reply(argp, &req, -EOPNOTSUPP);

	ret = lkmdbg_get_target_thread(session, req.tid, &task);
	if (ret)
		return lkmdbg_single_step_copy_reply(argp, &req, ret);

	tid = task->pid;
	freeze_flags = lkmdbg_freeze_thread_flags(session, tid);
	if (!(freeze_flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED)) {
		put_task_struct(task);
		return lkmdbg_single_step_copy_reply(argp, &req, -EBUSY);
	}

	mutex_lock(&session->lock);
	if (session->step_armed) {
		mutex_unlock(&session->lock);
		put_task_struct(task);
		return lkmdbg_single_step_copy_reply(argp, &req, -EBUSY);
	}

	WRITE_ONCE(session->step_armed, true);
	WRITE_ONCE(session->step_tgid, task->tgid);
	WRITE_ONCE(session->step_tid, tid);
	mutex_unlock(&session->lock);

	enable_fn =
		(lkmdbg_user_single_step_fn)lkmdbg_symbols.user_enable_single_step_sym;
	enable_fn(task);
	put_task_struct(task);

	req.tid = tid;
	return lkmdbg_single_step_copy_reply(argp, &req, 0);
#endif
}
