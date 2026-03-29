#include <linux/anon_inodes.h>
#include <linux/completion.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmap_lock.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/refcount.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "lkmdbg_internal.h"

#define LKMDBG_REMOTE_MAP_MAX_BYTES (16U * 1024U * 1024U)
#define LKMDBG_REMOTE_MAP_VALID_PROT                                         \
	(LKMDBG_REMOTE_MAP_PROT_READ | LKMDBG_REMOTE_MAP_PROT_WRITE |        \
	 LKMDBG_REMOTE_MAP_PROT_EXEC)
#define LKMDBG_REMOTE_MAP_VALID_FLAGS                                        \
	(LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET |                           \
	 LKMDBG_REMOTE_MAP_FLAG_FIXED_TARGET |                             \
	 LKMDBG_REMOTE_MAP_FLAG_STEALTH_LOCAL |                            \
	 LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET)
#define LKMDBG_REMOTE_MAP_DEFAULT_TIMEOUT_MS 1000U
#define LKMDBG_TASK_WORK_NOTIFY_RESUME 1U

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
#define LKMDBG_REMOTE_GUP_HAS_LOCKED_ONLY 1
#else
#define LKMDBG_REMOTE_GUP_HAS_LOCKED_ONLY 0
#endif

typedef unsigned long (*lkmdbg_vm_mmap_fn)(struct file *file,
					   unsigned long addr,
					   unsigned long len,
					   unsigned long prot,
					   unsigned long flag,
					   unsigned long offset);

struct lkmdbg_remote_map {
	refcount_t refs;
	pid_t source_tgid;
	u64 remote_addr;
	u64 mapped_length;
	u64 vm_flags_raw;
	u32 prot;
	u32 flags;
	u32 page_count;
	struct page **pages;
};

struct lkmdbg_remote_stealth_map {
	struct list_head node;
	u64 map_id;
	pid_t source_tgid;
	pid_t replaced_tgid;
	u64 remote_addr;
	u64 local_addr;
	u64 mapped_length;
	u64 source_vm_flags_raw;
	u64 replaced_vm_flags_raw;
	u32 prot;
	u32 flags;
	u32 page_count;
	struct page **source_pages;
	struct page **replaced_pages;
	pte_t *baseline_replaced_ptes;
};

struct lkmdbg_remote_map_install {
	struct callback_head work;
	struct completion done;
	struct task_struct *task;
	struct file *file;
	unsigned long addr_hint;
	unsigned long mapped_length;
	unsigned long prot;
	unsigned long flags;
	unsigned long offset;
	long result;
	bool callback_entered;
	bool completed;
};

static void lkmdbg_remote_map_put(struct lkmdbg_remote_map *map);
static void lkmdbg_remote_map_install_task_work(struct callback_head *work);

static bool lkmdbg_remote_map_is_local_to_target(const struct lkmdbg_remote_map_request *req)
{
	return !!(req->flags & LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET);
}

static bool lkmdbg_remote_map_is_stealth_local(
	const struct lkmdbg_remote_map_request *req)
{
	return !!(req->flags & LKMDBG_REMOTE_MAP_FLAG_STEALTH_LOCAL);
}

static bool lkmdbg_remote_map_is_stealth_target(
	const struct lkmdbg_remote_map_request *req)
{
	return !!(req->flags & LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET);
}

/*
 * Only bookkeeping/policy bits are touched here, so vm_page_prot does not
 * need to be recomputed. Use a casted accessor because Android/common trees
 * backported const-qualified vm_flags without a stable feature macro.
 */
static inline vm_flags_t *lkmdbg_remote_map_vm_flags_ptr(struct vm_area_struct *vma)
{
	return (vm_flags_t *)&vma->vm_flags;
}

static inline void lkmdbg_remote_map_vm_flags_set(struct vm_area_struct *vma,
						  vm_flags_t flags)
{
	*lkmdbg_remote_map_vm_flags_ptr(vma) |= flags;
}

static inline void lkmdbg_remote_map_vm_flags_clear(struct vm_area_struct *vma,
						    vm_flags_t flags)
{
	*lkmdbg_remote_map_vm_flags_ptr(vma) &= ~flags;
}

static bool lkmdbg_remote_map_req_wants_write(const struct lkmdbg_remote_map_request *req)
{
	return !!(req->prot & LKMDBG_REMOTE_MAP_PROT_WRITE);
}

static int lkmdbg_remote_map_validate_addr_range(u64 addr, u64 length,
						 bool require_nonzero)
{
	if ((!addr && require_nonzero) || !length)
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

static int lkmdbg_validate_remote_map_req(struct lkmdbg_remote_map_request *req)
{
	int ret;
	bool local_to_target;
	bool stealth_local;
	bool stealth_target;

	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags & ~LKMDBG_REMOTE_MAP_VALID_FLAGS)
		return -EINVAL;

	if (!req->prot || (req->prot & ~LKMDBG_REMOTE_MAP_VALID_PROT))
		return -EINVAL;

	if (!req->length)
		return -EINVAL;

	if (req->length > LKMDBG_REMOTE_MAP_MAX_BYTES)
		return -E2BIG;

	local_to_target = lkmdbg_remote_map_is_local_to_target(req);
	stealth_local = lkmdbg_remote_map_is_stealth_local(req);
	stealth_target = lkmdbg_remote_map_is_stealth_target(req);
	if (stealth_local && stealth_target)
		return -EINVAL;
	if (stealth_target && !local_to_target)
		return -EINVAL;
	if ((req->flags & LKMDBG_REMOTE_MAP_FLAG_FIXED_TARGET) &&
	    (stealth_local || stealth_target))
		return -EINVAL;

	if (stealth_target) {
		ret = lkmdbg_remote_map_validate_addr_range(req->local_addr,
							    req->length, true);
		if (ret)
			return ret;
		ret = lkmdbg_remote_map_validate_addr_range(req->remote_addr,
							    req->length, true);
		if (ret)
			return ret;
		if (req->timeout_ms)
			return -EINVAL;
		return 0;
	}

	if (local_to_target) {
		ret = lkmdbg_remote_map_validate_addr_range(req->local_addr,
							    req->length, true);
		if (ret)
			return ret;
		if (req->remote_addr) {
			ret = lkmdbg_remote_map_validate_addr_range(
				req->remote_addr, req->length, false);
			if (ret)
				return ret;
		}
		if ((req->flags & LKMDBG_REMOTE_MAP_FLAG_FIXED_TARGET) &&
		    !req->remote_addr)
			return -EINVAL;
		return 0;
	}

	if (stealth_local) {
		ret = lkmdbg_remote_map_validate_addr_range(req->remote_addr,
							    req->length, true);
		if (ret)
			return ret;
		ret = lkmdbg_remote_map_validate_addr_range(req->local_addr,
							    req->length, true);
		if (ret)
			return ret;
		if (req->timeout_ms)
			return -EINVAL;
		return 0;
	}

	if (req->flags || req->local_addr || req->timeout_ms)
		return -EINVAL;

	return lkmdbg_remote_map_validate_addr_range(req->remote_addr,
						     req->length, true);
}

