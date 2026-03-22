#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "lkmdbg_internal.h"

#define LKMDBG_REMOTE_ALLOC_MAX_BYTES (16U * 1024U * 1024U)
#define LKMDBG_REMOTE_ALLOC_MAX_ENTRIES 128U
#define LKMDBG_REMOTE_ALLOC_VALID_PROT                                        \
	(LKMDBG_REMOTE_ALLOC_PROT_READ | LKMDBG_REMOTE_ALLOC_PROT_WRITE |     \
	 LKMDBG_REMOTE_ALLOC_PROT_EXEC)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
#define LKMDBG_REMOTE_ALLOC_GUP_HAS_LOCKED_ONLY 1
#else
#define LKMDBG_REMOTE_ALLOC_GUP_HAS_LOCKED_ONLY 0
#endif

struct lkmdbg_remote_alloc {
	struct list_head node;
	u64 alloc_id;
	pid_t target_tgid;
	u64 remote_addr;
	u64 mapped_length;
	u64 baseline_vm_flags_raw;
	u32 prot;
	u32 flags;
	u32 page_count;
	struct page **pages;
	pte_t *baseline_ptes;
};

static bool lkmdbg_remote_alloc_ranges_overlap(u64 start_a, u64 len_a, u64 start_b,
					       u64 len_b)
{
	u64 end_a = start_a + len_a;
	u64 end_b = start_b + len_b;

	return start_a < end_b && start_b < end_a;
}

bool lkmdbg_remote_alloc_has_overlap_locked(struct lkmdbg_session *session,
					    unsigned long start,
					    unsigned long length)
{
	struct lkmdbg_remote_alloc *alloc;
	u64 end;

	if (!session || !length)
		return false;

	end = (u64)start + (u64)length;
	if (end < (u64)start)
		return true;

	list_for_each_entry(alloc, &session->remote_allocs, node) {
		if (lkmdbg_remote_alloc_ranges_overlap(
			    (u64)start, (u64)length, alloc->remote_addr,
			    alloc->mapped_length))
			return true;
	}

	return false;
}

static int lkmdbg_remote_alloc_validate_range(u64 addr, u64 length)
{
	if (!addr || !length)
		return -EINVAL;
	if (addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;
	if (addr & ~PAGE_MASK)
		return -EINVAL;
	if (addr + length < addr)
		return -EINVAL;
	if (addr + length > (u64)TASK_SIZE_MAX)
		return -EINVAL;
	return 0;
}

static int lkmdbg_validate_remote_alloc_req(
	const struct lkmdbg_remote_alloc_request *req)
{
	u64 mapped_length;
	int ret;

	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->prot || (req->prot & ~LKMDBG_REMOTE_ALLOC_VALID_PROT))
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	if (!req->length)
		return -EINVAL;
	if (req->length > LKMDBG_REMOTE_ALLOC_MAX_BYTES)
		return -E2BIG;

	mapped_length = PAGE_ALIGN(req->length);
	ret = lkmdbg_remote_alloc_validate_range(req->remote_addr, mapped_length);
	if (ret)
		return ret;

	return 0;
}

static int lkmdbg_validate_remote_alloc_handle(
	const struct lkmdbg_remote_alloc_handle_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->alloc_id)
		return -EINVAL;
	return 0;
}

static int lkmdbg_validate_remote_alloc_query(
	const struct lkmdbg_remote_alloc_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_REMOTE_ALLOC_MAX_ENTRIES)
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	return 0;
}

static int lkmdbg_remote_alloc_validate_shell(struct mm_struct *mm,
					      u64 remote_addr, u64 length,
					      u32 prot,
					      u64 *vm_flags_out)
{
	struct vm_area_struct *vma;
	u64 vm_flags;
	int ret;

	mmap_read_lock(mm);
	ret = lkmdbg_target_vma_lookup_locked(mm, remote_addr, length, &vma);
	if (ret) {
		mmap_read_unlock(mm);
		return ret;
	}

	vm_flags = (u64)vma->vm_flags;
	if ((prot & LKMDBG_REMOTE_ALLOC_PROT_READ) &&
	    !(vm_flags & (VM_READ | VM_MAYREAD)))
		ret = -EACCES;
	else if ((prot & LKMDBG_REMOTE_ALLOC_PROT_WRITE) &&
		 !(vm_flags & (VM_WRITE | VM_MAYWRITE)))
		ret = -EACCES;
	else if ((prot & LKMDBG_REMOTE_ALLOC_PROT_EXEC) &&
		 !(vm_flags & (VM_EXEC | VM_MAYEXEC)))
		ret = -EACCES;
	else if (vm_flags & (VM_PFNMAP | VM_IO))
		ret = -EOPNOTSUPP;
	else
		ret = 0;

	if (!ret && vm_flags_out)
		*vm_flags_out = vm_flags;
	mmap_read_unlock(mm);
	return ret;
}

