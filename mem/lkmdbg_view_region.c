#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/pid.h>
#include <linux/refcount.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "lkmdbg_internal.h"

#define LKMDBG_VIEW_REGION_MAX_PIN_PAGES 16U

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
#define LKMDBG_VIEW_REGION_GUP_HAS_LOCKED_ONLY 1
#else
#define LKMDBG_VIEW_REGION_GUP_HAS_LOCKED_ONLY 0
#endif

struct lkmdbg_view_region {
	struct list_head session_node;
	struct list_head global_node;
	refcount_t refs;
	struct lkmdbg_session *session;
	u64 region_id;
	pid_t target_tgid;
	u64 base_addr;
	u64 length;
	u64 read_source_id;
	u64 write_source_id;
	u64 exec_source_id;
	u32 access_mask;
	u32 flags;
	u32 requested_backend;
	u32 active_backend;
	u32 fault_policy;
	u32 sync_policy;
	u32 writeback_policy;
	u32 state;
	u32 read_backing_type;
	u32 write_backing_type;
	u32 exec_backing_type;
	u64 fault_count;
	u8 *read_bytes;
	u8 *write_bytes;
	u8 *exec_bytes;
	u8 *original_bytes;
	struct page **shadow_pages;
	pte_t *shadow_baseline_ptes;
	u32 shadow_page_count;
};

static LIST_HEAD(lkmdbg_view_region_global_list);
static DEFINE_SPINLOCK(lkmdbg_view_region_global_lock);

static void lkmdbg_view_region_refresh_active_state(
	struct lkmdbg_view_region *region)
{
	u32 state = 0;

	if (!region)
		return;

	if (region->read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    region->write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    region->exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL)
		state |= LKMDBG_VIEW_REGION_STATE_ACTIVE;
	if (region->write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    region->exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL)
		state |= LKMDBG_VIEW_REGION_STATE_MUTATED;
	if (region->fault_count)
		state |= LKMDBG_VIEW_REGION_STATE_FAULTED;

	region->state &= ~(LKMDBG_VIEW_REGION_STATE_ACTIVE |
			   LKMDBG_VIEW_REGION_STATE_MUTATED |
			   LKMDBG_VIEW_REGION_STATE_FAULTED);
	region->state |= state;
}

static void lkmdbg_view_region_free_pages(struct page **pages, u32 page_count)
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

static void lkmdbg_view_region_free_shadow_storage(
	struct lkmdbg_view_region *region)
{
	if (!region)
		return;

	lkmdbg_view_region_free_pages(region->shadow_pages,
				      region->shadow_page_count);
	kfree(region->shadow_pages);
	kfree(region->shadow_baseline_ptes);
	kvfree(region->original_bytes);
	region->shadow_pages = NULL;
	region->shadow_baseline_ptes = NULL;
	region->original_bytes = NULL;
	region->shadow_page_count = 0;
}

static bool lkmdbg_view_region_shadow_requested(u32 write_backing_type,
						u32 exec_backing_type)
{
	return write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	       exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL;
}

static const u8 *lkmdbg_view_region_overlay_bytes(
	const struct lkmdbg_view_region *region)
{
	if (!region)
		return NULL;

	switch (region->active_backend) {
	case LKMDBG_VIEW_BACKEND_EXTERNAL_READ:
		if (region->read_backing_type == LKMDBG_VIEW_BACKING_USER_BUFFER)
			return region->read_bytes;
		return NULL;
	case LKMDBG_VIEW_BACKEND_WXSHADOW:
		if (region->read_backing_type == LKMDBG_VIEW_BACKING_USER_BUFFER)
			return region->read_bytes;
		if (region->read_backing_type == LKMDBG_VIEW_BACKING_ORIGINAL)
			return region->original_bytes;
		return NULL;
	default:
		return NULL;
	}
}

static u32 lkmdbg_view_region_shadow_prot(u32 access_mask)
{
	u32 prot = 0;

	if (access_mask & LKMDBG_VIEW_ACCESS_READ)
		prot |= LKMDBG_REMOTE_ALLOC_PROT_READ;
	if (access_mask & LKMDBG_VIEW_ACCESS_WRITE)
		prot |= LKMDBG_REMOTE_ALLOC_PROT_WRITE;
	if (access_mask & LKMDBG_VIEW_ACCESS_EXEC)
		prot |= LKMDBG_REMOTE_ALLOC_PROT_EXEC;
	return prot;
}

static int lkmdbg_view_region_get_mm_by_tgid(pid_t tgid,
					     struct mm_struct **mm_out)
{
	struct task_struct *task;
	struct mm_struct *mm;

	if (tgid <= 0)
		return -ESRCH;

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

static long lkmdbg_view_region_pin_remote_pages(struct mm_struct *mm,
						unsigned long start,
						unsigned int nr_pages,
						struct page **pages)
{
	long ret;
	int locked = 1;

