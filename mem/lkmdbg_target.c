#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>

#include "lkmdbg_internal.h"

static int lkmdbg_get_target_ids_locked(struct lkmdbg_session *session,
					pid_t *tgid_out, pid_t *tid_out)
{
	if (session->target_tgid <= 0)
		return -ENODEV;

	if (tgid_out)
		*tgid_out = session->target_tgid;
	if (tid_out)
		*tid_out = session->target_tid;

	return 0;
}

int lkmdbg_get_target_identity(struct lkmdbg_session *session, pid_t *tgid_out,
			       pid_t *tid_out)
{
	int ret;

	mutex_lock(&session->lock);
	ret = lkmdbg_get_target_ids_locked(session, tgid_out, tid_out);
	mutex_unlock(&session->lock);
	return ret;
}

static struct task_struct *lkmdbg_get_target_leader(struct lkmdbg_session *session)
{
	pid_t target_tgid;
	struct task_struct *task;

	if (lkmdbg_get_target_identity(session, &target_tgid, NULL))
		return ERR_PTR(-ENODEV);

	task = get_pid_task(find_vpid(target_tgid), PIDTYPE_TGID);
	if (!task)
		return ERR_PTR(-ESRCH);

	return task;
}

int lkmdbg_get_target_thread(struct lkmdbg_session *session, pid_t tid_override,
			     struct task_struct **task_out)
{
	pid_t target_tgid;
	pid_t target_tid;
	struct task_struct *task;
	pid_t tid;
	int ret;

	ret = lkmdbg_get_target_identity(session, &target_tgid, &target_tid);
	if (ret)
		return ret;

	tid = tid_override > 0 ? tid_override :
	      (target_tid > 0 ? target_tid : target_tgid);
	task = get_pid_task(find_vpid(tid), PIDTYPE_PID);
	if (!task)
		return -ESRCH;

	if (task->tgid != target_tgid) {
		put_task_struct(task);
		return -ESRCH;
	}

	*task_out = task;
	return 0;
}

int lkmdbg_get_target_mm(struct lkmdbg_session *session,
			 struct mm_struct **mm_out)
{
	struct task_struct *task;
	struct mm_struct *mm;

	task = lkmdbg_get_target_leader(session);
	if (IS_ERR(task))
		return PTR_ERR(task);

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;

	*mm_out = mm;
	return 0;
}
