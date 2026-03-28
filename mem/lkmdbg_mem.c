#include <linux/cacheflush.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

#define LKMDBG_MEM_MAX_XFER (256U * 1024U)
#define LKMDBG_MEM_MAX_OPS 64U
#define LKMDBG_MEM_MAX_BATCH_BYTES (1024U * 1024U)
#define LKMDBG_MEM_MAX_PIN_PAGES 16U
#define LKMDBG_MEM_OP_VALID_FLAGS LKMDBG_MEM_OP_FLAG_FORCE_ACCESS
#define LKMDBG_PAGE_MAX_ENTRIES 1024U

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

static int lkmdbg_validate_page_query(struct lkmdbg_page_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_PAGE_MAX_ENTRIES)
		return -EINVAL;

	if (!req->length)
		return -EINVAL;

	if (req->flags & ~LKMDBG_PAGE_QUERY_FLAG_LEAF_STEP)
		return -EINVAL;

	if (req->start_addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (req->start_addr + req->length < req->start_addr)
		return -EINVAL;

	if (req->start_addr + req->length > (u64)TASK_SIZE_MAX)
		return -EINVAL;

	return 0;
}

static long lkmdbg_get_remote_pages_nofault(struct mm_struct *mm,
					    unsigned long start,
					    unsigned int nr_pages,
					    unsigned int flags,
					    struct page **pages)
{
	struct lkmdbg_target_pt_info pt_info;
	unsigned long page_addr;
	unsigned int present_pages = 0;
	long ret;
	int lookup_ret;
	int locked = 1;