static int lkmdbg_remote_alloc_get_mm_by_tgid(pid_t tgid, struct mm_struct **mm_out)
{
	struct task_struct *task;
	struct mm_struct *mm;

	task = get_pid_task(find_vpid(tgid), PIDTYPE_TGID);
	if (!task)
		return -ESRCH;

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;

	*mm_out = mm;
	return 0;
}

static void lkmdbg_remote_alloc_free_pages(struct page **pages, u32 page_count)
{
	u32 i;

	if (!pages)
		return;

	for (i = 0; i < page_count; i++) {
		if (!pages[i])
			continue;
		__free_page(pages[i]);
		pages[i] = NULL;
	}
}

static long lkmdbg_remote_alloc_pin_shell_pages(struct mm_struct *mm,
						unsigned long start,
						unsigned int nr_pages,
						struct page **pages)
{
	long ret;
	int locked = 1;

	mmap_read_lock(mm);
#ifdef FOLL_PIN
#if LKMDBG_REMOTE_ALLOC_GUP_HAS_LOCKED_ONLY
	ret = pin_user_pages_remote(mm, start, nr_pages, 0, pages, &locked);
#else
	ret = pin_user_pages_remote(mm, start, nr_pages, 0, pages, NULL,
				    &locked);
#endif
#else
#if LKMDBG_REMOTE_ALLOC_GUP_HAS_LOCKED_ONLY
	ret = get_user_pages_remote(mm, start, nr_pages, 0, pages, &locked);
#else
	ret = get_user_pages_remote(mm, start, nr_pages, 0, pages, NULL,
				    &locked);
#endif
#endif
	if (locked)
		mmap_read_unlock(mm);

	return ret;
}

static void lkmdbg_remote_alloc_put_shell_pages(struct page **pages,
						u32 page_count)
{
	u32 i;

	if (!pages)
		return;

#ifdef FOLL_PIN
	unpin_user_pages(pages, page_count);
	for (i = 0; i < page_count; i++)
		pages[i] = NULL;
#else
	for (i = 0; i < page_count; i++) {
		if (!pages[i])
			continue;
		put_page(pages[i]);
		pages[i] = NULL;
	}
#endif
}

static void lkmdbg_remote_alloc_fill_handle(
	const struct lkmdbg_remote_alloc *alloc,
	struct lkmdbg_remote_alloc_handle_request *req)
{
	memset(req, 0, sizeof(*req));
	req->version = LKMDBG_PROTO_VERSION;
	req->size = sizeof(*req);
	req->alloc_id = alloc->alloc_id;
	req->target_tgid = alloc->target_tgid;
	req->remote_addr = alloc->remote_addr;
	req->mapped_length = alloc->mapped_length;
	req->prot = alloc->prot;
	req->flags = alloc->flags;
}

static void lkmdbg_remote_alloc_fill_entry(
	struct lkmdbg_remote_alloc_entry *entry,
	const struct lkmdbg_remote_alloc *alloc)
{
	memset(entry, 0, sizeof(*entry));
	entry->alloc_id = alloc->alloc_id;
	entry->target_tgid = alloc->target_tgid;
	entry->remote_addr = alloc->remote_addr;
	entry->mapped_length = alloc->mapped_length;
	entry->prot = alloc->prot;
	entry->flags = alloc->flags;
}

static struct lkmdbg_remote_alloc *
lkmdbg_remote_alloc_find_id_locked(struct lkmdbg_session *session, u64 alloc_id)
{
	struct lkmdbg_remote_alloc *alloc;

	list_for_each_entry(alloc, &session->remote_allocs, node) {
		if (alloc->alloc_id == alloc_id)
			return alloc;
	}

	return NULL;
}

