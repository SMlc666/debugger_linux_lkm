#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "lkmdbg_internal.h"

#define LKMDBG_MEM_MAX_XFER (256U * 1024U)
#define LKMDBG_MEM_MAX_OPS 64U
#define LKMDBG_MEM_MAX_BATCH_BYTES (1024U * 1024U)
#define LKMDBG_MEM_MAX_PIN_PAGES 16U
#define LKMDBG_MEM_OP_VALID_FLAGS LKMDBG_MEM_OP_FLAG_FORCE_ACCESS

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

static int lkmdbg_validate_mem_op(const struct lkmdbg_mem_op *op, bool write)
{
	if (!op->length || op->length > LKMDBG_MEM_MAX_XFER)
		return -EINVAL;

	if (!op->local_addr || !op->remote_addr)
		return -EINVAL;

	if (op->remote_addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (op->remote_addr + op->length < op->remote_addr)
		return -EINVAL;

	if (op->remote_addr + op->length > (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (op->flags & ~LKMDBG_MEM_OP_VALID_FLAGS)
		return -EINVAL;

	return 0;
}

static int lkmdbg_validate_mem_req(struct lkmdbg_mem_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->ops_addr || !req->op_count || req->op_count > LKMDBG_MEM_MAX_OPS)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	return 0;
}

static long lkmdbg_get_remote_pages_nofault(struct mm_struct *mm,
					    unsigned long start,
					    unsigned int nr_pages,
					    unsigned int flags,
					    struct page **pages)
{
	long ret;
	int locked = 1;

	mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	ret = get_user_pages_remote(mm, start, nr_pages,
				    flags | LKMDBG_GUP_NOFAULT_FLAG, pages,
				    &locked);
#else
	ret = get_user_pages_remote(mm, start, nr_pages,
				    flags | LKMDBG_GUP_NOFAULT_FLAG, pages,
				    NULL, &locked);
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

static void lkmdbg_put_remote_pages(struct page **pages, unsigned int nr_pages)
{
	unsigned int i;

	for (i = 0; i < nr_pages; i++) {
		if (!pages[i])
			continue;
		put_page(pages[i]);
		pages[i] = NULL;
	}
}

static unsigned int lkmdbg_mem_gup_flags(const struct lkmdbg_mem_op *op,
					 bool write)
{
	unsigned int flags = write ? FOLL_WRITE : 0;

	if (op->flags & LKMDBG_MEM_OP_FLAG_FORCE_ACCESS)
		flags |= FOLL_FORCE;

	return flags;
}

static long lkmdbg_mem_xfer_window(struct mm_struct *mm, u64 remote_addr,
				   u64 local_addr, size_t length,
				   unsigned int gup_flags, bool write, u8 *bounce)
{
	struct page *pages[LKMDBG_MEM_MAX_PIN_PAGES] = { 0 };
	size_t total_done = 0;

	while (total_done < length) {
		unsigned long window_remote;
		unsigned long first_offset;
		size_t window_len;
		unsigned int nr_pages;
		long pinned;
		size_t window_done = 0;
		unsigned int i;

		window_remote = (unsigned long)remote_addr + total_done;
		first_offset = offset_in_page(window_remote);
		window_len = min_t(size_t, length - total_done,
				   (LKMDBG_MEM_MAX_PIN_PAGES * PAGE_SIZE) -
					   first_offset);
		nr_pages = DIV_ROUND_UP(first_offset + window_len, PAGE_SIZE);

		pinned = lkmdbg_get_remote_pages_nofault(mm, window_remote,
							 nr_pages, gup_flags,
							 pages);
		if (pinned <= 0) {
			if (lkmdbg_mem_is_nofault_miss(pinned))
				break;
			return pinned;
		}

		for (i = 0; i < pinned; i++) {
			unsigned long page_offset = (i == 0) ? first_offset : 0;
			size_t chunk_len;
			void *page_addr;
			void __user *user_addr;

			chunk_len = min_t(size_t, window_len - window_done,
					  PAGE_SIZE - page_offset);
			user_addr = u64_to_user_ptr(local_addr + total_done +
						    window_done);
			page_addr = kmap_local_page(pages[i]);

			if (write) {
				if (copy_from_user(bounce, user_addr, chunk_len)) {
					kunmap_local(page_addr);
					lkmdbg_put_remote_pages(pages, pinned);
					return -EFAULT;
				}
				memcpy((u8 *)page_addr + page_offset, bounce,
				       chunk_len);
				set_page_dirty_lock(pages[i]);
			} else if (copy_to_user(user_addr,
						(u8 *)page_addr + page_offset,
						chunk_len)) {
				kunmap_local(page_addr);
				lkmdbg_put_remote_pages(pages, pinned);
				return -EFAULT;
			}

			kunmap_local(page_addr);
			put_page(pages[i]);
			pages[i] = NULL;
			window_done += chunk_len;
		}

		total_done += window_done;
		if ((unsigned int)pinned < nr_pages)
			break;

		cond_resched();
	}

	return (long)total_done;
}

static int lkmdbg_get_target_mm(struct lkmdbg_session *session,
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

static long lkmdbg_mem_xfer_ops(struct mm_struct *mm, struct lkmdbg_mem_op *ops,
				u32 op_count, bool write)
{
	u8 *bounce = NULL;
	u64 batch_total = 0;
	u64 transferred = 0;
	u32 i;
	int ret;

	for (i = 0; i < op_count; i++) {
		ret = lkmdbg_validate_mem_op(&ops[i], write);
		if (ret)
			return ret;

		if (batch_total + ops[i].length > LKMDBG_MEM_MAX_BATCH_BYTES)
			return -E2BIG;

		batch_total += ops[i].length;
		ops[i].bytes_done = 0;
	}

	if (write) {
		bounce = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!bounce)
			return -ENOMEM;
	}

	for (i = 0; i < op_count; i++) {
		long copied;
		unsigned int gup_flags;

		gup_flags = lkmdbg_mem_gup_flags(&ops[i], write);
		copied = lkmdbg_mem_xfer_window(mm, ops[i].remote_addr,
						ops[i].local_addr, ops[i].length,
						gup_flags, write, bounce);
		if (copied < 0) {
			kfree(bounce);
			return copied;
		}

		ops[i].bytes_done = (u32)copied;
		transferred += copied;
		if ((u32)copied != ops[i].length)
			break;
	}

	kfree(bounce);
	return (long)transferred;
}

static long lkmdbg_mem_xfer(struct lkmdbg_session *session, void __user *argp,
			    bool write)
{
	struct lkmdbg_mem_request req;
	struct lkmdbg_mem_op *ops;
	struct mm_struct *mm;
	u64 total_bytes = 0;
	size_t ops_bytes;
	u32 processed = 0;
	u32 i;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_mem_req(&req);
	if (ret)
		return ret;

	ops_bytes = req.op_count * sizeof(*ops);
	ops = kmalloc_array(req.op_count, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	if (copy_from_user(ops, u64_to_user_ptr(req.ops_addr), ops_bytes)) {
		kfree(ops);
		return -EFAULT;
	}

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret) {
		kfree(ops);
		return ret;
	}

	ret = lkmdbg_mem_xfer_ops(mm, ops, req.op_count, write);
	if (ret < 0)
		goto out_mm;

	total_bytes = (u64)ret;
	for (i = 0; i < req.op_count; i++) {
		if (!ops[i].bytes_done)
			break;
		processed++;
		if (ops[i].bytes_done != ops[i].length)
			break;
	}

	req.ops_done = processed;
	req.bytes_done = total_bytes;

	if (copy_to_user(u64_to_user_ptr(req.ops_addr), ops, ops_bytes)) {
		ret = -EFAULT;
		goto out_mm;
	}

	if (copy_to_user(argp, &req, sizeof(req))) {
		ret = -EFAULT;
		goto out_mm;
	}

	ret = 0;

out_mm:
	mmput(mm);
	kfree(ops);
	return ret;
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