	mmap_read_lock(mm);
	page_addr = start & PAGE_MASK;
	while (present_pages < nr_pages) {
		lookup_ret = lkmdbg_target_pt_lookup_locked(
			mm, page_addr + (present_pages * PAGE_SIZE), &pt_info);
		if (lookup_ret) {
			ret = lookup_ret;
			goto out_unlock;
		}
		if (!(pt_info.flags & LKMDBG_TARGET_PT_FLAG_PRESENT))
			break;
		present_pages++;
	}
	if (!present_pages) {
		ret = 0;
		goto out_unlock;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	ret = get_user_pages_remote(mm, start, present_pages,
				    flags | LKMDBG_GUP_NOFAULT_FLAG, pages,
				    &locked);
#else
	ret = get_user_pages_remote(mm, start, present_pages,
				    flags | LKMDBG_GUP_NOFAULT_FLAG, pages,
				    NULL, &locked);
#endif
out_unlock:
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

static void lkmdbg_mem_sync_exec_page(struct mm_struct *mm, struct page *page,
				      unsigned long remote_addr)
{
	struct vm_area_struct *vma;

	flush_dcache_page(page);

	mmap_read_lock(mm);
	vma = find_vma(mm, remote_addr);
	if (vma && remote_addr >= vma->vm_start && remote_addr < vma->vm_end &&
	    (vma->vm_flags & VM_EXEC))
		flush_icache_page(vma, page);
	mmap_read_unlock(mm);
}

static long lkmdbg_mem_xfer_window(struct mm_struct *mm, u64 remote_addr,
				   u64 local_addr, size_t length,
				   unsigned int gup_flags, bool write)
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
			page_addr = lkmdbg_kmap_local_page(pages[i]);

			if (write) {
				if (copy_from_user((u8 *)page_addr + page_offset,
						   user_addr, chunk_len)) {
					lkmdbg_kunmap_local(pages[i], page_addr);
					lkmdbg_put_remote_pages(pages, pinned);
					return -EFAULT;
				}
				lkmdbg_mem_sync_exec_page(
					mm, pages[i],
					(unsigned long)remote_addr + total_done +
						window_done);
				set_page_dirty_lock(pages[i]);
			} else if (copy_to_user(user_addr,
						(u8 *)page_addr + page_offset,
						chunk_len)) {
				lkmdbg_kunmap_local(pages[i], page_addr);
				lkmdbg_put_remote_pages(pages, pinned);
				return -EFAULT;
			}

			lkmdbg_kunmap_local(pages[i], page_addr);
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

static void lkmdbg_mem_accumulate_progress(const struct lkmdbg_mem_op *ops,
					   u32 op_count, u32 *processed_out,
					   u64 *bytes_out)
{
	u64 total_bytes = 0;
	u32 processed = 0;
	u32 i;

	for (i = 0; i < op_count; i++) {
		total_bytes += ops[i].bytes_done;
		if (!ops[i].bytes_done)
			break;
		processed++;
		if (ops[i].bytes_done != ops[i].length)
			break;
	}

	*processed_out = processed;
	*bytes_out = total_bytes;
}

static bool lkmdbg_page_try_pin_nofault(struct mm_struct *mm,
					unsigned long page_addr,
					unsigned int gup_flags)
{
	struct page *page = NULL;
	long pinned;

	pinned = lkmdbg_get_remote_pages_nofault(mm, page_addr, 1, gup_flags,
						 &page);
	if (pinned == 1) {
		put_page(page);
		return true;
	}
	if (pinned > 0)
		lkmdbg_put_remote_pages(&page, (unsigned int)pinned);
	return false;
}

static void lkmdbg_page_fill_vma_info(struct mm_struct *mm,
				      struct lkmdbg_page_entry *entry,
				      struct vm_area_struct *vma,
				      unsigned long page_addr)
{
	struct lkmdbg_target_vma_info info;

	lkmdbg_target_vma_fill_info(mm, vma, &info);
	entry->vm_flags_raw = info.vm_flags_raw;
	entry->pgoff = info.pgoff +
		       ((page_addr - info.start_addr) >> PAGE_SHIFT);
	entry->inode = info.inode;
	entry->dev_major = info.dev_major;
	entry->dev_minor = info.dev_minor;

	if (info.prot & LKMDBG_VMA_PROT_READ)
		entry->flags |= LKMDBG_PAGE_FLAG_PROT_READ;
	if (info.prot & LKMDBG_VMA_PROT_WRITE)
		entry->flags |= LKMDBG_PAGE_FLAG_PROT_WRITE;
	if (info.prot & LKMDBG_VMA_PROT_EXEC)
		entry->flags |= LKMDBG_PAGE_FLAG_PROT_EXEC;
	if (info.prot & LKMDBG_VMA_PROT_MAYREAD)
		entry->flags |= LKMDBG_PAGE_FLAG_MAYREAD;
	if (info.prot & LKMDBG_VMA_PROT_MAYWRITE)
		entry->flags |= LKMDBG_PAGE_FLAG_MAYWRITE;
	if (info.prot & LKMDBG_VMA_PROT_MAYEXEC)
		entry->flags |= LKMDBG_PAGE_FLAG_MAYEXEC;
	if (info.flags & LKMDBG_VMA_FLAG_SHARED)
		entry->flags |= LKMDBG_PAGE_FLAG_SHARED;
	if (info.flags & LKMDBG_VMA_FLAG_PFNMAP)
		entry->flags |= LKMDBG_PAGE_FLAG_PFNMAP;
	if (info.flags & LKMDBG_VMA_FLAG_IO)
		entry->flags |= LKMDBG_PAGE_FLAG_IO;
	if (info.flags & LKMDBG_VMA_FLAG_FILE)
		entry->flags |= LKMDBG_PAGE_FLAG_FILE;
	if (info.flags & LKMDBG_VMA_FLAG_ANON)
		entry->flags |= LKMDBG_PAGE_FLAG_ANON;
}

static void lkmdbg_page_fill_pt_info(struct lkmdbg_page_entry *entry,
				     const struct lkmdbg_target_pt_info *info)
{
	entry->pt_entry_raw = info->entry_raw;
	entry->phys_addr = info->phys_addr;
	entry->page_shift = info->page_shift;
	entry->pt_level = info->level;
	entry->pt_flags = info->pt_flags;
	if (info->flags & LKMDBG_TARGET_PT_FLAG_PRESENT)
		entry->flags |= LKMDBG_PAGE_FLAG_PT_PRESENT;
	if (info->flags & LKMDBG_TARGET_PT_FLAG_HUGE)
		entry->flags |= LKMDBG_PAGE_FLAG_PT_HUGE;
}

static unsigned long lkmdbg_page_query_next_cursor(
	unsigned long cursor, const struct lkmdbg_target_pt_info *pt_info,
	u32 query_flags)
{
	u64 leaf_size;
	u64 next;

	if (!(query_flags & LKMDBG_PAGE_QUERY_FLAG_LEAF_STEP) ||
	    !pt_info->page_shift)
		return cursor + PAGE_SIZE;

	leaf_size = 1ULL << pt_info->page_shift;
	next = ((u64)cursor & ~(leaf_size - 1)) + leaf_size;
	if (next <= cursor)
		return cursor + PAGE_SIZE;

	return (unsigned long)next;
}

static void lkmdbg_page_probe_access(struct mm_struct *mm,
				     struct lkmdbg_page_entry *entry)
{
	bool readable;
	bool writable;
	bool force_readable;
	bool force_writable;

	readable = lkmdbg_page_try_pin_nofault(mm, entry->page_addr, 0);
	writable = lkmdbg_page_try_pin_nofault(mm, entry->page_addr, FOLL_WRITE);
	force_readable =
		lkmdbg_page_try_pin_nofault(mm, entry->page_addr, FOLL_FORCE);
	force_writable = lkmdbg_page_try_pin_nofault(
		mm, entry->page_addr, FOLL_FORCE | FOLL_WRITE);

	if (readable)
		entry->flags |= LKMDBG_PAGE_FLAG_NOFAULT_READ;
	if (writable)
		entry->flags |= LKMDBG_PAGE_FLAG_NOFAULT_WRITE;
	if (force_readable)
		entry->flags |= LKMDBG_PAGE_FLAG_FORCE_READ;
	if (force_writable)
		entry->flags |= LKMDBG_PAGE_FLAG_FORCE_WRITE;
	if (readable || writable || force_readable || force_writable)
		entry->flags |= LKMDBG_PAGE_FLAG_PRESENT;
}

static long lkmdbg_page_query_copy_reply(void __user *argp,
					 struct lkmdbg_page_query_request *req,
					 struct lkmdbg_page_entry *entries,
					 size_t entries_bytes)
{
	if (copy_to_user(u64_to_user_ptr(req->entries_addr), entries,
			 entries_bytes))
		return -EFAULT;

	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return 0;
}

static long lkmdbg_mem_copy_reply(void __user *argp, struct lkmdbg_mem_request *req,
				  struct lkmdbg_mem_op *ops, size_t ops_bytes,
				  long ret)
{
	if (copy_to_user(u64_to_user_ptr(req->ops_addr), ops, ops_bytes))
		return -EFAULT;

	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

static long lkmdbg_mem_xfer_ops(struct mm_struct *mm, struct lkmdbg_mem_op *ops,
				u32 op_count, bool write)
{
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

	for (i = 0; i < op_count; i++) {
		long copied;
		unsigned int gup_flags;

		gup_flags = lkmdbg_mem_gup_flags(&ops[i], write);
		copied = lkmdbg_mem_xfer_window(mm, ops[i].remote_addr,
						ops[i].local_addr, ops[i].length,
						gup_flags, write);
		if (copied < 0)
			return copied;

		ops[i].bytes_done = (u32)copied;
		transferred += copied;
		if ((u32)copied != ops[i].length)
			break;
	}

	return (long)transferred;
}

static long lkmdbg_mem_xfer(struct lkmdbg_session *session, void __user *argp,
			    bool write)
{
	struct lkmdbg_mem_request req;
	struct lkmdbg_mem_op *ops;
	struct mm_struct *mm;
	size_t ops_bytes;
	u64 total_bytes = 0;
	u32 processed = 0;
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
	lkmdbg_mem_accumulate_progress(ops, req.op_count, &processed, &total_bytes);
	req.ops_done = processed;
	req.bytes_done = total_bytes;
	if (ret >= 0)
		ret = 0;
	ret = lkmdbg_mem_copy_reply(argp, &req, ops, ops_bytes, ret);

	mmput(mm);
	kfree(ops);
	return ret;
}

long lkmdbg_mem_set_target(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_target_request req;
	struct task_struct *task = NULL;
	pid_t old_tgid;
	pid_t target_tid;

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

	target_tid = req.tid > 0 ? req.tid : req.tgid;
	task = get_pid_task(find_vpid(target_tid), PIDTYPE_PID);
	if (!task)
		return -ESRCH;
	if (task->tgid != req.tgid) {
		put_task_struct(task);
		return -ESRCH;
	}
	put_task_struct(task);

	mutex_lock(&session->lock);
	old_tgid = session->target_tgid;
	mutex_unlock(&session->lock);

	if (old_tgid > 0 && old_tgid != req.tgid)
		lkmdbg_pte_patch_on_target_change(session);

	mutex_lock(&session->lock);
	session->target_gen++;
	session->target_tgid = req.tgid;
	session->target_tid = target_tid;
	mutex_unlock(&session->lock);

	return 0;
}

long lkmdbg_page_query(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_page_query_request req;
	struct lkmdbg_page_entry *entries = NULL;
	struct mm_struct *mm = NULL;
	unsigned long cursor;
	unsigned long end;
	u32 filled = 0;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_page_query(&req);
	if (ret)
		return ret;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret)
		goto out;

	cursor = (unsigned long)(req.start_addr & PAGE_MASK);
	end = (unsigned long)PAGE_ALIGN(req.start_addr + req.length);
	req.done = 1;
	req.next_addr = 0;

	mmap_read_lock(mm);
	while (cursor < end && filled < req.max_entries) {
		struct lkmdbg_page_entry *entry = &entries[filled];
		struct vm_area_struct *vma;
		struct lkmdbg_target_pt_info pt_info;

		memset(entry, 0, sizeof(*entry));
		entry->page_addr = cursor;

		ret = lkmdbg_target_pt_lookup_locked(mm, cursor, &pt_info);
		if (ret) {
			mmap_read_unlock(mm);
			goto out;
		}
		lkmdbg_page_fill_pt_info(entry, &pt_info);

		vma = find_vma(mm, cursor);
		if (vma && cursor >= vma->vm_start && cursor < vma->vm_end) {
			entry->flags |= LKMDBG_PAGE_FLAG_MAPPED;
			lkmdbg_page_fill_vma_info(mm, entry, vma, cursor);
			lkmdbg_page_probe_access(mm, entry);
		}

		filled++;
		cursor = lkmdbg_page_query_next_cursor(cursor, &pt_info,
						       req.flags);
	}
	mmap_read_unlock(mm);

	if (cursor < end) {
		req.done = 0;
		req.next_addr = cursor;
	}
	req.entries_filled = filled;
	ret = lkmdbg_page_query_copy_reply(
		argp, &req, entries, req.max_entries * sizeof(*entries));

out:
	if (mm)
		mmput(mm);
	kfree(entries);
	return ret;
}

long lkmdbg_mem_read(struct lkmdbg_session *session, void __user *argp)
{
	return lkmdbg_mem_xfer(session, argp, false);
}

long lkmdbg_mem_write(struct lkmdbg_session *session, void __user *argp)
{
	return lkmdbg_mem_xfer(session, argp, true);
}
