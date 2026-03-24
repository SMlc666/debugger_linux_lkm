#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/task_work.h>

#include "lkmdbg_internal.h"

#define LKMDBG_FREEZE_MAX_THREADS 1024U
#define LKMDBG_FREEZE_DEFAULT_TIMEOUT_MS 1000U
#define LKMDBG_TASK_WORK_NOTIFY_RESUME 1U

typedef int (*lkmdbg_task_work_add_fn)(struct task_struct *task,
				       struct callback_head *work,
				       unsigned int notify);
typedef struct callback_head *(*lkmdbg_task_work_cancel_match_fn)(
	struct task_struct *task,
	bool (*match)(struct callback_head *cb, void *data), void *data);
typedef struct callback_head *(*lkmdbg_task_work_cancel_func_fn)(
	struct task_struct *task, task_work_func_t func);

#ifdef TWA_RESUME
typedef bool (*lkmdbg_task_work_cancel_cb_fn)(struct task_struct *task,
					      struct callback_head *work);
#else
typedef struct callback_head *(*lkmdbg_task_work_cancel_cb_fn)(
	struct task_struct *task, task_work_func_t func);
#endif

struct lkmdbg_freeze_thread {
	struct list_head node;
	struct callback_head work;
	struct lkmdbg_freezer *freezer;
	struct task_struct *task;
	pid_t tid;
	bool callback_entered;
	bool parked;
	bool completed;
};

struct lkmdbg_freezer {
	struct kref refcount;
	struct mutex lock;
	wait_queue_head_t waitq;
	struct list_head threads;
	pid_t target_tgid;
	bool thawing;
	u32 thread_count;
	u32 parked_count;
};

static DEFINE_MUTEX(lkmdbg_freeze_owner_lock);
static struct lkmdbg_freezer *lkmdbg_freeze_owner;

static void lkmdbg_freezer_get(struct lkmdbg_freezer *freezer)
{
	kref_get(&freezer->refcount);
}

static void lkmdbg_freezer_release(struct kref *ref)
{
	struct lkmdbg_freezer *freezer =
		container_of(ref, struct lkmdbg_freezer, refcount);
	struct lkmdbg_freeze_thread *entry;
	struct lkmdbg_freeze_thread *tmp;

	list_for_each_entry_safe(entry, tmp, &freezer->threads, node) {
		list_del_init(&entry->node);
		put_task_struct(entry->task);
		kfree(entry);
	}

	kfree(freezer);
}

static void lkmdbg_freezer_put(struct lkmdbg_freezer *freezer)
{
	kref_put(&freezer->refcount, lkmdbg_freezer_release);
}

static struct lkmdbg_freezer *
lkmdbg_session_freezer_get(struct lkmdbg_session *session)
{
	struct lkmdbg_freezer *freezer;

	mutex_lock(&session->lock);
	freezer = session->freezer;
	if (freezer)
		lkmdbg_freezer_get(freezer);
	mutex_unlock(&session->lock);
	return freezer;
}

static struct lkmdbg_freeze_thread *
lkmdbg_freezer_find_thread_locked(struct lkmdbg_freezer *freezer, pid_t tid)
{
	struct lkmdbg_freeze_thread *entry;

	list_for_each_entry(entry, &freezer->threads, node) {
		if (entry->tid == tid)
			return entry;
	}

	return NULL;
}

static bool lkmdbg_freeze_work_match(struct callback_head *cb, void *data)
{
	return cb == data;
}

static int lkmdbg_task_work_add_resume(struct task_struct *task,
				       struct callback_head *work)
{
	lkmdbg_task_work_add_fn fn;

	if (!lkmdbg_symbols.task_work_add_sym)
		return -EOPNOTSUPP;

	fn = (lkmdbg_task_work_add_fn)lkmdbg_symbols.task_work_add_sym;
	return fn(task, work, LKMDBG_TASK_WORK_NOTIFY_RESUME);
}

static bool lkmdbg_freezer_thread_settled_locked(
	struct lkmdbg_freeze_thread *entry)
{
	if (entry->completed || entry->parked)
		return true;

	/*
	 * A thread that is already asleep in the kernel has stopped executing
	 * user mode. Thaw will cancel its queued resume work if it never
	 * reaches the parking callback.
	 */
	return READ_ONCE(entry->task->__state) != TASK_RUNNING;
}