static int lkmdbg_task_work_add_resume(struct task_struct *task,
				       struct callback_head *work)
{
	return lkmdbg_task_work_add_runtime(task, work,
					    LKMDBG_TASK_WORK_NOTIFY_RESUME);
}

static lkmdbg_vm_mmap_fn lkmdbg_remote_map_vm_mmap_resolve(void)
{
	static lkmdbg_vm_mmap_fn fn;

	if (!fn && lkmdbg_symbols.kallsyms_lookup_name) {
		unsigned long addr;

		addr = lkmdbg_symbols.kallsyms_lookup_name("vm_mmap");
		if (addr)
			fn = (lkmdbg_vm_mmap_fn)addr;
	}

	return fn;
}

static void lkmdbg_remote_map_install_task_work(struct callback_head *work)
{
	struct lkmdbg_remote_map_install *install =
		container_of(work, struct lkmdbg_remote_map_install, work);
	lkmdbg_vm_mmap_fn vm_mmap_fn;

	install->callback_entered = true;
	vm_mmap_fn = lkmdbg_remote_map_vm_mmap_resolve();
	if (!vm_mmap_fn) {
		install->result = -EOPNOTSUPP;
		goto out;
	}

	if (current->flags & PF_EXITING) {
		install->result = -ESRCH;
		goto out;
	}

	install->result = (long)vm_mmap_fn(install->file, install->addr_hint,
					   install->mapped_length,
					   install->prot, install->flags,
					   install->offset);
out:
	install->completed = true;
	complete(&install->done);
}

static long lkmdbg_remote_map_install_into_target(struct task_struct *task,
						  struct file *file,
						  u64 addr_hint,
						  u64 mapped_length,
						  u32 prot, u32 flags,
						  u64 offset)
{
	struct lkmdbg_remote_map_install install;
	unsigned long map_flags = MAP_SHARED;
	long ret;

	if (!task || !file || !mapped_length)
		return -EINVAL;

	if (flags & LKMDBG_REMOTE_MAP_FLAG_FIXED_TARGET)
		map_flags |= MAP_FIXED_NOREPLACE;

	init_completion(&install.done);
	init_task_work(&install.work, lkmdbg_remote_map_install_task_work);
	install.task = task;
	install.file = file;
	install.addr_hint = (unsigned long)addr_hint;
	install.mapped_length = (unsigned long)mapped_length;
	install.prot = 0;
	install.flags = map_flags;
	install.offset = (unsigned long)offset;
	install.result = -EINPROGRESS;
	install.callback_entered = false;
	install.completed = false;
	if (prot & LKMDBG_REMOTE_MAP_PROT_READ)
		install.prot |= PROT_READ;
	if (prot & LKMDBG_REMOTE_MAP_PROT_WRITE)
		install.prot |= PROT_WRITE;
	if (prot & LKMDBG_REMOTE_MAP_PROT_EXEC)
		install.prot |= PROT_EXEC;

	ret = lkmdbg_task_work_add_resume(task, &install.work);
	if (ret)
		return ret;

	wait_for_completion(&install.done);
	return install.result;
}

static int lkmdbg_remote_map_vma_perms_ok(struct vm_area_struct *vma, u32 prot)
{
	u64 vm_flags = (u64)vma->vm_flags;

	if ((prot & LKMDBG_REMOTE_MAP_PROT_READ) && !(vm_flags & VM_READ))
		return -EACCES;
	if ((prot & LKMDBG_REMOTE_MAP_PROT_WRITE) && !(vm_flags & VM_WRITE))
		return -EACCES;
	if ((prot & LKMDBG_REMOTE_MAP_PROT_EXEC) && !(vm_flags & VM_EXEC))
		return -EACCES;
	if (vm_flags & (VM_PFNMAP | VM_IO))
		return -EOPNOTSUPP;

	return 0;
}

static int lkmdbg_remote_map_validate_range(struct mm_struct *mm, u64 remote_addr,
					    u64 length, u32 prot,
					    u64 *vm_flags_out)
{
	struct vm_area_struct *vma;
	int ret;

	mmap_read_lock(mm);
	ret = lkmdbg_target_vma_lookup_locked(mm, remote_addr, length, &vma);
	if (ret) {
		mmap_read_unlock(mm);
		return ret;
	}

	ret = lkmdbg_remote_map_vma_perms_ok(vma, prot);
	if (!ret && vm_flags_out)
		*vm_flags_out = (u64)vma->vm_flags;
	mmap_read_unlock(mm);

	return ret;
}