static void lkmdbg_remote_alloc_restore(struct lkmdbg_remote_alloc *alloc)
{
	struct mm_struct *mm = NULL;
	u32 i;

	if (!alloc)
		return;

	if (!lkmdbg_remote_alloc_get_mm_by_tgid(alloc->target_tgid, &mm)) {
		mmap_write_lock(mm);
		for (i = 0; i < alloc->page_count; i++) {
			unsigned long addr = (unsigned long)alloc->remote_addr +
					     (i * PAGE_SIZE);

			lkmdbg_pte_rewrite_locked(mm, addr,
						  alloc->baseline_ptes[i],
						  NULL, NULL);
		}
		mmap_write_unlock(mm);
		mmput(mm);
	}

	lkmdbg_remote_alloc_free_pages(alloc->pages, alloc->page_count);
	kfree(alloc->pages);
	kfree(alloc->baseline_ptes);
	kfree(alloc);
}

long lkmdbg_create_remote_alloc(struct lkmdbg_session *session, void __user *argp)
{
#ifndef CONFIG_ARM64
	(void)session;
	(void)argp;
	return -EOPNOTSUPP;
#else
	struct lkmdbg_remote_alloc_request req;
	struct lkmdbg_remote_alloc *alloc = NULL;
	struct mm_struct *mm = NULL;
	u64 target_addr;
	u64 target_vm_flags = 0;
	u64 mapped_length;
	pid_t target_tgid;
	u32 page_count;
	struct page **shell_pages = NULL;
	u32 i;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_remote_alloc_req(&req);
	if (ret)
		return ret;

	ret = lkmdbg_get_target_identity(session, &target_tgid, NULL);
	if (ret)
		return ret;

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret)
		return ret;

	mapped_length = PAGE_ALIGN(req.length);
	target_addr = req.remote_addr;
	page_count = mapped_length >> PAGE_SHIFT;
	ret = lkmdbg_remote_alloc_validate_shell(mm, target_addr, mapped_length,
						 req.prot, &target_vm_flags);
	if (ret)
		goto out_mm;

	shell_pages = kcalloc(page_count, sizeof(*shell_pages), GFP_KERNEL);
	if (!shell_pages) {
		ret = -ENOMEM;
		goto out_mm;
	}
	ret = (int)lkmdbg_remote_alloc_pin_shell_pages(
		mm, (unsigned long)target_addr, page_count, shell_pages);
	if (ret < 0)
		goto out_shell_pages;
	if ((u32)ret != page_count) {
		ret = -EFAULT;
		goto out_shell_pages;
	}
	lkmdbg_remote_alloc_put_shell_pages(shell_pages, page_count);
	kfree(shell_pages);
	shell_pages = NULL;

	alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
	if (!alloc) {
		ret = -ENOMEM;
		goto out_mm;
	}
	INIT_LIST_HEAD(&alloc->node);
	alloc->pages = kcalloc(page_count, sizeof(*alloc->pages), GFP_KERNEL);
	alloc->baseline_ptes =
		kcalloc(page_count, sizeof(*alloc->baseline_ptes), GFP_KERNEL);
	if (!alloc->pages || !alloc->baseline_ptes) {
		ret = -ENOMEM;
		goto out_alloc;
	}

	for (i = 0; i < page_count; i++) {
		alloc->pages[i] = alloc_page(GFP_HIGHUSER | __GFP_ZERO);
		if (!alloc->pages[i]) {
			ret = -ENOMEM;
			goto out_alloc;
		}
	}

	mmap_write_lock(mm);
	for (i = 0; i < page_count; i++) {
		unsigned long addr = (unsigned long)target_addr + (i * PAGE_SIZE);
		pte_t baseline_pte;
		pte_t alias_pte;

		ret = lkmdbg_pte_read_locked(mm, addr, &baseline_pte, NULL);
		if (ret)
			break;

		alloc->baseline_ptes[i] = baseline_pte;
		alias_pte = lkmdbg_pte_build_alias_pte(alloc->pages[i],
						       baseline_pte, req.prot);
		ret = lkmdbg_pte_rewrite_locked(mm, addr, alias_pte, NULL, NULL);
		if (ret)
			break;
	}
	if (ret) {
		while (i > 0) {
			unsigned long addr = (unsigned long)target_addr +
					     ((i - 1) * PAGE_SIZE);

			lkmdbg_pte_rewrite_locked(mm, addr,
						  alloc->baseline_ptes[i - 1],
						  NULL, NULL);
			i--;
		}
	}
	mmap_write_unlock(mm);
	if (ret)
		goto out_alloc;

	alloc->target_tgid = target_tgid;
	alloc->remote_addr = target_addr;
	alloc->mapped_length = mapped_length;
	alloc->baseline_vm_flags_raw = target_vm_flags;
	alloc->prot = req.prot;
	alloc->flags = req.flags;
	alloc->page_count = page_count;

	mutex_lock(&session->lock);
	if (lkmdbg_remote_alloc_has_overlap_locked(session, target_addr,
						   mapped_length) ||
	    lkmdbg_pte_patch_has_overlap_locked(session, target_addr,
						 mapped_length)) {
		ret = -EEXIST;
	} else {
		alloc->alloc_id = ++session->next_remote_alloc_id;
		list_add_tail(&alloc->node, &session->remote_allocs);
	}
	mutex_unlock(&session->lock);
	if (ret)
		goto out_restore;

	req.alloc_id = alloc->alloc_id;
	req.mapped_length = mapped_length;
	mmput(mm);
	if (copy_to_user(argp, &req, sizeof(req))) {
		mutex_lock(&session->lock);
		if (!list_empty(&alloc->node))
			list_del_init(&alloc->node);
		mutex_unlock(&session->lock);
		lkmdbg_remote_alloc_restore(alloc);
		return -EFAULT;
	}
	return 0;

