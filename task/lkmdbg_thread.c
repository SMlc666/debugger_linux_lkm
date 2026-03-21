#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#ifdef CONFIG_ARM64
#include <asm/processor.h>
#include <asm/ptrace.h>
#endif

#include "lkmdbg_internal.h"

#define LKMDBG_THREAD_MAX_ENTRIES 1024U

struct lkmdbg_thread_ref {
	struct task_struct *task;
	pid_t tid;
};

static int lkmdbg_thread_ref_cmp(const void *lhs, const void *rhs)
{
	const struct lkmdbg_thread_ref *a = lhs;
	const struct lkmdbg_thread_ref *b = rhs;

	if (a->tid < b->tid)
		return -1;
	if (a->tid > b->tid)
		return 1;
	return 0;
}

static void lkmdbg_put_thread_refs(struct lkmdbg_thread_ref *refs, u32 count)
{
	u32 i;

	if (!refs)
		return;

	for (i = 0; i < count; i++) {
		if (refs[i].task)
			put_task_struct(refs[i].task);
	}

	kfree(refs);
}

static int lkmdbg_capture_thread_refs(pid_t target_tgid,
				      struct lkmdbg_thread_ref **refs_out,
				      u32 *count_out)
{
	struct lkmdbg_thread_ref *refs;
	struct task_struct *leader;
	struct task_struct *task;
	u32 count = 0;
	int ret = 0;

	refs = kcalloc(LKMDBG_THREAD_MAX_ENTRIES, sizeof(*refs), GFP_KERNEL);
	if (!refs)
		return -ENOMEM;

	leader = get_pid_task(find_vpid(target_tgid), PIDTYPE_TGID);
	if (!leader) {
		kfree(refs);
		return -ESRCH;
	}

	refs[count].task = leader;
	refs[count].tid = leader->pid;
	count++;

	rcu_read_lock();
	for_each_thread(leader, task) {
		if (task == leader)
			continue;

		if (count == LKMDBG_THREAD_MAX_ENTRIES) {
			ret = -E2BIG;
			rcu_read_unlock();
			goto out;
		}

		get_task_struct(task);
		refs[count].task = task;
		refs[count].tid = task->pid;
		count++;
	}
	rcu_read_unlock();

	sort(refs, count, sizeof(*refs), lkmdbg_thread_ref_cmp, NULL);
	*refs_out = refs;
	*count_out = count;
	return 0;

out:
	lkmdbg_put_thread_refs(refs, count);
	return ret;
}

static int lkmdbg_validate_thread_query(struct lkmdbg_thread_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_THREAD_MAX_ENTRIES)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (req->start_tid < 0)
		return -EINVAL;

	return 0;
}

static int lkmdbg_validate_thread_regs(struct lkmdbg_thread_regs_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (req->tid <= 0)
		return -EINVAL;

	return 0;
}

static long lkmdbg_thread_query_copy_reply(void __user *argp,
					   struct lkmdbg_thread_query_request *req,
					   struct lkmdbg_thread_entry *entries,
					   size_t entries_bytes)
{
	if (copy_to_user(u64_to_user_ptr(req->entries_addr), entries,
			 entries_bytes))
		return -EFAULT;

	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return 0;
}

