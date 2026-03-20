#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "lkmdbg_internal.h"

#define LKMDBG_MEM_MAX_XFER 16384U

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

static int lkmdbg_validate_mem_req(struct lkmdbg_mem_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->length || req->length > LKMDBG_MEM_MAX_XFER)
		return -EINVAL;

	if (!req->local_addr || !req->remote_addr)
		return -EINVAL;

	if (req->remote_addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (req->remote_addr + req->length < req->remote_addr)
		return -EINVAL;

	if (req->remote_addr + req->length > (u64)TASK_SIZE_MAX)
		return -EINVAL;

	return 0;
}

static long lkmdbg_mem_xfer(struct lkmdbg_session *session, void __user *argp,
			    bool write)
{
	struct lkmdbg_mem_request req;
	struct task_struct *task;
	void *kbuf;
	int copied;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_mem_req(&req);
	if (ret)
		return ret;

	kbuf = kvmalloc(req.length, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (write) {
		if (copy_from_user(kbuf, u64_to_user_ptr(req.local_addr),
				   req.length)) {
			kvfree(kbuf);
			return -EFAULT;
		}
	}

	task = lkmdbg_get_target_task(session);
	if (IS_ERR(task)) {
		kvfree(kbuf);
		return PTR_ERR(task);
	}

	if (!lkmdbg_symbols.access_process_vm) {
		put_task_struct(task);
		kvfree(kbuf);
		return -ENOENT;
	}

	copied = lkmdbg_symbols.access_process_vm(task,
						  (unsigned long)req.remote_addr,
						  kbuf, (int)req.length,
						  write ? FOLL_WRITE : 0);
	put_task_struct(task);
	if (copied <= 0) {
		kvfree(kbuf);
		return -EFAULT;
	}

	req.bytes_done = copied;

	if (!write && copy_to_user(u64_to_user_ptr(req.local_addr), kbuf, copied)) {
		kvfree(kbuf);
		return -EFAULT;
	}

	kvfree(kbuf);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_mem_set_target(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_target_request req;
	struct task_struct *task;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req))
		return -EINVAL;

	if (req.tgid <= 0)
		return -EINVAL;

	task = get_pid_task(find_vpid(req.tgid), PIDTYPE_TGID);
	if (!task)
		return -ESRCH;
	put_task_struct(task);

	mutex_lock(&session->lock);
	session->target_tgid = req.tgid;
	mutex_unlock(&session->lock);

	return 0;
}

long lkmdbg_mem_read(struct lkmdbg_session *session, void __user *argp)
{
	return lkmdbg_mem_xfer(session, argp, false);
}

long lkmdbg_mem_write(struct lkmdbg_session *session, void __user *argp)
{
	return lkmdbg_mem_xfer(session, argp, true);
}