out_restore:
	lkmdbg_remote_alloc_restore(alloc);
	mmput(mm);
	return ret;
out_alloc:
	lkmdbg_remote_alloc_free_pages(alloc ? alloc->pages : NULL, page_count);
	kfree(alloc ? alloc->pages : NULL);
	kfree(alloc ? alloc->baseline_ptes : NULL);
	kfree(alloc);
out_shell_pages:
	lkmdbg_remote_alloc_put_shell_pages(shell_pages, page_count);
	kfree(shell_pages);
out_mm:
	mmput(mm);
	return ret;
#endif
}

long lkmdbg_remove_remote_alloc(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_remote_alloc_handle_request req;
	struct lkmdbg_remote_alloc *alloc;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (lkmdbg_validate_remote_alloc_handle(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	alloc = lkmdbg_remote_alloc_find_id_locked(session, req.alloc_id);
	if (!alloc) {
		mutex_unlock(&session->lock);
		return -ENOENT;
	}
	list_del_init(&alloc->node);
	mutex_unlock(&session->lock);

	lkmdbg_remote_alloc_fill_handle(alloc, &req);
	lkmdbg_remote_alloc_restore(alloc);
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_query_remote_allocs(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_remote_alloc_query_request req;
	struct lkmdbg_remote_alloc_entry *entries;
	struct lkmdbg_remote_alloc *alloc;
	u32 filled = 0;
	bool started;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_remote_alloc_query(&req);
	if (ret)
		return ret;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	started = req.start_id == 0;
	req.done = 1;
	req.next_id = req.start_id;
	mutex_lock(&session->lock);
	list_for_each_entry(alloc, &session->remote_allocs, node) {
		if (!started) {
			if (alloc->alloc_id <= req.start_id)
				continue;
			started = true;
		}
		if (filled == req.max_entries) {
			req.done = 0;
			break;
		}

		lkmdbg_remote_alloc_fill_entry(&entries[filled], alloc);
		req.next_id = alloc->alloc_id;
		filled++;
	}
	req.entries_filled = filled;
	mutex_unlock(&session->lock);

	if (copy_to_user(u64_to_user_ptr(req.entries_addr), entries,
			 filled * sizeof(*entries))) {
		kfree(entries);
		return -EFAULT;
	}
	kfree(entries);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

void lkmdbg_remote_alloc_release_session(struct lkmdbg_session *session)
{
	struct lkmdbg_remote_alloc *alloc;
	struct lkmdbg_remote_alloc *tmp;
	LIST_HEAD(release_list);

	if (!session)
		return;

	mutex_lock(&session->lock);
	list_for_each_entry_safe(alloc, tmp, &session->remote_allocs, node) {
		list_del_init(&alloc->node);
		list_add_tail(&alloc->node, &release_list);
	}
	mutex_unlock(&session->lock);

	list_for_each_entry_safe(alloc, tmp, &release_list, node) {
		list_del_init(&alloc->node);
		lkmdbg_remote_alloc_restore(alloc);
	}
}