	mmap_read_lock(mm);
#if LKMDBG_VIEW_REGION_GUP_HAS_LOCKED_ONLY
	ret = get_user_pages_remote(mm, start, nr_pages, 0, pages, &locked);
#else
	ret = get_user_pages_remote(mm, start, nr_pages, 0, pages, NULL,
				    &locked);
#endif
	if (locked)
		mmap_read_unlock(mm);
	return ret;
}

static void lkmdbg_view_region_put_remote_pages(struct page **pages,
						u32 page_count)
{
	u32 i;

	for (i = 0; i < page_count; i++) {
		if (!pages[i])
			continue;
		put_page(pages[i]);
		pages[i] = NULL;
	}
}

static int lkmdbg_view_region_read_remote_kernel(struct mm_struct *mm,
						 unsigned long remote_addr,
						 void *dst, size_t length)
{
	struct page *pages[LKMDBG_VIEW_REGION_MAX_PIN_PAGES] = { 0 };
	size_t total_done = 0;
	u8 *out = dst;

	while (total_done < length) {
		unsigned long window_remote;
		unsigned long first_offset;
		size_t window_len;
		unsigned int nr_pages;
		long pinned;
		size_t window_done = 0;
		unsigned int i;

		window_remote = remote_addr + total_done;
		first_offset = offset_in_page(window_remote);
		window_len = min_t(size_t, length - total_done,
				   (LKMDBG_VIEW_REGION_MAX_PIN_PAGES * PAGE_SIZE) -
					   first_offset);
		nr_pages = DIV_ROUND_UP(first_offset + window_len, PAGE_SIZE);
		pinned = lkmdbg_view_region_pin_remote_pages(mm, window_remote,
							     nr_pages, pages);
		if (pinned <= 0)
			return pinned < 0 ? (int)pinned : -EFAULT;
		if ((unsigned int)pinned != nr_pages) {
			lkmdbg_view_region_put_remote_pages(pages, (u32)pinned);
			return -EFAULT;
		}

		for (i = 0; i < nr_pages; i++) {
			unsigned long page_offset = (i == 0) ? first_offset : 0;
			size_t chunk_len;
			void *page_addr;

			chunk_len = min_t(size_t, window_len - window_done,
					  PAGE_SIZE - page_offset);
			page_addr = lkmdbg_kmap_local_page(pages[i]);
			memcpy(out + total_done + window_done,
			       (u8 *)page_addr + page_offset, chunk_len);
			lkmdbg_kunmap_local(pages[i], page_addr);
			put_page(pages[i]);
			pages[i] = NULL;
			window_done += chunk_len;
		}

		total_done += window_done;
		cond_resched();
	}

	return 0;
}

static int lkmdbg_view_region_validate_shadow_range(struct mm_struct *mm,
						    u64 base_addr, u64 length,
						    u32 access_mask)
{
	struct vm_area_struct *vma;
	u64 vm_flags;
	int ret;

	mmap_read_lock(mm);
	ret = lkmdbg_target_vma_lookup_locked(mm, base_addr, length, &vma);
	if (ret) {
		mmap_read_unlock(mm);
		return ret;
	}

	vm_flags = (u64)vma->vm_flags;
	if ((access_mask & LKMDBG_VIEW_ACCESS_READ) &&
	    !(vm_flags & (VM_READ | VM_MAYREAD)))
		ret = -EACCES;
	else if ((access_mask & LKMDBG_VIEW_ACCESS_WRITE) &&
		 !(vm_flags & (VM_WRITE | VM_MAYWRITE)))
		ret = -EACCES;
	else if ((access_mask & LKMDBG_VIEW_ACCESS_EXEC) &&
		 !(vm_flags & (VM_EXEC | VM_MAYEXEC)))
		ret = -EACCES;
	else if (vm_flags & (VM_PFNMAP | VM_IO))
		ret = -EOPNOTSUPP;
	else
		ret = 0;

	mmap_read_unlock(mm);
	return ret;
}

static void lkmdbg_view_region_sync_shadow_page(struct page *page, void *page_addr,
						u32 prot)
{
	flush_dcache_page(page);
	if ((prot & LKMDBG_REMOTE_ALLOC_PROT_EXEC) &&
	    lkmdbg_symbols.flush_icache_range)
		lkmdbg_flush_icache_runtime((unsigned long)page_addr,
					    (unsigned long)page_addr + PAGE_SIZE);
}

static int lkmdbg_view_region_install_wxshadow(
	struct lkmdbg_view_region *region, u32 access_mask,
	const u8 *shadow_bytes)
{
#ifndef CONFIG_ARM64
	(void)region;
	(void)access_mask;
	(void)shadow_bytes;
	return -EOPNOTSUPP;
#else
	struct mm_struct *mm = NULL;
	struct page **shadow_pages = NULL;
	pte_t *baseline_ptes = NULL;
	u8 *original_bytes = NULL;
	u64 mapped_length;
	u32 shadow_prot;
	u32 page_count;
	u32 i;
	int ret;

	if (!region || !shadow_bytes)
		return -EINVAL;

	mapped_length = region->length;
	page_count = (u32)(mapped_length >> PAGE_SHIFT);
	shadow_prot = lkmdbg_view_region_shadow_prot(access_mask);
	if (!page_count || !shadow_prot)
		return -EINVAL;

	ret = lkmdbg_view_region_get_mm_by_tgid(region->target_tgid, &mm);
	if (ret)
		return ret;

	ret = lkmdbg_view_region_validate_shadow_range(
		mm, region->base_addr, mapped_length, access_mask);
	if (ret)
		goto out_mm;

	shadow_pages = kcalloc(page_count, sizeof(*shadow_pages), GFP_KERNEL);
	baseline_ptes = kcalloc(page_count, sizeof(*baseline_ptes), GFP_KERNEL);
	original_bytes = kvmalloc(mapped_length, GFP_KERNEL);
	if (!shadow_pages || !baseline_ptes || !original_bytes) {
		ret = -ENOMEM;
		goto out_free;
	}

	ret = lkmdbg_view_region_read_remote_kernel(
		mm, (unsigned long)region->base_addr, original_bytes,
		(size_t)mapped_length);
	if (ret)
		goto out_free;

	for (i = 0; i < page_count; i++) {
		void *page_addr;

		shadow_pages[i] = alloc_page(GFP_HIGHUSER | __GFP_ZERO);
		if (!shadow_pages[i]) {
			ret = -ENOMEM;
			goto out_free;
		}

		page_addr = lkmdbg_kmap_local_page(shadow_pages[i]);
		memcpy(page_addr, shadow_bytes + ((size_t)i * PAGE_SIZE),
		       PAGE_SIZE);
		lkmdbg_view_region_sync_shadow_page(shadow_pages[i], page_addr,
						    shadow_prot);
		lkmdbg_kunmap_local(shadow_pages[i], page_addr);
	}

	mmap_write_lock(mm);
	for (i = 0; i < page_count; i++) {
		unsigned long addr = (unsigned long)region->base_addr +
				     ((unsigned long)i * PAGE_SIZE);
		pte_t baseline_pte;
		pte_t alias_pte;

		ret = lkmdbg_pte_read_locked(mm, addr, &baseline_pte, NULL);
		if (ret)
			break;
		baseline_ptes[i] = baseline_pte;
		alias_pte = lkmdbg_pte_build_alias_pte(shadow_pages[i],
						       baseline_pte,
						       shadow_prot);
		ret = lkmdbg_pte_rewrite_locked(mm, addr, alias_pte, NULL, NULL);
		if (ret)
			break;
	}
	if (ret) {
		while (i > 0) {
			unsigned long addr = (unsigned long)region->base_addr +
					     ((unsigned long)(i - 1) * PAGE_SIZE);

			lkmdbg_pte_rewrite_locked(mm, addr,
						  baseline_ptes[i - 1],
						  NULL, NULL);
			i--;
		}
	}
	mmap_write_unlock(mm);
	if (ret)
		goto out_free;

	region->shadow_pages = shadow_pages;
	region->shadow_baseline_ptes = baseline_ptes;
	region->original_bytes = original_bytes;
	region->shadow_page_count = page_count;
	mmput(mm);
	return 0;

out_free:
	lkmdbg_view_region_free_pages(shadow_pages, page_count);
	kfree(shadow_pages);
	kfree(baseline_ptes);
	kvfree(original_bytes);
out_mm:
	mmput(mm);
	return ret;
#endif
}

static int lkmdbg_view_region_restore_wxshadow(struct lkmdbg_view_region *region)
{
#ifndef CONFIG_ARM64
	lkmdbg_view_region_free_shadow_storage(region);
	return -EOPNOTSUPP;
#else
	struct mm_struct *mm = NULL;
	int ret = 0;
	u32 i;

	if (!region || !region->shadow_pages || !region->shadow_baseline_ptes ||
	    !region->shadow_page_count) {
		lkmdbg_view_region_free_shadow_storage(region);
		return 0;
	}

	if (!lkmdbg_view_region_get_mm_by_tgid(region->target_tgid, &mm)) {
		mmap_write_lock(mm);
		for (i = 0; i < region->shadow_page_count; i++) {
			unsigned long addr = (unsigned long)region->base_addr +
					     ((unsigned long)i * PAGE_SIZE);
			int page_ret;

			page_ret = lkmdbg_pte_rewrite_locked(
				mm, addr, region->shadow_baseline_ptes[i], NULL,
				NULL);
			if (page_ret && !ret)
				ret = page_ret;
		}
		mmap_write_unlock(mm);
		mmput(mm);
	} else {
		ret = -ESRCH;
	}

	lkmdbg_view_region_free_shadow_storage(region);
	return ret;
#endif
}

static void lkmdbg_view_region_destroy(struct lkmdbg_view_region *region)
{
	if (!region)
		return;

	kvfree(region->read_bytes);
	kvfree(region->write_bytes);
	kvfree(region->exec_bytes);
	lkmdbg_view_region_free_shadow_storage(region);
	kfree(region);
}

static void lkmdbg_view_region_get(struct lkmdbg_view_region *region)
{
	refcount_inc(&region->refs);
}

static void lkmdbg_view_region_put(struct lkmdbg_view_region *region)
{
	if (!region)
		return;
	if (refcount_dec_and_test(&region->refs))
		lkmdbg_view_region_destroy(region);
}

static struct lkmdbg_view_region *
lkmdbg_find_view_region_locked(struct lkmdbg_session *session, u64 region_id)
{
	struct lkmdbg_view_region *region;

	list_for_each_entry(region, &session->view_regions, session_node) {
		if (region->region_id == region_id)
			return region;
	}

	return NULL;
}

static bool lkmdbg_view_region_ranges_overlap(u64 start_a, u64 len_a, u64 start_b,
					      u64 len_b)
{
	u64 end_a = start_a + len_a;
	u64 end_b = start_b + len_b;

	return start_a < end_b && start_b < end_a;
}

static struct lkmdbg_view_region *
lkmdbg_view_region_lookup(pid_t target_tgid, u64 addr)
{
	struct lkmdbg_view_region *region;
	unsigned long irqflags;

	if (target_tgid <= 0)
		return NULL;

	spin_lock_irqsave(&lkmdbg_view_region_global_lock, irqflags);
	list_for_each_entry(region, &lkmdbg_view_region_global_list, global_node) {
		if (region->target_tgid != target_tgid)
			continue;
		if (addr < region->base_addr || addr >= region->base_addr + region->length)
			continue;
		if (!lkmdbg_view_region_overlay_bytes(region)) {
			spin_unlock_irqrestore(&lkmdbg_view_region_global_lock,
					       irqflags);
			return NULL;
		}
		lkmdbg_view_region_get(region);
		spin_unlock_irqrestore(&lkmdbg_view_region_global_lock, irqflags);
		return region;
	}
	spin_unlock_irqrestore(&lkmdbg_view_region_global_lock, irqflags);
	return NULL;
}

static void lkmdbg_view_region_overlay_user(pid_t target_tgid, u64 remote_addr,
					    char __user *dst, size_t len)
{
	while (len > 0) {
		struct lkmdbg_view_region *region;
		size_t step;

		region = lkmdbg_view_region_lookup(target_tgid, remote_addr);
		if (!region) {
			step = min_t(size_t, len,
				     PAGE_SIZE - ((size_t)remote_addr & (PAGE_SIZE - 1)));
			remote_addr += step;
			dst += step;
			len -= step;
			continue;
		}

		step = min_t(size_t, len,
			     (size_t)((region->base_addr + region->length) -
				      remote_addr));
		if (copy_to_user(dst, lkmdbg_view_region_overlay_bytes(region) +
				 (size_t)(remote_addr - region->base_addr),
				 step)) {
			lkmdbg_view_region_put(region);
			return;
		}
		WRITE_ONCE(region->fault_count, region->fault_count + 1);
		remote_addr += step;
		dst += step;
		len -= step;
		lkmdbg_view_region_put(region);
	}
}

static void lkmdbg_view_region_overlay_kernel(pid_t target_tgid, u64 remote_addr,
					      void *dst, size_t len)
{
	u8 *buf = dst;

	while (len > 0) {
		struct lkmdbg_view_region *region;
		size_t step;

		region = lkmdbg_view_region_lookup(target_tgid, remote_addr);
		if (!region) {
			step = min_t(size_t, len,
				     PAGE_SIZE - ((size_t)remote_addr & (PAGE_SIZE - 1)));
			remote_addr += step;
			buf += step;
			len -= step;
			continue;
		}

		step = min_t(size_t, len,
			     (size_t)((region->base_addr + region->length) -
				      remote_addr));
		memcpy(buf, lkmdbg_view_region_overlay_bytes(region) +
			      (size_t)(remote_addr - region->base_addr),
		       step);
		WRITE_ONCE(region->fault_count, region->fault_count + 1);
		remote_addr += step;
		buf += step;
		len -= step;
		lkmdbg_view_region_put(region);
	}
}

static int lkmdbg_view_region_select_external_read(
	u32 access_mask, u32 read_backing_type, u32 write_backing_type,
	u32 exec_backing_type, u32 fault_policy, u32 sync_policy,
	u32 writeback_policy, u32 *active_backend_out)
{
	int ret;

	if (!(access_mask & LKMDBG_VIEW_ACCESS_READ))
		return -EOPNOTSUPP;
	if (read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL &&
	    read_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER)
		return -EOPNOTSUPP;
	if (write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	    exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL)
		return -EOPNOTSUPP;
	if (fault_policy != LKMDBG_VIEW_FAULT_POLICY_TRAP_ONLY)
		return -EOPNOTSUPP;
	if (sync_policy != LKMDBG_VIEW_SYNC_NONE)
		return -EOPNOTSUPP;
	if (writeback_policy != LKMDBG_VIEW_WRITEBACK_DISCARD)
		return -EOPNOTSUPP;

	ret = lkmdbg_external_read_hooks_ensure();
	if (ret)
		return ret;

	*active_backend_out = LKMDBG_VIEW_BACKEND_EXTERNAL_READ;
	return 0;
}

static int lkmdbg_view_region_select_wxshadow(
	u32 access_mask, u32 read_backing_type, u32 write_backing_type,
	u32 exec_backing_type, u32 fault_policy, u32 sync_policy,
	u32 writeback_policy, u32 *active_backend_out)
{
	int ret;
	u32 shadow_count = 0;

#ifndef CONFIG_ARM64
	(void)access_mask;
	(void)read_backing_type;
	(void)write_backing_type;
	(void)exec_backing_type;
	(void)fault_policy;
	(void)sync_policy;
	(void)writeback_policy;
	(void)active_backend_out;
	return -EOPNOTSUPP;
#else
	if (read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL &&
	    read_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER)
		return -EOPNOTSUPP;
	if (write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL &&
	    write_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER)
		return -EOPNOTSUPP;
	if (exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL &&
	    exec_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER)
		return -EOPNOTSUPP;
	if (fault_policy != LKMDBG_VIEW_FAULT_POLICY_TRAP_ONLY)
		return -EOPNOTSUPP;
	if (sync_policy != LKMDBG_VIEW_SYNC_NONE)
		return -EOPNOTSUPP;
	if (writeback_policy != LKMDBG_VIEW_WRITEBACK_DISCARD)
		return -EOPNOTSUPP;
	if (read_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL &&
	    !(access_mask & LKMDBG_VIEW_ACCESS_READ))
		return -EOPNOTSUPP;
	if (write_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL) {
		if (!(access_mask & LKMDBG_VIEW_ACCESS_WRITE))
			return -EOPNOTSUPP;
		shadow_count++;
	}
	if (exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL) {
		if (!(access_mask & LKMDBG_VIEW_ACCESS_EXEC))
			return -EOPNOTSUPP;
		shadow_count++;
	}
	if (shadow_count != 1)
		return -EOPNOTSUPP;

	if (access_mask & LKMDBG_VIEW_ACCESS_READ) {
		ret = lkmdbg_external_read_hooks_ensure();
		if (ret)
			return ret;
	}

	*active_backend_out = LKMDBG_VIEW_BACKEND_WXSHADOW;
	return 0;
#endif
}

static int lkmdbg_view_region_prepare_backend(
	u32 requested_backend, u32 access_mask, u32 read_backing_type,
	u32 write_backing_type, u32 exec_backing_type, u32 fault_policy,
	u32 sync_policy, u32 writeback_policy, u32 *active_backend_out)
{
	switch (requested_backend) {
	case LKMDBG_VIEW_BACKEND_AUTO:
		if (!lkmdbg_view_region_select_external_read(
			    access_mask, read_backing_type, write_backing_type,
			    exec_backing_type, fault_policy, sync_policy,
			    writeback_policy, active_backend_out))
			return 0;
		return lkmdbg_view_region_select_wxshadow(
			access_mask, read_backing_type, write_backing_type,
			exec_backing_type, fault_policy, sync_policy,
			writeback_policy, active_backend_out);
	case LKMDBG_VIEW_BACKEND_EXTERNAL_READ:
		return lkmdbg_view_region_select_external_read(
			access_mask, read_backing_type, write_backing_type,
			exec_backing_type, fault_policy, sync_policy,
			writeback_policy, active_backend_out);
	case LKMDBG_VIEW_BACKEND_WXSHADOW:
		return lkmdbg_view_region_select_wxshadow(
			access_mask, read_backing_type, write_backing_type,
			exec_backing_type, fault_policy, sync_policy,
			writeback_policy, active_backend_out);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
lkmdbg_validate_view_region_request(const struct lkmdbg_view_region_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->base_addr || !req->length)
		return -EINVAL;
	if (req->base_addr & ~PAGE_MASK)
		return -EINVAL;
	if (req->length & ~PAGE_MASK)
		return -EINVAL;
	if (req->base_addr + req->length < req->base_addr)
		return -EINVAL;
	if (!req->access_mask ||
	    (req->access_mask &
	     ~(LKMDBG_VIEW_ACCESS_READ | LKMDBG_VIEW_ACCESS_WRITE |
	       LKMDBG_VIEW_ACCESS_EXEC)))
		return -EINVAL;
	if (req->flags || req->reserved0 || req->reserved1)
		return -EINVAL;
	if (req->backend > LKMDBG_VIEW_BACKEND_GENERIC_SWITCH)
		return -EINVAL;
	if (req->fault_policy > LKMDBG_VIEW_FAULT_POLICY_EMULATE_WRITE)
		return -EINVAL;
	if (req->sync_policy > LKMDBG_VIEW_SYNC_WRITE_TO_ALL)
		return -EINVAL;
	if (req->writeback_policy > LKMDBG_VIEW_WRITEBACK_COMMIT_EXEC_VIEW)
		return -EINVAL;
	return 0;
}

static int lkmdbg_validate_view_region_handle_request(
	const struct lkmdbg_view_region_handle_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->region_id || req->flags || req->reserved0)
		return -EINVAL;
	return 0;
}

static int lkmdbg_validate_view_policy_request(
	const struct lkmdbg_view_policy_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->region_id || req->flags || req->reserved0)
		return -EINVAL;
	if (req->backend > LKMDBG_VIEW_BACKEND_GENERIC_SWITCH)
		return -EINVAL;
	if (req->fault_policy > LKMDBG_VIEW_FAULT_POLICY_EMULATE_WRITE)
		return -EINVAL;
	if (req->sync_policy > LKMDBG_VIEW_SYNC_WRITE_TO_ALL)
		return -EINVAL;
	if (req->writeback_policy > LKMDBG_VIEW_WRITEBACK_COMMIT_EXEC_VIEW)
		return -EINVAL;
	return 0;
}

