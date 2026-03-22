#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

#define LKMDBG_IMAGE_MAX_ENTRIES 256U
#define LKMDBG_IMAGE_MAX_NAMES_SIZE (128U * 1024U)

static int lkmdbg_validate_image_query(struct lkmdbg_image_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_IMAGE_MAX_ENTRIES)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (req->names_size > LKMDBG_IMAGE_MAX_NAMES_SIZE)
		return -E2BIG;

	if ((req->names_size && !req->names_addr) ||
	    (!req->names_size && req->names_addr))
		return -EINVAL;

	if (req->start_addr > (u64)TASK_SIZE_MAX)
		return -EINVAL;

	return 0;
}

static void lkmdbg_image_fill_prot(struct lkmdbg_image_entry *entry,
				   struct vm_area_struct *vma)
{
	entry->prot |= lkmdbg_target_vm_prot_bits((u64)vma->vm_flags);
}

static bool lkmdbg_image_vma_supported(struct vm_area_struct *vma)
{
	return vma->vm_file && file_inode(vma->vm_file);
}

static bool lkmdbg_image_same_backing(struct vm_area_struct *lhs,
				      struct vm_area_struct *rhs)
{
	if (!lkmdbg_image_vma_supported(lhs) || !lkmdbg_image_vma_supported(rhs))
		return false;

	return lhs->vm_file == rhs->vm_file;
}

static bool lkmdbg_image_is_main_exe(struct mm_struct *mm,
				     struct vm_area_struct *vma)
{
	struct inode *lhs;
	struct inode *rhs;

	if (!mm || !mm->exe_file || !vma->vm_file)
		return false;

	lhs = file_inode(mm->exe_file);
	rhs = file_inode(vma->vm_file);
	if (!lhs || !rhs)
		return false;

	return lhs->i_ino == rhs->i_ino && lhs->i_sb->s_dev == rhs->i_sb->s_dev;
}

static struct vm_area_struct *
lkmdbg_image_find_next_supported_vma(struct mm_struct *mm, u64 cursor)
{
	struct vm_area_struct *vma;

	vma = find_vma(mm, cursor);
	while (vma && !lkmdbg_image_vma_supported(vma))
		vma = find_vma(mm, vma->vm_end);
	return vma;
}

static int lkmdbg_image_fill_name(struct lkmdbg_image_query_request *req,
				  struct lkmdbg_image_entry *entry,
				  struct vm_area_struct *vma, char *names,
				  u32 *names_used, char *path_buf)
{
	char *resolved;
	size_t name_len;

	if (!req->names_size)
		return 0;
	if (!path_buf || !vma->vm_file)
		return -EINVAL;

	resolved = d_path(&vma->vm_file->f_path, path_buf, PATH_MAX);
	if (IS_ERR(resolved))
		return PTR_ERR(resolved);

	name_len = strlen(resolved) + 1;
	if (name_len > req->names_size - *names_used)
		return -E2BIG;

	entry->name_offset = *names_used;
	entry->name_size = (u32)name_len;
	memcpy(names + *names_used, resolved, name_len);
	*names_used += (u32)name_len;
	return 0;
}

static int lkmdbg_image_fill_entry(struct mm_struct *mm,
				   struct vm_area_struct *start_vma,
				   struct lkmdbg_image_entry *entry)
{
	struct vm_area_struct *vma = start_vma;
	struct inode *inode;
	u64 base_addr;

	if (!lkmdbg_image_vma_supported(start_vma))
		return -EINVAL;

	inode = file_inode(start_vma->vm_file);
	if (!inode)
		return -EINVAL;

	memset(entry, 0, sizeof(*entry));
	entry->start_addr = start_vma->vm_start;
	entry->end_addr = start_vma->vm_end;
	entry->pgoff = start_vma->vm_pgoff;
	entry->inode = inode->i_ino;
	entry->flags = LKMDBG_IMAGE_FLAG_FILE;
	entry->dev_major = MAJOR(inode->i_sb->s_dev);
	entry->dev_minor = MINOR(inode->i_sb->s_dev);
	if ((u64)start_vma->vm_flags & VM_SHARED)
		entry->flags |= LKMDBG_IMAGE_FLAG_SHARED;
	if (d_unlinked(start_vma->vm_file->f_path.dentry))
		entry->flags |= LKMDBG_IMAGE_FLAG_DELETED;
	if (lkmdbg_image_is_main_exe(mm, start_vma))
		entry->flags |= LKMDBG_IMAGE_FLAG_MAIN_EXE;
	lkmdbg_image_fill_prot(entry, start_vma);