static int lkmdbg_remote_map_validate_local_alias_range(
	struct mm_struct *mm, u64 local_addr, u64 length, u32 prot,
	u64 *vm_flags_out)
{
	struct vm_area_struct *vma;
	u64 vm_flags;
	int ret;

	mmap_read_lock(mm);
	ret = lkmdbg_target_vma_lookup_locked(mm, local_addr, length, &vma);
	if (ret) {
		mmap_read_unlock(mm);
		return ret;
	}

	vm_flags = (u64)vma->vm_flags;
	ret = lkmdbg_remote_map_vma_perms_ok(vma, prot);
	if (ret) {
		mmap_read_unlock(mm);
		return ret;
	}

	if (!vma->vm_file || !(vm_flags & VM_SHARED) ||
	    !(vm_flags & VM_MAYSHARE)) {
		mmap_read_unlock(mm);
		return -EOPNOTSUPP;
	}

	if (vm_flags_out)
		*vm_flags_out = vm_flags;
	mmap_read_unlock(mm);
	return 0;
}

static int lkmdbg_remote_map_prepare_local_file(struct mm_struct *mm,
						u64 local_addr, u64 length,
						u32 prot, u64 *vm_flags_out,
						struct file **file_out,
						u64 *offset_out)
{
	struct vm_area_struct *vma;
	u64 offset;
	int ret;

	if (!file_out || !offset_out)
		return -EINVAL;

	mmap_read_lock(mm);
	ret = lkmdbg_target_vma_lookup_locked(mm, local_addr, length, &vma);
	if (ret)
		goto out_unlock;

	ret = lkmdbg_remote_map_vma_perms_ok(vma, prot);
	if (ret)
		goto out_unlock;

	if (!vma->vm_file || !(vma->vm_flags & VM_SHARED)) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	offset = ((u64)vma->vm_pgoff << PAGE_SHIFT) +
		 (local_addr - (u64)vma->vm_start);
	get_file(vma->vm_file);
	*file_out = vma->vm_file;
	*offset_out = offset;
	if (vm_flags_out)
		*vm_flags_out = (u64)vma->vm_flags;
	ret = 0;

out_unlock:
	mmap_read_unlock(mm);
	return ret;
}

static long lkmdbg_remote_map_pin_pages(struct mm_struct *mm,
					unsigned long start,
					unsigned int nr_pages,
					unsigned int flags,
					struct page **pages)
{
	long ret;
	int locked = 1;

	mmap_read_lock(mm);
#ifdef FOLL_PIN
#if LKMDBG_REMOTE_GUP_HAS_LOCKED_ONLY
	ret = pin_user_pages_remote(mm, start, nr_pages, flags, pages,
				    &locked);
#else
	ret = pin_user_pages_remote(mm, start, nr_pages, flags, pages, NULL,
				    &locked);
#endif
#else
#if LKMDBG_REMOTE_GUP_HAS_LOCKED_ONLY
	ret = get_user_pages_remote(mm, start, nr_pages, flags, pages, &locked);
#else
	ret = get_user_pages_remote(mm, start, nr_pages, flags, pages, NULL,
				    &locked);
#endif
#endif
	if (locked)
		mmap_read_unlock(mm);

	return ret;
}

static void lkmdbg_remote_map_put_pages(struct page **pages, u32 page_count,
					bool dirty)
{
	u32 i;

	if (!pages)
		return;

#ifdef FOLL_PIN
	if (dirty)
		unpin_user_pages_dirty_lock(pages, page_count, true);
	else
		unpin_user_pages(pages, page_count);

	for (i = 0; i < page_count; i++)
		pages[i] = NULL;
#else
	for (i = 0; i < page_count; i++) {
		if (!pages[i])
			continue;
		if (dirty)
			set_page_dirty_lock(pages[i]);
		put_page(pages[i]);
		pages[i] = NULL;
	}
#endif
}

static bool lkmdbg_remote_map_ranges_overlap(u64 start_a, u64 len_a, u64 start_b,
					     u64 len_b)
{
	u64 end_a = start_a + len_a;
	u64 end_b = start_b + len_b;

	return start_a < end_b && start_b < end_a;
}

static int lkmdbg_remote_map_check_stealth_overlap(struct lkmdbg_session *session,
						   u64 addr, u64 length, u32 flags)
{
	struct lkmdbg_remote_stealth_map *map;
	u64 map_addr;

	list_for_each_entry(map, &session->remote_maps, node) {
		map_addr = (map->flags & LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET) ?
				   map->remote_addr :
				   map->local_addr;
		if (lkmdbg_remote_map_ranges_overlap(addr, length, map_addr,
						     map->mapped_length))
			return -EEXIST;
	}

	return 0;
}

static void lkmdbg_remote_map_fill_handle(
	const struct lkmdbg_remote_stealth_map *map,
	struct lkmdbg_remote_map_handle_request *req)
{
	memset(req, 0, sizeof(*req));
	req->version = LKMDBG_PROTO_VERSION;
	req->size = sizeof(*req);
	req->map_id = map->map_id;
	req->remote_addr = map->remote_addr;
	req->local_addr = map->local_addr;
	req->mapped_length = map->mapped_length;
	req->prot = map->prot;
	req->flags = map->flags;
}

static int lkmdbg_remote_map_remove_locked(
	struct lkmdbg_session *session, u64 map_id,
	struct lkmdbg_remote_stealth_map **map_out)
{
	struct lkmdbg_remote_stealth_map *map;

	if (!map_out || !map_id)
		return -EINVAL;

	list_for_each_entry(map, &session->remote_maps, node) {
		if (map->map_id != map_id)
			continue;

		list_del_init(&map->node);
		*map_out = map;
		return 0;
	}