static int
lkmdbg_validate_view_backing_request(const struct lkmdbg_view_backing_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->region_id || req->flags || req->reserved0 || req->reserved1)
		return -EINVAL;
	if (req->view_kind < LKMDBG_VIEW_KIND_READ ||
	    req->view_kind > LKMDBG_VIEW_KIND_EXEC)
		return -EINVAL;
	if (req->backing_type > LKMDBG_VIEW_BACKING_REMOTE_ALLOC)
		return -EINVAL;
	if (req->backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER &&
	    (req->source_addr & ~PAGE_MASK))
		return -EINVAL;
	if (req->source_addr + req->source_length < req->source_addr)
		return -EINVAL;
	return 0;
}

static int lkmdbg_validate_view_region_query_request(
	const struct lkmdbg_view_region_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->entries_addr || !req->max_entries)
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	return 0;
}

long lkmdbg_create_view_region(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_view_region_request req;
	struct lkmdbg_view_region *region;
	pid_t target_tgid;
	u32 active_backend;
	int ret;
	unsigned long irqflags;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (lkmdbg_validate_view_region_request(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	target_tgid = session->target_tgid;
	mutex_unlock(&session->lock);
	if (target_tgid <= 0)
		return -ENODEV;

	ret = lkmdbg_view_region_prepare_backend(
		req.backend, req.access_mask, LKMDBG_VIEW_BACKING_ORIGINAL,
		LKMDBG_VIEW_BACKING_ORIGINAL, LKMDBG_VIEW_BACKING_ORIGINAL,
		req.fault_policy, req.sync_policy, req.writeback_policy,
		&active_backend);
	if (ret) {
		if (req.backend == LKMDBG_VIEW_BACKEND_AUTO ||
		    req.backend == LKMDBG_VIEW_BACKEND_WXSHADOW)
			active_backend = req.backend;
		else
			return ret;
	}

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	INIT_LIST_HEAD(&region->session_node);
	INIT_LIST_HEAD(&region->global_node);
	refcount_set(&region->refs, 1);
	region->session = session;
	region->target_tgid = target_tgid;
	region->base_addr = req.base_addr;
	region->length = req.length;
	region->access_mask = req.access_mask;
	region->flags = req.flags;
	region->requested_backend = req.backend;
	region->active_backend = active_backend;
	region->fault_policy = req.fault_policy;
	region->sync_policy = req.sync_policy;
	region->writeback_policy = req.writeback_policy;
	region->read_backing_type = LKMDBG_VIEW_BACKING_ORIGINAL;
	region->write_backing_type = LKMDBG_VIEW_BACKING_ORIGINAL;
	region->exec_backing_type = LKMDBG_VIEW_BACKING_ORIGINAL;

	mutex_lock(&session->lock);
	if (session->target_tgid != target_tgid) {
		mutex_unlock(&session->lock);
		kfree(region);
		return -ESTALE;
	}
	{
		struct lkmdbg_view_region *iter;

		list_for_each_entry(iter, &session->view_regions, session_node) {
			if (lkmdbg_view_region_ranges_overlap(
				    iter->base_addr, iter->length, req.base_addr,
				    req.length)) {
				mutex_unlock(&session->lock);
				kfree(region);
				return -EEXIST;
			}
		}
	}
	session->next_view_region_id++;
	region->region_id = session->next_view_region_id;
	req.region_id = region->region_id;
	list_add_tail(&region->session_node, &session->view_regions);
	mutex_unlock(&session->lock);

	spin_lock_irqsave(&lkmdbg_view_region_global_lock, irqflags);
	list_add_tail(&region->global_node, &lkmdbg_view_region_global_list);
	spin_unlock_irqrestore(&lkmdbg_view_region_global_lock, irqflags);

	if (copy_to_user(argp, &req, sizeof(req))) {
		mutex_lock(&session->lock);
		if (!list_empty(&region->session_node))
			list_del_init(&region->session_node);
		mutex_unlock(&session->lock);
		spin_lock_irqsave(&lkmdbg_view_region_global_lock, irqflags);
		if (!list_empty(&region->global_node))
			list_del_init(&region->global_node);
		spin_unlock_irqrestore(&lkmdbg_view_region_global_lock, irqflags);
		lkmdbg_view_region_put(region);
		return -EFAULT;
	}
	return 0;
}

long lkmdbg_remove_view_region(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_view_region_handle_request req;
	struct lkmdbg_view_region *region;
	unsigned long irqflags;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (lkmdbg_validate_view_region_handle_request(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	region = lkmdbg_find_view_region_locked(session, req.region_id);
	if (!region) {
		mutex_unlock(&session->lock);
		return -ENOENT;
	}
	list_del_init(&region->session_node);
	mutex_unlock(&session->lock);

	spin_lock_irqsave(&lkmdbg_view_region_global_lock, irqflags);
	if (!list_empty(&region->global_node))
		list_del_init(&region->global_node);
	spin_unlock_irqrestore(&lkmdbg_view_region_global_lock, irqflags);

	(void)lkmdbg_view_region_restore_wxshadow(region);
	lkmdbg_view_region_put(region);
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

long lkmdbg_set_view_backing(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_view_backing_request req;
	struct lkmdbg_view_region *region;
	u8 *backing = NULL;
	u8 *old_backing = NULL;
	u32 read_backing_type;
	u32 write_backing_type;
	u32 exec_backing_type;
	u32 active_backend;
	u32 *kind_type = NULL;
	u8 **kind_bytes = NULL;
	u64 *kind_source_id = NULL;
	u64 requested_backend;
	u8 *new_read_bytes;
	u8 *new_write_bytes;
	u8 *new_exec_bytes;
	u64 new_read_source_id;
	u64 new_write_source_id;
	u64 new_exec_source_id;
	u64 new_source_id = 0;
	bool had_shadow;
	bool need_shadow;
	bool target_shadow_changed = false;
	bool updated_target_shadow = false;
	int ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (lkmdbg_validate_view_backing_request(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	region = lkmdbg_find_view_region_locked(session, req.region_id);
	if (region)
		lkmdbg_view_region_get(region);
	mutex_unlock(&session->lock);
	if (!region)
		return -ENOENT;

	if (req.backing_type == LKMDBG_VIEW_BACKING_USER_BUFFER) {
		if (!req.source_addr || req.source_length != region->length) {
			ret = -EINVAL;
			goto out_put;
		}
		backing = kvmalloc(region->length, GFP_KERNEL);
		if (!backing) {
			ret = -ENOMEM;
			goto out_put;
		}
		if (copy_from_user(backing, u64_to_user_ptr(req.source_addr),
				   region->length)) {
			ret = -EFAULT;
			goto out_free;
		}
	}

	mutex_lock(&session->lock);
	requested_backend = region->requested_backend;
	read_backing_type = region->read_backing_type;
	write_backing_type = region->write_backing_type;
	exec_backing_type = region->exec_backing_type;
	new_read_bytes = region->read_bytes;
	new_write_bytes = region->write_bytes;
	new_exec_bytes = region->exec_bytes;
	new_read_source_id = region->read_source_id;
	new_write_source_id = region->write_source_id;
	new_exec_source_id = region->exec_source_id;
	switch (req.view_kind) {
	case LKMDBG_VIEW_KIND_READ:
		kind_type = &read_backing_type;
		kind_bytes = &new_read_bytes;
		kind_source_id = &new_read_source_id;
		break;
	case LKMDBG_VIEW_KIND_WRITE:
		kind_type = &write_backing_type;
		kind_bytes = &new_write_bytes;
		kind_source_id = &new_write_source_id;
		updated_target_shadow = true;
		break;
	case LKMDBG_VIEW_KIND_EXEC:
		kind_type = &exec_backing_type;
		kind_bytes = &new_exec_bytes;
		kind_source_id = &new_exec_source_id;
		updated_target_shadow = true;
		break;
	default:
		mutex_unlock(&session->lock);
		ret = -EINVAL;
		goto out_free;
	}
	*kind_type = req.backing_type;
	if (req.backing_type == LKMDBG_VIEW_BACKING_ORIGINAL) {
		*kind_bytes = NULL;
		*kind_source_id = 0;
	} else if (req.backing_type == LKMDBG_VIEW_BACKING_USER_BUFFER) {
		new_source_id = session->next_view_source_id + 1;
		*kind_bytes = backing;
		*kind_source_id = new_source_id;
	} else {
		mutex_unlock(&session->lock);
		ret = -EOPNOTSUPP;
		goto out_free;
	}
	ret = lkmdbg_view_region_prepare_backend(
		(u32)requested_backend, region->access_mask, read_backing_type,
		write_backing_type, exec_backing_type, region->fault_policy,
		region->sync_policy, region->writeback_policy, &active_backend);
	if (ret) {
		mutex_unlock(&session->lock);
		goto out_free;
	}

	had_shadow = region->shadow_pages && region->shadow_page_count;
	need_shadow = active_backend == LKMDBG_VIEW_BACKEND_WXSHADOW &&
		      lkmdbg_view_region_shadow_requested(write_backing_type,
							 exec_backing_type);
	target_shadow_changed =
		(region->active_backend != active_backend) ||
		(had_shadow != need_shadow);
	if (updated_target_shadow &&
	    (req.backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ||
	     req.backing_type != ((req.view_kind == LKMDBG_VIEW_KIND_WRITE) ?
					  region->write_backing_type :
					  region->exec_backing_type)))
		target_shadow_changed = true;

	if (had_shadow && (!need_shadow || target_shadow_changed)) {
		ret = lkmdbg_view_region_restore_wxshadow(region);
		if (ret) {
			mutex_unlock(&session->lock);
			goto out_free;
		}
	}

	if (need_shadow && (!had_shadow || target_shadow_changed)) {
		const u8 *shadow_source =
			exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ?
				new_exec_bytes :
				new_write_bytes;

		ret = lkmdbg_view_region_install_wxshadow(
			region, region->access_mask, shadow_source);
		if (ret) {
			if (had_shadow) {
				const u8 *old_shadow_source =
					region->exec_backing_type !=
							LKMDBG_VIEW_BACKING_ORIGINAL ?
						region->exec_bytes :
						region->write_bytes;

				(void)lkmdbg_view_region_install_wxshadow(
					region, region->access_mask,
					old_shadow_source);
			}
			mutex_unlock(&session->lock);
			goto out_free;
		}
	}

	if (new_source_id)
		session->next_view_source_id = new_source_id;
	req.source_id = new_source_id;
	switch (req.view_kind) {
	case LKMDBG_VIEW_KIND_READ:
		old_backing = region->read_bytes;
		region->read_bytes = new_read_bytes;
		region->read_source_id = new_read_source_id;
		region->read_backing_type = read_backing_type;
		break;
	case LKMDBG_VIEW_KIND_WRITE:
		old_backing = region->write_bytes;
		region->write_bytes = new_write_bytes;
		region->write_source_id = new_write_source_id;
		region->write_backing_type = write_backing_type;
		break;
	case LKMDBG_VIEW_KIND_EXEC:
		old_backing = region->exec_bytes;
		region->exec_bytes = new_exec_bytes;
		region->exec_source_id = new_exec_source_id;
		region->exec_backing_type = exec_backing_type;
		break;
	}
	region->active_backend = active_backend;
	lkmdbg_view_region_refresh_active_state(region);
	mutex_unlock(&session->lock);
	if (old_backing != backing)
		kvfree(old_backing);
	backing = NULL;

	if (copy_to_user(argp, &req, sizeof(req))) {
		ret = -EFAULT;
		goto out_put;
	}
	goto out_put;

out_free:
	kvfree(backing);
out_put:
	lkmdbg_view_region_put(region);
	return ret;
}

long lkmdbg_set_view_policy(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_view_policy_request req;
	struct lkmdbg_view_region *region;
	u32 active_backend;
	bool had_shadow;
	bool need_shadow;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (lkmdbg_validate_view_policy_request(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	region = lkmdbg_find_view_region_locked(session, req.region_id);
	if (!region) {
		mutex_unlock(&session->lock);
		return -ENOENT;
	}
	ret = lkmdbg_view_region_prepare_backend(
		req.backend, region->access_mask, region->read_backing_type,
		region->write_backing_type, region->exec_backing_type,
		req.fault_policy, req.sync_policy, req.writeback_policy,
		&active_backend);
	if (ret) {
		if (!lkmdbg_view_region_shadow_requested(
			    region->write_backing_type, region->exec_backing_type) &&
		    (req.backend == LKMDBG_VIEW_BACKEND_AUTO ||
		     req.backend == LKMDBG_VIEW_BACKEND_WXSHADOW))
			active_backend = req.backend;
		else {
			mutex_unlock(&session->lock);
			return ret;
		}
	}

	had_shadow = region->shadow_pages && region->shadow_page_count;
	need_shadow = active_backend == LKMDBG_VIEW_BACKEND_WXSHADOW &&
		      lkmdbg_view_region_shadow_requested(
			      region->write_backing_type,
			      region->exec_backing_type);
	if (had_shadow && !need_shadow) {
		ret = lkmdbg_view_region_restore_wxshadow(region);
		if (ret) {
			mutex_unlock(&session->lock);
			return ret;
		}
	} else if (!had_shadow && need_shadow) {
		const u8 *shadow_source =
			region->exec_backing_type != LKMDBG_VIEW_BACKING_ORIGINAL ?
				region->exec_bytes :
				region->write_bytes;

		ret = lkmdbg_view_region_install_wxshadow(
			region, region->access_mask, shadow_source);
		if (ret) {
			mutex_unlock(&session->lock);
			return ret;
		}
	}
	region->requested_backend = req.backend;
	region->active_backend = active_backend;
	region->fault_policy = req.fault_policy;
	region->sync_policy = req.sync_policy;
	region->writeback_policy = req.writeback_policy;
	mutex_unlock(&session->lock);
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

long lkmdbg_query_view_regions(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_view_region_query_request req;
	struct lkmdbg_view_region_entry *entries;
	struct lkmdbg_view_region *region;
	u32 filled = 0;
	bool done = true;
	u64 next_id = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (lkmdbg_validate_view_region_query_request(&req))
		return -EINVAL;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	mutex_lock(&session->lock);
	list_for_each_entry(region, &session->view_regions, session_node) {
		if (region->region_id < req.start_id)
			continue;
		if (filled >= req.max_entries) {
			done = false;
			next_id = region->region_id;
			break;
		}
		entries[filled].region_id = region->region_id;
		entries[filled].base_addr = region->base_addr;
		entries[filled].length = region->length;
		if (region->shadow_pages && region->shadow_baseline_ptes &&
		    region->shadow_page_count) {
			entries[filled].original_pte =
				pte_val(region->shadow_baseline_ptes[0]);
			entries[filled].current_pte = pte_val(
				lkmdbg_pte_build_alias_pte(
					region->shadow_pages[0],
					region->shadow_baseline_ptes[0],
					lkmdbg_view_region_shadow_prot(
						region->access_mask)));
		}
		entries[filled].fault_count = region->fault_count;
		entries[filled].read_source_id = region->read_source_id;
		entries[filled].write_source_id = region->write_source_id;
		entries[filled].exec_source_id = region->exec_source_id;
		entries[filled].access_mask = region->access_mask;
		entries[filled].flags = region->flags;
		entries[filled].requested_backend = region->requested_backend;
		entries[filled].active_backend = region->active_backend;
		entries[filled].fault_policy = region->fault_policy;
		entries[filled].sync_policy = region->sync_policy;
		entries[filled].writeback_policy = region->writeback_policy;
		entries[filled].state = region->state;
		entries[filled].read_backing_type = region->read_backing_type;
		entries[filled].write_backing_type = region->write_backing_type;
		entries[filled].exec_backing_type = region->exec_backing_type;
		filled++;
	}
	mutex_unlock(&session->lock);

	req.entries_filled = filled;
	req.done = done ? 1U : 0U;
	req.next_id = done ? 0 : next_id;

	if (copy_to_user(u64_to_user_ptr(req.entries_addr), entries,
			 sizeof(*entries) * filled)) {
		kfree(entries);
		return -EFAULT;
	}
	kfree(entries);
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

bool lkmdbg_view_region_blocks_target_change(struct lkmdbg_session *session)
{
	bool blocked;

	mutex_lock(&session->lock);
	blocked = !list_empty(&session->view_regions);
	mutex_unlock(&session->lock);
	return blocked;
}

void lkmdbg_view_region_release(struct lkmdbg_session *session)
{
	struct lkmdbg_view_region *region;
	unsigned long irqflags;

	if (!session)
		return;

	for (;;) {
		mutex_lock(&session->lock);
		if (list_empty(&session->view_regions)) {
			mutex_unlock(&session->lock);
			return;
		}
		region = list_first_entry(&session->view_regions,
					  struct lkmdbg_view_region, session_node);
		list_del_init(&region->session_node);
		mutex_unlock(&session->lock);

		spin_lock_irqsave(&lkmdbg_view_region_global_lock, irqflags);
		if (!list_empty(&region->global_node))
			list_del_init(&region->global_node);
		spin_unlock_irqrestore(&lkmdbg_view_region_global_lock, irqflags);
		(void)lkmdbg_view_region_restore_wxshadow(region);
		lkmdbg_view_region_put(region);
	}
}

void lkmdbg_view_region_overlay_process_vm_read(
	pid_t pid, const struct iovec __user *lvec, unsigned long liovcnt,
	const struct iovec __user *rvec, unsigned long riovcnt, ssize_t bytes_done)
{
	struct iovec local_iov = { 0 };
	struct iovec remote_iov = { 0 };
	unsigned long li = 0;
	unsigned long ri = 0;
	size_t local_off = 0;
	size_t remote_off = 0;
	ssize_t remaining = bytes_done;

	while (remaining > 0) {
		size_t chunk;

		while (li < liovcnt &&
		       (!local_iov.iov_len || local_off >= local_iov.iov_len)) {
			if (copy_from_user(&local_iov, &lvec[li], sizeof(local_iov)))
				return;
			li++;
			local_off = 0;
		}
		while (ri < riovcnt &&
		       (!remote_iov.iov_len || remote_off >= remote_iov.iov_len)) {
			if (copy_from_user(&remote_iov, &rvec[ri], sizeof(remote_iov)))
				return;
			ri++;
			remote_off = 0;
		}
		if (!local_iov.iov_len || !remote_iov.iov_len)
			return;

		chunk = min_t(size_t, (size_t)remaining,
			      min_t(size_t, local_iov.iov_len - local_off,
				    remote_iov.iov_len - remote_off));
		lkmdbg_view_region_overlay_user(
			pid, (u64)(unsigned long)remote_iov.iov_base + remote_off,
			(char __user *)local_iov.iov_base + local_off, chunk);
		remaining -= (ssize_t)chunk;
		local_off += chunk;
		remote_off += chunk;
	}
}

void lkmdbg_view_region_overlay_remote_vm_read(struct mm_struct *mm,
					       unsigned long addr, void *buf,
					       int len)
{
	struct task_struct *owner;
	pid_t target_tgid;

	if (!mm || !buf || len <= 0)
		return;

	owner = READ_ONCE(mm->owner);
	if (!owner)
		return;

	target_tgid = owner->tgid;
	lkmdbg_view_region_overlay_kernel(target_tgid, addr, buf, (size_t)len);
}
