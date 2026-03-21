#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>

#include "lkmdbg_internal.h"

static struct task_struct *lkmdbg_get_target_task(struct lkmdbg_session *session)
{
	pid_t target_tgid;
	struct task_struct *task;

	mutex_lock(&session->lock);
	target_tgid = session->target_tgid;
	mutex_unlock(&session->lock);

	if (target_tgid <= 0)
		return ERR_PTR(-ENODEV);

	task = get_pid_task(find_vpid(target_tgid), PIDTYPE_TGID);
	if (!task)
		return ERR_PTR(-ESRCH);

	return task;
}

int lkmdbg_get_target_mm(struct lkmdbg_session *session,
			 struct mm_struct **mm_out)
{
	struct task_struct *task;
	struct mm_struct *mm;

	task = lkmdbg_get_target_task(session);
	if (IS_ERR(task))
		return PTR_ERR(task);

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;

	*mm_out = mm;
	return 0;
}