	return -ENOENT;
}

static int lkmdbg_validate_remote_map_query(
	const struct lkmdbg_remote_map_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (!req->max_entries || !req->entries_addr)
		return -EINVAL;
	if (req->flags)
		return -EINVAL;

	return 0;
}

static void lkmdbg_remote_map_fill_entry(
	struct lkmdbg_remote_map_entry *entry,
	const struct lkmdbg_remote_stealth_map *map)
{
	memset(entry, 0, sizeof(*entry));
	entry->map_id = map->map_id;
	entry->remote_addr = map->remote_addr;
	entry->local_addr = map->local_addr;
	entry->mapped_length = map->mapped_length;
	entry->prot = map->prot;
	entry->flags = map->flags;
}

static int lkmdbg_remote_map_get_mm_by_tgid(pid_t tgid, struct mm_struct **mm_out)
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

static void lkmdbg_remote_map_restore_stealth(struct lkmdbg_remote_stealth_map *map)
{
	struct mm_struct *mm = NULL;
	u32 i;

	if (!map)
		return;

	if (!lkmdbg_remote_map_get_mm_by_tgid(map->replaced_tgid, &mm)) {
		mmap_write_lock(mm);
		for (i = 0; i < map->page_count; i++) {
			unsigned long addr;

			addr = (map->flags & LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET) ?
				       (unsigned long)map->remote_addr +
					       (i * PAGE_SIZE) :
				       (unsigned long)map->local_addr +
					       (i * PAGE_SIZE);

			lkmdbg_pte_rewrite_locked(mm, addr,
						  map->baseline_replaced_ptes[i],
						  NULL, NULL);
		}
		mmap_write_unlock(mm);
		mmput(mm);
	}

	lkmdbg_remote_map_put_pages(map->source_pages, map->page_count,
				    !!(map->prot & LKMDBG_REMOTE_MAP_PROT_WRITE));
	lkmdbg_remote_map_put_pages(map->replaced_pages, map->page_count, false);
	kfree(map->baseline_replaced_ptes);
	kfree(map->source_pages);
	kfree(map->replaced_pages);
	kfree(map);
}

static void lkmdbg_remote_map_release_pages(struct lkmdbg_remote_map *map)
{
	u32 i;

	if (!map->pages)
		return;

#ifdef FOLL_PIN
	if (map->prot & LKMDBG_REMOTE_MAP_PROT_WRITE)
		unpin_user_pages_dirty_lock(map->pages, map->page_count, true);
	else
		unpin_user_pages(map->pages, map->page_count);

	for (i = 0; i < map->page_count; i++)
		map->pages[i] = NULL;
#else
	for (i = 0; i < map->page_count; i++) {
		if (!map->pages[i])
			continue;
		if (map->prot & LKMDBG_REMOTE_MAP_PROT_WRITE)
			set_page_dirty_lock(map->pages[i]);
		put_page(map->pages[i]);
		map->pages[i] = NULL;
	}
#endif
}

static void lkmdbg_remote_map_destroy(struct lkmdbg_remote_map *map)
{
	if (!map)
		return;

	lkmdbg_remote_map_release_pages(map);
	kfree(map->pages);
	kfree(map);
}

static void lkmdbg_remote_map_get(struct lkmdbg_remote_map *map)
{
	refcount_inc(&map->refs);
}

static void lkmdbg_remote_map_put(struct lkmdbg_remote_map *map)
{
	if (map && refcount_dec_and_test(&map->refs))
		lkmdbg_remote_map_destroy(map);
}

static void lkmdbg_remote_map_vma_open(struct vm_area_struct *vma)
{
	struct lkmdbg_remote_map *map = vma->vm_private_data;

	lkmdbg_remote_map_get(map);
}

static void lkmdbg_remote_map_vma_close(struct vm_area_struct *vma)
{
	struct lkmdbg_remote_map *map = vma->vm_private_data;

	lkmdbg_remote_map_put(map);
}

static const struct vm_operations_struct lkmdbg_remote_map_vm_ops = {
	.open = lkmdbg_remote_map_vma_open,
	.close = lkmdbg_remote_map_vma_close,
};

static int lkmdbg_remote_map_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct lkmdbg_remote_map *map = file->private_data;
	bool insert_pages;
	vm_flags_t deny_may_flags = 0;
	unsigned long addr;
	unsigned long offset_pages;
	unsigned long map_pages;
	unsigned long i;
	int ret;

	if (!map)
		return -ENXIO;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	if ((vma->vm_flags & VM_READ) &&
	    !(map->prot & LKMDBG_REMOTE_MAP_PROT_READ))
		return -EACCES;
	if ((vma->vm_flags & VM_WRITE) &&
	    !(map->prot & LKMDBG_REMOTE_MAP_PROT_WRITE))
		return -EACCES;
	if ((vma->vm_flags & VM_EXEC) &&
	    !(map->prot & LKMDBG_REMOTE_MAP_PROT_EXEC))
		return -EACCES;

	offset_pages = vma->vm_pgoff;
	map_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	if (!map_pages || offset_pages >= map->page_count ||
	    map_pages > map->page_count - offset_pages)
		return -EINVAL;

	insert_pages = !!(map->flags & LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET);
	if (insert_pages)
		lkmdbg_remote_map_vm_flags_set(vma,
					      VM_DONTEXPAND | VM_DONTDUMP |
						      VM_MIXEDMAP);
	else
		lkmdbg_remote_map_vm_flags_set(vma,
					      VM_DONTEXPAND | VM_DONTDUMP |
						      VM_IO | VM_PFNMAP);
	if (!(map->prot & LKMDBG_REMOTE_MAP_PROT_READ))
		deny_may_flags |= VM_MAYREAD;
	if (!(map->prot & LKMDBG_REMOTE_MAP_PROT_WRITE))
		deny_may_flags |= VM_MAYWRITE;
	if (!(map->prot & LKMDBG_REMOTE_MAP_PROT_EXEC))
		deny_may_flags |= VM_MAYEXEC;
	if (deny_may_flags)
		lkmdbg_remote_map_vm_flags_clear(vma, deny_may_flags);

	addr = vma->vm_start;
	for (i = 0; i < map_pages; i++) {
		if (insert_pages) {
			ret = vm_insert_page(vma, addr + (i * PAGE_SIZE),
					     map->pages[offset_pages + i]);
		} else {
			ret = remap_pfn_range(
				vma, addr + (i * PAGE_SIZE),
				page_to_pfn(map->pages[offset_pages + i]),
				PAGE_SIZE, vma->vm_page_prot);
		}
		if (ret)
			return ret;
	}

	vma->vm_ops = &lkmdbg_remote_map_vm_ops;
	vma->vm_private_data = map;
	lkmdbg_remote_map_get(map);
	return 0;
}

