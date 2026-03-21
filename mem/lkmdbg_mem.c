#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/pid.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "lkmdbg_internal.h"

#define LKMDBG_MEM_MAX_XFER 16384U

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define LKMDBG_GUP_NOFAULT_FLAG (1U << 5)
#else
#define LKMDBG_GUP_NOFAULT_FLAG FOLL_NOFAULT
#endif

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

static long lkmdbg_get_remote_page_nofault(struct mm_struct *mm,
					   unsigned long addr,
					   unsigned int flags,
					   struct page **page_out)
{
	long ret;
	int locked = 1;

	mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	ret = get_user_pages_remote(mm, addr, 1,
				    flags | LKMDBG_GUP_NOFAULT_FLAG,
				    page_out, &locked);
#else
	struct vm_area_struct *vma = NULL;

	ret = get_user_pages_remote(mm, addr, 1,
				    flags | LKMDBG_GUP_NOFAULT_FLAG,
				    page_out, &vma, &locked);
#endif
	if (locked)
		mmap_read_unlock(mm);

	return ret;
}

static bool lkmdbg_mem_is_nofault_miss(long ret)
{
	switch (ret) {
	case 0:
	case -EFAULT:
	case -ENOENT:
	case -EBUSY:
	case -EHWPOISON:
		return true;
	default:
		return false;
	}
}

static long lkmdbg_copy_remote_page(struct mm_struct *mm, u8 *kbuf,
				    unsigned long remote_addr, size_t length,
				    bool write)
{
	struct page *page = NULL;
	unsigned long page_offset;
	void *page_addr;
	long ret;

	ret = lkmdbg_get_remote_page_nofault(mm, remote_addr,
					     write ? FOLL_WRITE : 0, &page);
	if (ret <= 0)
		return ret;

	page_offset = offset_in_page(remote_addr);
	page_addr = kmap_local_page(page);
	if (write) {
		memcpy((u8 *)page_addr + page_offset, kbuf, length);
		set_page_dirty_lock(page);
	} else {
		memcpy(kbuf, (u8 *)page_addr + page_offset, length);
	}
	kunmap_local(page_addr);
	put_page(page);

	return (long)length;
}

static long lkmdbg_mem_xfer(struct lkmdbg_session *session, void __user *argp,
			    bool write)
{
	struct lkmdbg_mem_request req;
	struct task_struct *task;
	struct mm_struct *mm;
	void *kbuf;
	size_t total_done;
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

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm) {
		kvfree(kbuf);
		return -ESRCH;
	}

	total_done = 0;
	while (total_done < req.length) {
		unsigned long remote_addr;
		size_t chunk_len;
		long copied;

		remote_addr = (unsigned long)req.remote_addr + total_done;
		chunk_len = min_t(size_t, req.length - total_done,
				  PAGE_SIZE - offset_in_page(remote_addr));
		copied = lkmdbg_copy_remote_page(mm, (u8 *)kbuf + total_done,
						 remote_addr, chunk_len, write);
		if (copied < 0) {
			if (lkmdbg_mem_is_nofault_miss(copied))
				break;

			ret = (int)copied;
			goto out_copy;
		}
		if (!copied)
			break;

		total_done += (size_t)copied;
	}

	ret = 0;
	req.bytes_done = total_done;

	if (!write && total_done &&
	    copy_to_user(u64_to_user_ptr(req.local_addr), kbuf, total_done)) {
		ret = -EFAULT;
		goto out_copy;
	}

	if (copy_to_user(argp, &req, sizeof(req))) {
		ret = -EFAULT;
		goto out_copy;
	}

out_copy:
	mmput(mm);
	if (ret) {
		kvfree(kbuf);
		return ret;
	}

	kvfree(kbuf);
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