static bool lkmdbg_freezer_all_settled_locked(struct lkmdbg_freezer *freezer)
{
	struct lkmdbg_freeze_thread *entry;

	list_for_each_entry(entry, &freezer->threads, node) {
		if (!lkmdbg_freezer_thread_settled_locked(entry))
			return false;
	}

	return true;
}

static bool lkmdbg_freezer_all_completed_locked(struct lkmdbg_freezer *freezer)
{
	struct lkmdbg_freeze_thread *entry;

	list_for_each_entry(entry, &freezer->threads, node) {
		if (!entry->completed)
			return false;
	}

	return true;
}

static u32 lkmdbg_freezer_settled_count_locked(struct lkmdbg_freezer *freezer)
{
	struct lkmdbg_freeze_thread *entry;
	u32 settled = 0;

	list_for_each_entry(entry, &freezer->threads, node) {
		if (lkmdbg_freezer_thread_settled_locked(entry))
			settled++;
	}

	return settled;
}

static void lkmdbg_freezer_fill_reply_locked(struct lkmdbg_freezer *freezer,
					     struct lkmdbg_freeze_request *req)
{
	req->threads_total = freezer->thread_count;
	req->threads_settled = lkmdbg_freezer_settled_count_locked(freezer);
	req->threads_parked = freezer->parked_count;
}

static void lkmdbg_freezer_finish_thread(struct lkmdbg_freeze_thread *entry)
{
	struct lkmdbg_freezer *freezer = entry->freezer;

	mutex_lock(&freezer->lock);
	if (!entry->completed) {
		if (entry->parked && freezer->parked_count > 0)
			freezer->parked_count--;
		entry->parked = false;
		entry->completed = true;
		wake_up_all(&freezer->waitq);
	}
	mutex_unlock(&freezer->lock);
}

static void lkmdbg_freeze_task_work(struct callback_head *work)
{
	struct lkmdbg_freeze_thread *entry =
		container_of(work, struct lkmdbg_freeze_thread, work);
	struct lkmdbg_freezer *freezer = entry->freezer;
	bool should_park = false;

	mutex_lock(&freezer->lock);
	entry->callback_entered = true;
	if (!freezer->thawing && !(current->flags & PF_EXITING)) {
		entry->parked = true;
		freezer->parked_count++;
		should_park = true;
	}
	wake_up_all(&freezer->waitq);
	mutex_unlock(&freezer->lock);

	if (should_park)
		wait_event(freezer->waitq, READ_ONCE(freezer->thawing));

	lkmdbg_freezer_finish_thread(entry);
	lkmdbg_freezer_put(freezer);
}

static int lkmdbg_freezer_create(struct lkmdbg_freezer **freezer_out,
				 pid_t target_tgid)
{
	struct lkmdbg_freezer *freezer;

	freezer = kzalloc(sizeof(*freezer), GFP_KERNEL);
	if (!freezer)
		return -ENOMEM;

	kref_init(&freezer->refcount);
	mutex_init(&freezer->lock);
	init_waitqueue_head(&freezer->waitq);
	INIT_LIST_HEAD(&freezer->threads);
	freezer->target_tgid = target_tgid;
	*freezer_out = freezer;
	return 0;
}

static int lkmdbg_freezer_track_task(struct lkmdbg_freezer *freezer,
				     struct task_struct *task)
{
	struct lkmdbg_freeze_thread *entry;
	int ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		put_task_struct(task);
		return -ENOMEM;
	}

	entry->freezer = freezer;
	entry->task = task;
	entry->tid = task->pid;
	INIT_LIST_HEAD(&entry->node);
	init_task_work(&entry->work, lkmdbg_freeze_task_work);

	mutex_lock(&freezer->lock);
	if (freezer->thawing ||
	    lkmdbg_freezer_find_thread_locked(freezer, entry->tid)) {
		mutex_unlock(&freezer->lock);
		put_task_struct(task);
		kfree(entry);
		return 0;
	}

	list_add_tail(&entry->node, &freezer->threads);
	freezer->thread_count++;
	lkmdbg_freezer_get(freezer);
	mutex_unlock(&freezer->lock);

	ret = lkmdbg_task_work_add_resume(task, &entry->work);
	if (!ret)
		return 0;

	mutex_lock(&freezer->lock);
	if (!entry->completed) {
		entry->completed = true;
		list_del_init(&entry->node);
		if (freezer->thread_count > 0)
			freezer->thread_count--;
	}
	mutex_unlock(&freezer->lock);

	put_task_struct(task);
	kfree(entry);
	lkmdbg_freezer_put(freezer);
	if (ret == -ESRCH)
		return 0;
	return ret;
}