static int lkmdbg_remote_map_release(struct inode *inode, struct file *file)
{
	struct lkmdbg_remote_map *map = file->private_data;

	lkmdbg_remote_map_put(map);
	return 0;
}

static const struct file_operations lkmdbg_remote_map_fops = {
	.owner = THIS_MODULE,
	.release = lkmdbg_remote_map_release,
	.mmap = lkmdbg_remote_map_mmap,
	.llseek = noop_llseek,
};

static int lkmdbg_remote_map_prepare_file(struct lkmdbg_remote_map *map,
					  struct file **file_out)
{
	struct file *file;

	file = anon_inode_getfile("lkmdbg-rmap", &lkmdbg_remote_map_fops, map,
				  O_RDWR | O_CLOEXEC);
	if (IS_ERR(file))
		return PTR_ERR(file);

	*file_out = file;
	return 0;
}

static int lkmdbg_remote_map_prepare_fd(struct lkmdbg_remote_map *map,
					struct file **file_out)
{
	int fd;
	int ret;

	ret = lkmdbg_remote_map_prepare_file(map, file_out);
	if (ret)
		return ret;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(*file_out);
		*file_out = NULL;
		return fd;
	}

	return fd;
}

static int lkmdbg_remote_map_create_stealth_local(
	struct lkmdbg_session *session, struct lkmdbg_remote_map_request *req,
	void __user *argp)
{
	struct lkmdbg_remote_stealth_map *map = NULL;
	struct mm_struct *source_mm = NULL;
	struct mm_struct *local_mm = NULL;
	unsigned int source_gup_flags = 0;
	u64 source_vm_flags_raw = 0;
	u64 local_vm_flags_raw = 0;
	u64 mapped_length;
	u32 page_count;
	long pinned;
	u32 i;
	int ret;

	ret = lkmdbg_get_target_mm(session, &source_mm);
	if (ret)
		return ret;

	ret = lkmdbg_remote_map_validate_range(source_mm, req->remote_addr,
					       req->length, req->prot,
					       &source_vm_flags_raw);
	if (ret)
		goto out_source_mm;

	local_mm = get_task_mm(current);
	if (!local_mm) {
		ret = -ESRCH;
		goto out_source_mm;
	}

	ret = lkmdbg_remote_map_validate_local_alias_range(
		local_mm, req->local_addr, req->length, req->prot,
		&local_vm_flags_raw);
	if (ret)
		goto out_local_mm;

	mapped_length = PAGE_ALIGN(req->length);
	page_count = mapped_length >> PAGE_SHIFT;
	req->map_id = 0;
	req->mapped_length = mapped_length;
	req->map_fd = -1;

	mutex_lock(&session->lock);
	ret = lkmdbg_remote_map_check_stealth_overlap(
		session, req->local_addr, mapped_length,
		LKMDBG_REMOTE_MAP_FLAG_STEALTH_LOCAL);
	mutex_unlock(&session->lock);
	if (ret)
		goto out_local_mm;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		ret = -ENOMEM;
		goto out_local_mm;
	}
	INIT_LIST_HEAD(&map->node);

	map->source_pages = kcalloc(page_count, sizeof(*map->source_pages),
				    GFP_KERNEL);
	map->replaced_pages = kcalloc(page_count, sizeof(*map->replaced_pages),
				      GFP_KERNEL);
	map->baseline_replaced_ptes = kcalloc(
		page_count, sizeof(*map->baseline_replaced_ptes), GFP_KERNEL);
	if (!map->source_pages || !map->replaced_pages ||
	    !map->baseline_replaced_ptes) {
		ret = -ENOMEM;
		goto out_map;
	}

	if (req->prot & LKMDBG_REMOTE_MAP_PROT_WRITE)
		source_gup_flags |= FOLL_WRITE;

	pinned = lkmdbg_remote_map_pin_pages(source_mm,
					     (unsigned long)req->remote_addr,
					     page_count, source_gup_flags,
					     map->source_pages);
	if (pinned < 0) {
		ret = (int)pinned;
		goto out_map;
	}
	if ((u32)pinned != page_count) {
		ret = -EFAULT;
		goto out_map;
	}

	pinned = lkmdbg_remote_map_pin_pages(local_mm,
					     (unsigned long)req->local_addr,
					     page_count, 0, map->replaced_pages);
	if (pinned < 0) {
		ret = (int)pinned;
		goto out_map;
	}
	if ((u32)pinned != page_count) {
		ret = -EFAULT;
		goto out_map;
	}

	mmap_write_lock(local_mm);
	for (i = 0; i < page_count; i++) {
		unsigned long addr = (unsigned long)req->local_addr +
				      (i * PAGE_SIZE);
		pte_t baseline_pte;
		pte_t alias_pte;

		ret = lkmdbg_pte_read_locked(local_mm, addr, &baseline_pte,
					     NULL);
		if (ret)
			break;
		map->baseline_replaced_ptes[i] = baseline_pte;
		alias_pte = lkmdbg_pte_build_alias_pte(
			map->source_pages[i], baseline_pte, req->prot);
		ret = lkmdbg_pte_rewrite_locked(local_mm, addr, alias_pte, NULL,
						NULL);
		if (ret)
			break;
	}
	if (ret) {
		while (i > 0) {
			unsigned long addr = (unsigned long)req->local_addr +
					      ((i - 1) * PAGE_SIZE);
			lkmdbg_pte_rewrite_locked(local_mm, addr,
						  map->baseline_replaced_ptes[i - 1],
						  NULL, NULL);
			i--;
		}
	}
	mmap_write_unlock(local_mm);
	if (ret)
		goto out_map;

	map->replaced_tgid = current->tgid;
	if (lkmdbg_get_target_identity(session, &map->source_tgid, NULL))
		map->source_tgid = 0;
	map->remote_addr = req->remote_addr;
	map->local_addr = req->local_addr;
	map->mapped_length = mapped_length;
	map->source_vm_flags_raw = source_vm_flags_raw;
	map->replaced_vm_flags_raw = local_vm_flags_raw;
	map->prot = req->prot;
	map->flags = LKMDBG_REMOTE_MAP_FLAG_STEALTH_LOCAL;
	map->page_count = page_count;

	mutex_lock(&session->lock);
	ret = lkmdbg_remote_map_check_stealth_overlap(
		session, req->local_addr, mapped_length, map->flags);
	if (!ret) {
		map->map_id = ++session->next_remote_map_id;
		req->map_id = map->map_id;
		list_add_tail(&map->node, &session->remote_maps);
	}
	mutex_unlock(&session->lock);
	if (ret)
		goto out_map_restore;

	mmput(local_mm);
	local_mm = NULL;
	mmput(source_mm);
	source_mm = NULL;
	if (copy_to_user(argp, req, sizeof(*req))) {
		mutex_lock(&session->lock);
		if (!list_empty(&map->node))
			list_del_init(&map->node);
		mutex_unlock(&session->lock);
		lkmdbg_remote_map_restore_stealth(map);
		return -EFAULT;
	}
	return 0;

