#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmap_lock.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/refcount.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "lkmdbg_internal.h"

#define LKMDBG_REMOTE_MAP_MAX_BYTES (16U * 1024U * 1024U)
#define LKMDBG_REMOTE_MAP_VALID_PROT                                         \
	(LKMDBG_REMOTE_MAP_PROT_READ | LKMDBG_REMOTE_MAP_PROT_WRITE |        \
	 LKMDBG_REMOTE_MAP_PROT_EXEC)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
#define LKMDBG_REMOTE_GUP_HAS_LOCKED_ONLY 1
#else
#define LKMDBG_REMOTE_GUP_HAS_LOCKED_ONLY 0
#endif

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

static void lkmdbg_remote_map_put(struct lkmdbg_remote_map *map);

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

static int
lkmdbg_validate_remote_map_req(struct lkmdbg_remote_map_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->remote_addr || !req->length)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (!req->prot || (req->prot & ~LKMDBG_REMOTE_MAP_VALID_PROT))
		return -EINVAL;

	if (req->remote_addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (req->remote_addr & ~PAGE_MASK)
		return -EINVAL;

	if (req->remote_addr + req->length < req->remote_addr)
		return -EINVAL;

	if (req->remote_addr + req->length > (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (req->length > LKMDBG_REMOTE_MAP_MAX_BYTES)
		return -E2BIG;

	return 0;
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
	u64 end_addr = remote_addr + length;
	int ret;

	mmap_read_lock(mm);
	vma = find_vma(mm, remote_addr);
	if (!vma || remote_addr < vma->vm_start || end_addr > vma->vm_end) {
		mmap_read_unlock(mm);
		return -EINVAL;
	}

	ret = lkmdbg_remote_map_vma_perms_ok(vma, prot);
	if (!ret && vm_flags_out)
		*vm_flags_out = (u64)vma->vm_flags;
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

	lkmdbg_remote_map_vm_flags_set(vma,
				      VM_DONTEXPAND | VM_DONTDUMP | VM_IO |
					      VM_PFNMAP);
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
		ret = remap_pfn_range(vma, addr + (i * PAGE_SIZE),
				      page_to_pfn(map->pages[offset_pages + i]),
				      PAGE_SIZE, vma->vm_page_prot);
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
	.llseek = no_llseek,
};

static int lkmdbg_remote_map_prepare_fd(struct lkmdbg_remote_map *map,
					struct file **file_out)
{
	struct file *file;
	int open_flags;
	int fd;

	open_flags = O_CLOEXEC;
	if (map->prot & LKMDBG_REMOTE_MAP_PROT_WRITE)
		open_flags |= O_RDWR;
	else
		open_flags |= O_RDONLY;

	file = anon_inode_getfile("lkmdbg-rmap", &lkmdbg_remote_map_fops, map,
				  open_flags);
	if (IS_ERR(file))
		return PTR_ERR(file);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(file);
		return fd;
	}

	*file_out = file;
	return fd;
}

long lkmdbg_create_remote_map(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_remote_map_request req;
	struct lkmdbg_remote_map *map = NULL;
	struct mm_struct *mm = NULL;
	u64 vm_flags_raw = 0;
	u64 mapped_length;
	u32 page_count;
	unsigned int gup_flags = 0;
	long pinned;
	struct file *map_file = NULL;
	int map_fd;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_remote_map_req(&req);
	if (ret)
		return ret;

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret)
		return ret;

	ret = lkmdbg_remote_map_validate_range(mm, req.remote_addr, req.length,
					       req.prot, &vm_flags_raw);
	if (ret)
		goto out_mm;

	mapped_length = PAGE_ALIGN(req.length);
	page_count = mapped_length >> PAGE_SHIFT;

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
	if (lkmdbg_get_target_identity(session, &map->source_tgid, NULL))
		map->source_tgid = 0;
	map->remote_addr = req.remote_addr;
	map->mapped_length = mapped_length;
	map->vm_flags_raw = vm_flags_raw;
	map->prot = req.prot;
	map->flags = req.flags;
	map->page_count = page_count;
	refcount_set(&map->refs, 1);

	if (lkmdbg_remote_map_req_wants_write(&req))
		gup_flags |= FOLL_WRITE;

	pinned = lkmdbg_remote_map_pin_pages(mm, (unsigned long)req.remote_addr,
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

	req.mapped_length = mapped_length;
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

out_map:
	lkmdbg_remote_map_put(map);
out_mm:
	mmput(mm);
	return ret;
}