static long lkmdbg_thread_regs_copy_reply(void __user *argp,
					  struct lkmdbg_thread_regs_request *req,
					  long ret)
{
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

static void lkmdbg_thread_entry_fill_state(struct lkmdbg_thread_entry *entry,
					   struct task_struct *task)
{
#ifdef CONFIG_ARM64
	const struct pt_regs *regs;

	if (!task->mm)
		return;

	regs = task_pt_regs(task);
	if (!regs)
		return;

	entry->user_pc = regs->pc;
	entry->user_sp = regs->sp;
#else
	(void)entry;
	(void)task;
#endif
}

static void lkmdbg_thread_entry_fill(struct lkmdbg_session *session,
				     struct lkmdbg_thread_entry *entry,
				     struct task_struct *task, pid_t target_tid)
{
	memset(entry, 0, sizeof(*entry));
	entry->tid = task->pid;
	entry->tgid = task->tgid;
	entry->flags = lkmdbg_freeze_thread_flags(session, task->pid);
	if (thread_group_leader(task))
		entry->flags |= LKMDBG_THREAD_FLAG_GROUP_LEADER;
	if (task->flags & PF_EXITING)
		entry->flags |= LKMDBG_THREAD_FLAG_EXITING;
	if (task->pid == target_tid)
		entry->flags |= LKMDBG_THREAD_FLAG_SESSION_TARGET;

	get_task_comm(entry->comm, task);
	lkmdbg_thread_entry_fill_state(entry, task);
}

static int lkmdbg_thread_regs_require_state(struct lkmdbg_session *session,
					    pid_t tid, bool write)
{
	u32 flags;

	flags = lkmdbg_freeze_thread_flags(session, tid);
	if (!(flags & LKMDBG_THREAD_FLAG_FREEZE_TRACKED) ||
	    !(flags & LKMDBG_THREAD_FLAG_FREEZE_SETTLED))
		return -EBUSY;

	if (write && !(flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED))
		return -EBUSY;

	return 0;
}

#ifdef CONFIG_ARM64
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

static int lkmdbg_collect_thread_regs(struct task_struct *task,
				      struct lkmdbg_thread_regs_request *req)
{
	const struct pt_regs *regs;

	if (!task->mm)
		return -ESRCH;

	regs = task_pt_regs(task);
	if (!regs)
		return -ESRCH;

	lkmdbg_regs_arm64_export(&req->regs, regs);
	return 0;
}

static int lkmdbg_apply_thread_regs(struct task_struct *task,
				    const struct lkmdbg_thread_regs_request *req)
{
	struct pt_regs *regs;

	if (!task->mm)
		return -ESRCH;

	regs = task_pt_regs(task);
	if (!regs)
		return -ESRCH;

	lkmdbg_regs_arm64_import(regs, &req->regs);
	return 0;
}
#else
static int lkmdbg_collect_thread_regs(struct task_struct *task,
				      struct lkmdbg_thread_regs_request *req)
{
	(void)task;
	(void)req;
	return -EOPNOTSUPP;
}

static int lkmdbg_apply_thread_regs(struct task_struct *task,
				    const struct lkmdbg_thread_regs_request *req)
{
	(void)task;
	(void)req;
	return -EOPNOTSUPP;
}
#endif

long lkmdbg_query_threads(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_thread_query_request req;
	struct lkmdbg_thread_entry *entries = NULL;
	struct lkmdbg_thread_ref *refs = NULL;
	pid_t target_tgid;
	pid_t target_tid;
	u32 ref_count = 0;
	u32 filled = 0;
	u32 i;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_thread_query(&req);
	if (ret)
		return ret;

	ret = lkmdbg_get_target_identity(session, &target_tgid, &target_tid);
	if (ret)
		return ret;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	ret = lkmdbg_capture_thread_refs(target_tgid, &refs, &ref_count);
	if (ret)
		goto out;

	req.done = 1;
	req.next_tid = 0;
	for (i = 0; i < ref_count; i++) {
		if (refs[i].tid <= req.start_tid)
			continue;
		if (filled == req.max_entries) {
			req.done = 0;
			req.next_tid = entries[filled - 1].tid;
			break;
		}

		lkmdbg_thread_entry_fill(session, &entries[filled], refs[i].task,
					 target_tid);
		filled++;
	}

	req.entries_filled = filled;
	ret = lkmdbg_thread_query_copy_reply(
		argp, &req, entries, req.max_entries * sizeof(*entries));

out:
	lkmdbg_put_thread_refs(refs, ref_count);
	kfree(entries);
	return ret;
}

long lkmdbg_get_regs(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_thread_regs_request req;
	struct task_struct *task = NULL;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_thread_regs(&req);
	if (ret)
		return ret;

	ret = lkmdbg_get_target_thread(session, req.tid, &task);
	if (ret)
		return lkmdbg_thread_regs_copy_reply(argp, &req, ret);

	ret = lkmdbg_thread_regs_require_state(session, req.tid, false);
	if (ret) {
		put_task_struct(task);
		return lkmdbg_thread_regs_copy_reply(argp, &req, ret);
	}

	ret = lkmdbg_collect_thread_regs(task, &req);
	put_task_struct(task);
	return lkmdbg_thread_regs_copy_reply(argp, &req, ret);
}

long lkmdbg_set_regs(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_thread_regs_request req;
	struct task_struct *task = NULL;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_thread_regs(&req);
	if (ret)
		return ret;

	ret = lkmdbg_get_target_thread(session, req.tid, &task);
	if (ret)
		return lkmdbg_thread_regs_copy_reply(argp, &req, ret);

	ret = lkmdbg_thread_regs_require_state(session, req.tid, true);
	if (ret) {
		put_task_struct(task);
		return lkmdbg_thread_regs_copy_reply(argp, &req, ret);
	}

	ret = lkmdbg_apply_thread_regs(task, &req);
	put_task_struct(task);
	return lkmdbg_thread_regs_copy_reply(argp, &req, ret);
}