out_map_restore:
	if (local_mm)
		mmput(local_mm);
	if (source_mm)
		mmput(source_mm);
	lkmdbg_remote_map_restore_stealth(map);
	return ret;
out_map:
	lkmdbg_remote_map_put_pages(map ? map->source_pages : NULL, page_count,
				    !!(req->prot & LKMDBG_REMOTE_MAP_PROT_WRITE));
	lkmdbg_remote_map_put_pages(map ? map->replaced_pages : NULL, page_count,
				    false);
	kfree(map ? map->baseline_replaced_ptes : NULL);
	kfree(map ? map->source_pages : NULL);
	kfree(map ? map->replaced_pages : NULL);
	kfree(map);
out_local_mm:
	if (local_mm)
		mmput(local_mm);
out_source_mm:
	if (source_mm)
		mmput(source_mm);
	return ret;
}

static int lkmdbg_remote_map_create_stealth_target(
	struct lkmdbg_session *session, struct lkmdbg_remote_map_request *req,
	void __user *argp)
{
	struct lkmdbg_remote_stealth_map *map = NULL;
	struct mm_struct *source_mm = NULL;
	struct mm_struct *target_mm = NULL;
	unsigned int source_gup_flags = 0;
	u64 source_vm_flags_raw = 0;
	u64 target_vm_flags_raw = 0;
	u64 mapped_length;
	u32 page_count;
	long pinned;
	pid_t target_tgid;
	u32 i;
	int ret;

	source_mm = get_task_mm(current);
	if (!source_mm)
		return -ESRCH;

	ret = lkmdbg_remote_map_validate_range(source_mm, req->local_addr,
					       req->length, req->prot,
					       &source_vm_flags_raw);
	if (ret)
		goto out_source_mm;

	ret = lkmdbg_get_target_identity(session, &target_tgid, NULL);
	if (ret)
		goto out_source_mm;

	ret = lkmdbg_get_target_mm(session, &target_mm);
	if (ret)
		goto out_source_mm;

	ret = lkmdbg_remote_map_validate_range(target_mm, req->remote_addr,
					       req->length, req->prot,
					       &target_vm_flags_raw);
	if (ret)
		goto out_target_mm;

	mapped_length = PAGE_ALIGN(req->length);
	page_count = mapped_length >> PAGE_SHIFT;
	req->map_id = 0;
	req->mapped_length = mapped_length;
	req->map_fd = -1;

	mutex_lock(&session->lock);
	ret = lkmdbg_remote_map_check_stealth_overlap(
		session, req->remote_addr, mapped_length,
		LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET |
			LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET);
	if (!ret &&
	    (lkmdbg_remote_alloc_has_overlap_locked(session, req->remote_addr,
						      mapped_length) ||
	     lkmdbg_pte_patch_has_overlap_locked(session, req->remote_addr,
						 mapped_length)))
		ret = -EEXIST;
	mutex_unlock(&session->lock);
	if (ret)
		goto out_target_mm;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		ret = -ENOMEM;
		goto out_target_mm;
	}
	INIT_LIST_HEAD(&map->node);

	map->source_pages = kcalloc(page_count, sizeof(*map->source_pages),
				    GFP_KERNEL);
	map->replaced_pages = kcalloc(page_count, sizeof(*map->replaced_pages),
				      GFP_KERNEL);
	map->baseline_replaced_ptes = kcalloc(
		page_count, sizeof(*map->baseline_replaced_ptes), GFP_KERNEL);
	if (!map->source_pages || !map->replaced_pages ||
	    !map->baseline_replaced_ptes) {
		ret = -ENOMEM;
		goto out_map;
	}

	if (req->prot & LKMDBG_REMOTE_MAP_PROT_WRITE)
		source_gup_flags |= FOLL_WRITE;

	pinned = lkmdbg_remote_map_pin_pages(source_mm,
					     (unsigned long)req->local_addr,
					     page_count, source_gup_flags,
					     map->source_pages);
	if (pinned < 0) {
		ret = (int)pinned;
		goto out_map;
	}
	if ((u32)pinned != page_count) {
		ret = -EFAULT;
		goto out_map;
	}

	pinned = lkmdbg_remote_map_pin_pages(target_mm,
					     (unsigned long)req->remote_addr,
					     page_count, 0,
					     map->replaced_pages);
	if (pinned < 0) {
		ret = (int)pinned;
		goto out_map;
	}
	if ((u32)pinned != page_count) {
		ret = -EFAULT;
		goto out_map;
	}

	mmap_write_lock(target_mm);
	for (i = 0; i < page_count; i++) {
		unsigned long addr = (unsigned long)req->remote_addr +
				      (i * PAGE_SIZE);
		pte_t baseline_pte;
		pte_t alias_pte;

		ret = lkmdbg_pte_read_locked(target_mm, addr, &baseline_pte,
					     NULL);
		if (ret)
			break;
		map->baseline_replaced_ptes[i] = baseline_pte;
		alias_pte = lkmdbg_pte_build_alias_pte(
			map->source_pages[i], baseline_pte, req->prot);
		ret = lkmdbg_pte_rewrite_locked(target_mm, addr, alias_pte,
						NULL, NULL);
		if (ret)
			break;
	}
	if (ret) {
		while (i > 0) {
			unsigned long addr = (unsigned long)req->remote_addr +
					      ((i - 1) * PAGE_SIZE);
			lkmdbg_pte_rewrite_locked(
				target_mm, addr, map->baseline_replaced_ptes[i - 1],
				NULL, NULL);
			i--;
		}
	}
	mmap_write_unlock(target_mm);
	if (ret)
		goto out_map;

	map->source_tgid = current->tgid;
	map->replaced_tgid = target_tgid;
	map->remote_addr = req->remote_addr;
	map->local_addr = req->local_addr;
	map->mapped_length = mapped_length;
	map->source_vm_flags_raw = source_vm_flags_raw;
	map->replaced_vm_flags_raw = target_vm_flags_raw;
	map->prot = req->prot;
	map->flags = LKMDBG_REMOTE_MAP_FLAG_LOCAL_TO_TARGET |
		     LKMDBG_REMOTE_MAP_FLAG_STEALTH_TARGET;
	map->page_count = page_count;

	mutex_lock(&session->lock);
	ret = lkmdbg_remote_map_check_stealth_overlap(session, req->remote_addr,
						      mapped_length,
						      map->flags);
	if (!ret &&
	    (lkmdbg_remote_alloc_has_overlap_locked(session, req->remote_addr,
						      mapped_length) ||
	     lkmdbg_pte_patch_has_overlap_locked(session, req->remote_addr,
						 mapped_length)))
		ret = -EEXIST;
	if (!ret) {
		map->map_id = ++session->next_remote_map_id;
		req->map_id = map->map_id;
		list_add_tail(&map->node, &session->remote_maps);
	}
	mutex_unlock(&session->lock);
	if (ret)
		goto out_map_restore;

	mmput(target_mm);
	target_mm = NULL;
	mmput(source_mm);
	source_mm = NULL;
	if (copy_to_user(argp, req, sizeof(*req))) {
		mutex_lock(&session->lock);
		if (!list_empty(&map->node))
			list_del_init(&map->node);
		mutex_unlock(&session->lock);
		lkmdbg_remote_map_restore_stealth(map);
		return -EFAULT;
	}
	return 0;