	base_addr = start_vma->vm_start - ((u64)start_vma->vm_pgoff << PAGE_SHIFT);
	entry->base_addr = base_addr;
	entry->segment_count = 1;

	for (;;) {
		struct vm_area_struct *next;
		u64 next_base;

		next = find_vma(mm, vma->vm_end);
		if (!next || !lkmdbg_image_same_backing(start_vma, next))
			break;

		if (next->vm_start < entry->start_addr)
			entry->start_addr = next->vm_start;
		if (next->vm_end > entry->end_addr)
			entry->end_addr = next->vm_end;
		if (next->vm_pgoff < entry->pgoff)
			entry->pgoff = next->vm_pgoff;

		next_base = next->vm_start - ((u64)next->vm_pgoff << PAGE_SHIFT);
		if (next_base < entry->base_addr)
			entry->base_addr = next_base;

		if ((u64)next->vm_flags & VM_SHARED)
			entry->flags |= LKMDBG_IMAGE_FLAG_SHARED;
		lkmdbg_image_fill_prot(entry, next);
		entry->segment_count++;
		vma = next;
	}

	return 0;
}

static long lkmdbg_image_copy_reply(void __user *argp,
				    struct lkmdbg_image_query_request *req,
				    struct lkmdbg_image_entry *entries,
				    size_t entries_bytes, char *names)
{
	if (copy_to_user(u64_to_user_ptr(req->entries_addr), entries,
			 entries_bytes))
		return -EFAULT;

	if (req->names_used &&
	    copy_to_user(u64_to_user_ptr(req->names_addr), names,
			 req->names_used))
		return -EFAULT;

	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_image_query(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_image_query_request req;
	struct lkmdbg_image_entry *entries = NULL;
	struct mm_struct *mm = NULL;
	char *names = NULL;
	char *path_buf = NULL;
	size_t entries_bytes;
	u64 cursor;
	u32 filled = 0;
	u32 names_used = 0;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_image_query(&req);
	if (ret)
		return ret;

	entries_bytes = req.max_entries * sizeof(*entries);
	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	if (req.names_size) {
		names = kzalloc(req.names_size, GFP_KERNEL);
		path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
		if (!names || !path_buf) {
			ret = -ENOMEM;
			goto out;
		}
	}

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret)
		goto out;

	cursor = req.start_addr;
	req.done = 1;
	req.next_addr = 0;

	mmap_read_lock(mm);
	while (filled < req.max_entries) {
		struct vm_area_struct *vma;
		u64 group_start;
		int name_ret;

		vma = lkmdbg_image_find_next_supported_vma(mm, cursor);
		if (!vma)
			break;

		group_start = vma->vm_start;
		ret = lkmdbg_image_fill_entry(mm, vma, &entries[filled]);
		if (ret) {
			mmap_read_unlock(mm);
			goto out;
		}

		name_ret = lkmdbg_image_fill_name(&req, &entries[filled], vma, names,
						  &names_used, path_buf);
		if (name_ret == -E2BIG) {
			if (!filled) {
				ret = -E2BIG;
				mmap_read_unlock(mm);
				goto out;
			}

			req.done = 0;
			req.next_addr = group_start;
			break;
		}
		if (name_ret) {
			ret = name_ret;
			mmap_read_unlock(mm);
			goto out;
		}

		filled++;
		cursor = entries[filled - 1].end_addr;
	}

	if (filled == req.max_entries) {
		struct vm_area_struct *next_vma;

		next_vma = lkmdbg_image_find_next_supported_vma(mm, cursor);
		if (next_vma) {
			req.done = 0;
			req.next_addr = next_vma->vm_start;
		}
	}
	mmap_read_unlock(mm);

	req.entries_filled = filled;
	req.names_used = names_used;
	ret = lkmdbg_image_copy_reply(argp, &req, entries, entries_bytes, names);

out:
	if (mm)
		mmput(mm);
	kfree(path_buf);
	kfree(names);
	kfree(entries);
	return ret;
}