static int lkmdbg_freezer_queue_snapshot(struct lkmdbg_freezer *freezer)
{
	struct task_struct **tasks;
	struct task_struct *leader;
	struct task_struct *task;
	u32 count = 0;
	u32 i;
	int ret = 0;

	tasks = kcalloc(LKMDBG_FREEZE_MAX_THREADS, sizeof(*tasks), GFP_KERNEL);
	if (!tasks)
		return -ENOMEM;

	leader = get_pid_task(find_vpid(freezer->target_tgid), PIDTYPE_TGID);
	if (!leader) {
		kfree(tasks);
		return -ESRCH;
	}

	tasks[count++] = leader;
	rcu_read_lock();
	for_each_thread(leader, task) {
		if (count == LKMDBG_FREEZE_MAX_THREADS) {
			rcu_read_unlock();
			ret = -E2BIG;
			goto out_put;
		}
		get_task_struct(task);
		tasks[count++] = task;
	}
	rcu_read_unlock();

	for (i = 0; i < count; i++) {
		ret = lkmdbg_freezer_track_task(freezer, tasks[i]);
		if (ret)
			goto out_remaining;
	}

	kfree(tasks);
	return 0;

out_remaining:
	for (; i < count; i++)
		put_task_struct(tasks[i]);
	kfree(tasks);
	return ret;

out_put:
	for (i = 0; i < count; i++)
		put_task_struct(tasks[i]);
	kfree(tasks);
	return ret;
}

static int lkmdbg_freezer_expand_until_stable(struct lkmdbg_freezer *freezer,
					      unsigned long deadline)
{
	u32 tracked_before;
	u32 tracked_after;
	long remaining;
	int ret;

	for (;;) {
		mutex_lock(&freezer->lock);
		tracked_before = freezer->thread_count;
		mutex_unlock(&freezer->lock);

		ret = lkmdbg_freezer_queue_snapshot(freezer);
		if (ret)
			return ret;

		remaining = deadline ? (long)(deadline - jiffies) : 1;
		if (deadline && remaining <= 0)
			return -ETIMEDOUT;

		if (!wait_event_timeout(
			    freezer->waitq,
			    ({ bool done; \
				mutex_lock(&freezer->lock); \
				done = lkmdbg_freezer_all_settled_locked(freezer); \
				mutex_unlock(&freezer->lock); \
				done; }),
			    deadline ? remaining : 1))
			return -ETIMEDOUT;

		mutex_lock(&freezer->lock);
		tracked_after = freezer->thread_count;
		if (tracked_after == tracked_before &&
		    lkmdbg_freezer_all_settled_locked(freezer)) {
			mutex_unlock(&freezer->lock);
			return 0;
		}
		mutex_unlock(&freezer->lock);
	}
}

static struct callback_head *
lkmdbg_freezer_cancel_pending_entry(struct lkmdbg_freeze_thread *entry)
{
	lkmdbg_task_work_cancel_match_fn cancel_match;
	lkmdbg_task_work_cancel_func_fn cancel_func;
	lkmdbg_task_work_cancel_cb_fn cancel_cb;

	if (entry->callback_entered || entry->completed)
		return NULL;

	if (lkmdbg_symbols.task_work_cancel_match_sym) {
		cancel_match = (lkmdbg_task_work_cancel_match_fn)
			lkmdbg_symbols.task_work_cancel_match_sym;
		return cancel_match(entry->task, lkmdbg_freeze_work_match,
				    &entry->work);
	}

	if (lkmdbg_symbols.task_work_cancel_func_sym) {
		cancel_func = (lkmdbg_task_work_cancel_func_fn)
			lkmdbg_symbols.task_work_cancel_func_sym;
		return cancel_func(entry->task, lkmdbg_freeze_task_work);
	}

	if (!lkmdbg_symbols.task_work_cancel_sym)
		return NULL;

	cancel_cb = (lkmdbg_task_work_cancel_cb_fn)
		lkmdbg_symbols.task_work_cancel_sym;
#ifdef TWA_RESUME
	return cancel_cb(entry->task, &entry->work) ? &entry->work : NULL;
#else
	return cancel_cb(entry->task, lkmdbg_freeze_task_work);
#endif
}