out_map_restore:
	if (target_mm)
		mmput(target_mm);
	if (source_mm)
		mmput(source_mm);
	lkmdbg_remote_map_restore_stealth(map);
	return ret;
out_map:
	lkmdbg_remote_map_put_pages(map ? map->source_pages : NULL, page_count,
				    !!(req->prot & LKMDBG_REMOTE_MAP_PROT_WRITE));
	lkmdbg_remote_map_put_pages(map ? map->replaced_pages : NULL,
				    page_count, false);
	kfree(map ? map->baseline_replaced_ptes : NULL);
	kfree(map ? map->source_pages : NULL);
	kfree(map ? map->replaced_pages : NULL);
	kfree(map);
out_target_mm:
	if (target_mm)
		mmput(target_mm);
out_source_mm:
	if (source_mm)
		mmput(source_mm);
	return ret;
}

long lkmdbg_create_remote_map(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_remote_map_request req;
	struct lkmdbg_remote_map *map = NULL;
	struct task_struct *task = NULL;
	struct mm_struct *mm = NULL;
	u64 vm_flags_raw = 0;
	u64 source_addr;
	u64 mapped_length;
	u64 local_file_offset = 0;
	u32 page_count;
	unsigned int gup_flags = 0;
	long pinned;
	struct file *map_file = NULL;
	bool local_to_target;
	bool stealth_local;
	bool stealth_target;
	long install_addr = 0;
	int map_fd = -1;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_remote_map_req(&req);
	if (ret)
		return ret;

	local_to_target = lkmdbg_remote_map_is_local_to_target(&req);
	stealth_local = lkmdbg_remote_map_is_stealth_local(&req);
	stealth_target = lkmdbg_remote_map_is_stealth_target(&req);
	if (stealth_local)
		return lkmdbg_remote_map_create_stealth_local(session, &req,
							      argp);
	if (stealth_target)
		return lkmdbg_remote_map_create_stealth_target(session, &req,
							       argp);
	source_addr = local_to_target ? req.local_addr : req.remote_addr;
	if (local_to_target) {
		mm = get_task_mm(current);
		if (!mm)
			return -ESRCH;
		ret = lkmdbg_remote_map_prepare_local_file(
			mm, req.local_addr, req.length, req.prot, &vm_flags_raw,
			&map_file, &local_file_offset);
		if (ret)
			goto out_mm;
	} else {
		ret = lkmdbg_get_target_mm(session, &mm);
		if (ret)
			return ret;

		ret = lkmdbg_remote_map_validate_range(mm, req.remote_addr,
						       req.length, req.prot,
						       &vm_flags_raw);
		if (ret)
			goto out_mm;
	}

	mapped_length = PAGE_ALIGN(req.length);
	page_count = mapped_length >> PAGE_SHIFT;
	req.map_id = 0;
	req.mapped_length = mapped_length;
	req.local_addr = source_addr;

	if (local_to_target) {
		ret = lkmdbg_get_target_thread(session, 0, &task);
		if (ret)
			goto out_file;

		install_addr = lkmdbg_remote_map_install_into_target(
			task, map_file, req.remote_addr, mapped_length,
			req.prot, req.flags, local_file_offset);
		put_task_struct(task);
		task = NULL;
		if (install_addr < 0) {
			ret = (int)install_addr;
			goto out_file;
		}

		req.remote_addr = (u64)install_addr;
		req.map_fd = -1;
		if (copy_to_user(argp, &req, sizeof(req))) {
			mmput(mm);
			fput(map_file);
			return -EFAULT;
		}

		mmput(mm);
		fput(map_file);
		return 0;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		ret = -ENOMEM;
		goto out_mm;
	}

	map->pages = kcalloc(page_count, sizeof(*map->pages), GFP_KERNEL);
	if (!map->pages) {
		ret = -ENOMEM;
		goto out_map;
	}

	map->source_tgid = current->tgid;
	if (!local_to_target &&
	    lkmdbg_get_target_identity(session, &map->source_tgid, NULL))
		map->source_tgid = 0;
	map->remote_addr = source_addr;
	map->mapped_length = mapped_length;
	map->vm_flags_raw = vm_flags_raw;
	map->prot = req.prot;
	map->flags = req.flags;
	map->page_count = page_count;
	refcount_set(&map->refs, 1);

	if (lkmdbg_remote_map_req_wants_write(&req))
		gup_flags |= FOLL_WRITE;

	pinned = lkmdbg_remote_map_pin_pages(mm, (unsigned long)source_addr,
					     page_count, gup_flags, map->pages);
	if (pinned < 0) {
		ret = (int)pinned;
		goto out_map;
	}
	if ((u32)pinned != page_count) {
		ret = -EFAULT;
		goto out_map;
	}

	map_fd = lkmdbg_remote_map_prepare_fd(map, &map_file);
	if (map_fd < 0) {
		ret = map_fd;
		goto out_map;
	}

	req.map_fd = map_fd;
	req.remote_addr = map->remote_addr;
	if (copy_to_user(argp, &req, sizeof(req))) {
		put_unused_fd(map_fd);
		fput(map_file);
		mmput(mm);
		return -EFAULT;
	}

	fd_install(map_fd, map_file);
	mmput(mm);
	return 0;

out_file:
	if (task)
		put_task_struct(task);
	if (map_file)
		fput(map_file);
out_map:
	lkmdbg_remote_map_put(map);
out_mm:
	mmput(mm);
	return ret;
}

long lkmdbg_remove_remote_map(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_remote_map_handle_request req;
	struct lkmdbg_remote_stealth_map *map = NULL;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req))
		return -EINVAL;

	mutex_lock(&session->lock);
	ret = lkmdbg_remote_map_remove_locked(session, req.map_id, &map);
	mutex_unlock(&session->lock);
	if (ret)
		return ret;

	lkmdbg_remote_map_fill_handle(map, &req);
	lkmdbg_remote_map_restore_stealth(map);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_query_remote_maps(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_remote_map_query_request req;
	struct lkmdbg_remote_map_entry *entries;
	struct lkmdbg_remote_stealth_map *map;
	u32 filled = 0;
	bool started;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_remote_map_query(&req);
	if (ret)
		return ret;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	started = req.start_id == 0;
	req.done = 1;
	req.next_id = req.start_id;
	mutex_lock(&session->lock);
	list_for_each_entry(map, &session->remote_maps, node) {
		if (!started) {
			if (map->map_id <= req.start_id)
				continue;
			started = true;
		}
		if (filled == req.max_entries) {
			req.done = 0;
			break;
		}

		lkmdbg_remote_map_fill_entry(&entries[filled], map);
		req.next_id = map->map_id;
		filled++;
	}
	req.entries_filled = filled;
	mutex_unlock(&session->lock);

	if (copy_to_user((void __user *)(uintptr_t)req.entries_addr, entries,
			 filled * sizeof(*entries))) {
		kfree(entries);
		return -EFAULT;
	}
	kfree(entries);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

void lkmdbg_remote_map_release_session(struct lkmdbg_session *session)
{
	struct lkmdbg_remote_stealth_map *map;
	struct lkmdbg_remote_stealth_map *tmp;
	LIST_HEAD(release_list);

	if (!session)
		return;

	mutex_lock(&session->lock);
	list_for_each_entry_safe(map, tmp, &session->remote_maps, node) {
		list_del_init(&map->node);
		list_add_tail(&map->node, &release_list);
	}
	mutex_unlock(&session->lock);

	list_for_each_entry_safe(map, tmp, &release_list, node) {
		list_del_init(&map->node);
		lkmdbg_remote_map_restore_stealth(map);
	}
}