static void lkmdbg_freezer_begin_thaw(struct lkmdbg_freezer *freezer)
{
	struct lkmdbg_freeze_thread *entry;

	mutex_lock(&freezer->lock);
	freezer->thawing = true;
	mutex_unlock(&freezer->lock);
	wake_up_all(&freezer->waitq);

	list_for_each_entry(entry, &freezer->threads, node) {
		if (!lkmdbg_freezer_cancel_pending_entry(entry))
			continue;

		lkmdbg_freezer_finish_thread(entry);
		lkmdbg_freezer_put(freezer);
	}

	wake_up_all(&freezer->waitq);
}

static void lkmdbg_freezer_detach_session(struct lkmdbg_session *session,
					  struct lkmdbg_freezer *freezer)
{
	mutex_lock(&session->lock);
	if (session->freezer == freezer)
		session->freezer = NULL;
	mutex_unlock(&session->lock);

	mutex_lock(&lkmdbg_freeze_owner_lock);
	if (lkmdbg_freeze_owner == freezer)
		lkmdbg_freeze_owner = NULL;
	mutex_unlock(&lkmdbg_freeze_owner_lock);
}

static long lkmdbg_freezer_copy_reply(void __user *argp,
				      struct lkmdbg_freeze_request *req,
				      long ret)
{
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

static int lkmdbg_validate_freeze_request(struct lkmdbg_freeze_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	return 0;
}

static int lkmdbg_session_target_tgid(struct lkmdbg_session *session,
				      pid_t *tgid_out)
{
	pid_t target_tgid;

	mutex_lock(&session->lock);
	target_tgid = session->target_tgid;
	mutex_unlock(&session->lock);

	if (target_tgid <= 0)
		return -ENODEV;

	*tgid_out = target_tgid;
	return 0;
}

int lkmdbg_session_freeze_target(struct lkmdbg_session *session,
				 u32 timeout_ms,
				 struct lkmdbg_freeze_request *req_out)
{
	struct lkmdbg_freezer *freezer = NULL;
	unsigned long deadline = 0;
	pid_t target_tgid;
	long ret;

	ret = lkmdbg_session_target_tgid(session, &target_tgid);
	if (ret)
		return ret;

	mutex_lock(&session->lock);
	if (session->freezer) {
		mutex_unlock(&session->lock);
		return -EBUSY;
	}
	mutex_unlock(&session->lock);

	ret = lkmdbg_freezer_create(&freezer, target_tgid);
	if (ret)
		return ret;

	mutex_lock(&lkmdbg_freeze_owner_lock);
	if (lkmdbg_freeze_owner) {
		mutex_unlock(&lkmdbg_freeze_owner_lock);
		lkmdbg_freezer_put(freezer);
		return -EBUSY;
	}
	lkmdbg_freeze_owner = freezer;
	mutex_unlock(&lkmdbg_freeze_owner_lock);

	mutex_lock(&session->lock);
	session->freezer = freezer;
	mutex_unlock(&session->lock);

	deadline = jiffies +
		   msecs_to_jiffies(timeout_ms ? timeout_ms :
						      LKMDBG_FREEZE_DEFAULT_TIMEOUT_MS);
	ret = lkmdbg_freezer_expand_until_stable(freezer, deadline);

	if (req_out) {
		memset(req_out, 0, sizeof(*req_out));
		req_out->version = LKMDBG_PROTO_VERSION;
		req_out->size = sizeof(*req_out);
		mutex_lock(&freezer->lock);
		lkmdbg_freezer_fill_reply_locked(freezer, req_out);
		mutex_unlock(&freezer->lock);
	}

	if (ret) {
		lkmdbg_freezer_detach_session(session, freezer);
		lkmdbg_freezer_begin_thaw(freezer);
		lkmdbg_freezer_put(freezer);
	}

	return ret;
}

int lkmdbg_session_thaw_target(struct lkmdbg_session *session, u32 timeout_ms,
			       struct lkmdbg_freeze_request *req_out)
{
	struct lkmdbg_freezer *freezer;
	unsigned long deadline;
	long ret = 0;

	mutex_lock(&session->lock);
	freezer = session->freezer;
	mutex_unlock(&session->lock);
	if (!freezer)
		return -ENODEV;

	lkmdbg_freezer_detach_session(session, freezer);
	lkmdbg_freezer_begin_thaw(freezer);

	deadline = jiffies +
		   msecs_to_jiffies(timeout_ms ? timeout_ms :
						      LKMDBG_FREEZE_DEFAULT_TIMEOUT_MS);
	wait_event_timeout(
		freezer->waitq,
		({ bool done; \
			mutex_lock(&freezer->lock); \
			done = lkmdbg_freezer_all_completed_locked(freezer); \
			if (req_out) \
				lkmdbg_freezer_fill_reply_locked(freezer, req_out); \
			mutex_unlock(&freezer->lock); \
			done; }),
		(long)(deadline - jiffies) > 0 ? (long)(deadline - jiffies) : 1);

	mutex_lock(&freezer->lock);
	if (req_out)
		lkmdbg_freezer_fill_reply_locked(freezer, req_out);
	if (!lkmdbg_freezer_all_completed_locked(freezer))
		ret = -ETIMEDOUT;
	mutex_unlock(&freezer->lock);

	lkmdbg_freezer_put(freezer);
	return ret;
}

long lkmdbg_freeze_threads(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_freeze_request req;
	pid_t target_tgid;
	u64 counts;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_freeze_request(&req);
	if (ret)
		return ret;

	ret = lkmdbg_session_target_tgid(session, &target_tgid);
	if (ret)
		return lkmdbg_freezer_copy_reply(argp, &req, ret);

	ret = lkmdbg_session_freeze_target(session, req.timeout_ms, &req);
	if (ret)
		return lkmdbg_freezer_copy_reply(argp, &req, ret);

	counts = ((u64)req.threads_settled << 32) | req.threads_parked;
	lkmdbg_session_commit_stop(session, LKMDBG_STOP_REASON_FREEZE,
				   target_tgid, 0, 0,
				   LKMDBG_STOP_FLAG_FROZEN,
				   req.threads_total, counts, NULL);
	lkmdbg_session_queue_event_ex(session, LKMDBG_EVENT_TARGET_STOP,
				      LKMDBG_STOP_REASON_FREEZE,
				      target_tgid, 0, 0,
				      req.threads_total, counts);
	return lkmdbg_freezer_copy_reply(argp, &req, 0);
}

long lkmdbg_thaw_threads(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_freeze_request req;
	struct lkmdbg_stop_state stop;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_freeze_request(&req);
	if (ret)
		return ret;

	memset(&stop, 0, sizeof(stop));
	mutex_lock(&session->lock);
	stop = session->stop_state;
	mutex_unlock(&session->lock);
	lkmdbg_session_clear_stop(session);

	ret = lkmdbg_session_thaw_target(session, req.timeout_ms, &req);
	if (ret && ret != -ENODEV && (stop.flags & LKMDBG_STOP_FLAG_ACTIVE)) {
		mutex_lock(&session->lock);
		session->stop_state = stop;
		mutex_unlock(&session->lock);
	}
	return lkmdbg_freezer_copy_reply(argp, &req, ret);
}

int lkmdbg_session_freeze_on_target_change(struct lkmdbg_session *session)
{
	mutex_lock(&session->lock);
	session->target_gen++;
	session->target_tgid = 0;
	session->target_tid = 0;
	mutex_unlock(&session->lock);
	lkmdbg_session_freeze_release(session);
	lkmdbg_session_clear_stop(session);
	return 0;
}

u32 lkmdbg_freeze_thread_flags(struct lkmdbg_session *session, pid_t tid)
{
	struct lkmdbg_freezer *freezer;
	struct lkmdbg_freeze_thread *entry;
	u32 flags = 0;

	freezer = lkmdbg_session_freezer_get(session);
	if (freezer) {
		mutex_lock(&freezer->lock);
		entry = lkmdbg_freezer_find_thread_locked(freezer, tid);
		if (entry) {
			flags |= LKMDBG_THREAD_FLAG_FREEZE_TRACKED;
			if (lkmdbg_freezer_thread_settled_locked(entry))
				flags |= LKMDBG_THREAD_FLAG_FREEZE_SETTLED;
			if (entry->parked)
				flags |= LKMDBG_THREAD_FLAG_FREEZE_PARKED;
		}
		mutex_unlock(&freezer->lock);
		lkmdbg_freezer_put(freezer);
	}
	flags |= lkmdbg_remote_call_thread_flags(session, tid);
	return flags;
}

void lkmdbg_session_freeze_release(struct lkmdbg_session *session)
{
	struct lkmdbg_freezer *freezer;

	mutex_lock(&session->lock);
	freezer = session->freezer;
	mutex_unlock(&session->lock);
	if (!freezer)
		return;

	lkmdbg_freezer_detach_session(session, freezer);
	lkmdbg_freezer_begin_thaw(freezer);
	lkmdbg_freezer_put(freezer);
	lkmdbg_session_clear_stop(session);
}
