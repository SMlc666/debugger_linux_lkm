#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

#define LKMDBG_VMA_MAX_ENTRIES 256U
#define LKMDBG_VMA_MAX_NAMES_SIZE (128U * 1024U)

static int lkmdbg_validate_vma_query(struct lkmdbg_vma_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_VMA_MAX_ENTRIES)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (req->names_size > LKMDBG_VMA_MAX_NAMES_SIZE)
		return -E2BIG;

	if ((req->names_size && !req->names_addr) ||
	    (!req->names_size && req->names_addr))
		return -EINVAL;

	if (req->start_addr > (u64)TASK_SIZE_MAX)
		return -EINVAL;

	return 0;
}

static const char *lkmdbg_vma_special_name(struct mm_struct *mm,
					   struct vm_area_struct *vma,
					   u32 *flags)
{
	if (mm->start_stack >= vma->vm_start && mm->start_stack < vma->vm_end) {
		*flags |= LKMDBG_VMA_FLAG_STACK;
		return "[stack]";
	}

	if (mm->start_brk >= vma->vm_start && mm->start_brk < vma->vm_end) {
		*flags |= LKMDBG_VMA_FLAG_HEAP;
		return "[heap]";
	}

	return NULL;
}

static void lkmdbg_vma_fill_prot(struct lkmdbg_vma_entry *entry,
				 struct vm_area_struct *vma)
{
	u64 vm_flags = (u64)vma->vm_flags;

	if (vm_flags & VM_READ)
		entry->prot |= LKMDBG_VMA_PROT_READ;
	if (vm_flags & VM_WRITE)
		entry->prot |= LKMDBG_VMA_PROT_WRITE;
	if (vm_flags & VM_EXEC)
		entry->prot |= LKMDBG_VMA_PROT_EXEC;
	if (vm_flags & VM_MAYREAD)
		entry->prot |= LKMDBG_VMA_PROT_MAYREAD;
	if (vm_flags & VM_MAYWRITE)
		entry->prot |= LKMDBG_VMA_PROT_MAYWRITE;
	if (vm_flags & VM_MAYEXEC)
		entry->prot |= LKMDBG_VMA_PROT_MAYEXEC;
}

static int lkmdbg_vma_fill_name(struct mm_struct *mm, struct vm_area_struct *vma,
				struct lkmdbg_vma_query_request *req,
				struct lkmdbg_vma_entry *entry, char *names,
				u32 *names_used)
{
	const char *name = NULL;
	size_t name_len = 0;

	if (!req->names_size)
		return 0;

	if (vma->vm_file) {
		name = vma->vm_file->f_path.dentry->d_name.name;
		name_len = vma->vm_file->f_path.dentry->d_name.len + 1;
	} else {
		name = lkmdbg_vma_special_name(mm, vma, &entry->flags);
	}

	if (!name)
		return 0;

	if (!name_len)
		name_len = strlen(name) + 1;
	if (name_len > req->names_size - *names_used)
		return -E2BIG;

	entry->name_offset = *names_used;
	entry->name_size = (u32)name_len;
	memcpy(names + *names_used, name, name_len);
	*names_used += (u32)name_len;
	return 0;
}

static void lkmdbg_vma_fill_entry(struct mm_struct *mm,
				  struct vm_area_struct *vma,
				  struct lkmdbg_vma_entry *entry)
{
	struct inode *inode = NULL;
	u64 vm_flags = (u64)vma->vm_flags;

	memset(entry, 0, sizeof(*entry));
	entry->start_addr = vma->vm_start;
	entry->end_addr = vma->vm_end;
	entry->pgoff = vma->vm_pgoff;
	entry->vm_flags_raw = vm_flags;

	lkmdbg_vma_fill_prot(entry, vma);

	if (vm_flags & VM_SHARED)
		entry->flags |= LKMDBG_VMA_FLAG_SHARED;
	if (vm_flags & VM_PFNMAP)
		entry->flags |= LKMDBG_VMA_FLAG_PFNMAP;
	if (vm_flags & VM_IO)
		entry->flags |= LKMDBG_VMA_FLAG_IO;

	if (!vma->vm_file) {
		entry->flags |= LKMDBG_VMA_FLAG_ANON;
		lkmdbg_vma_special_name(mm, vma, &entry->flags);
		return;
	}

	entry->flags |= LKMDBG_VMA_FLAG_FILE;
	inode = file_inode(vma->vm_file);
	if (!inode)
		return;

	entry->inode = inode->i_ino;
	entry->dev_major = MAJOR(inode->i_sb->s_dev);
	entry->dev_minor = MINOR(inode->i_sb->s_dev);
}

static long lkmdbg_vma_copy_reply(void __user *argp,
				  struct lkmdbg_vma_query_request *req,
				  struct lkmdbg_vma_entry *entries,
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

long lkmdbg_vma_query(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_vma_query_request req;
	struct lkmdbg_vma_entry *entries = NULL;
	struct mm_struct *mm = NULL;
	char *names = NULL;
	u64 cursor;
	size_t entries_bytes;
	u32 filled = 0;
	u32 names_used = 0;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_vma_query(&req);
	if (ret)
		return ret;

	entries_bytes = req.max_entries * sizeof(*entries);
	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	if (req.names_size) {
		names = kzalloc(req.names_size, GFP_KERNEL);
		if (!names) {
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
		int name_ret;

		vma = find_vma(mm, cursor);
		if (!vma)
			break;

		if (vma->vm_end <= cursor) {
			ret = -EINVAL;
			mmap_read_unlock(mm);
			goto out;
		}

		lkmdbg_vma_fill_entry(mm, vma, &entries[filled]);
		name_ret = lkmdbg_vma_fill_name(mm, vma, &req, &entries[filled],
						names, &names_used);
		if (name_ret == -E2BIG) {
			if (!filled) {
				ret = -E2BIG;
				mmap_read_unlock(mm);
				goto out;
			}

			req.done = 0;
			req.next_addr = vma->vm_start;
			break;
		}
		if (name_ret) {
			ret = name_ret;
			mmap_read_unlock(mm);
			goto out;
		}

		filled++;
		cursor = vma->vm_end;
	}

	if (filled == req.max_entries) {
		struct vm_area_struct *next_vma;

		next_vma = find_vma(mm, cursor);
		if (next_vma) {
			req.done = 0;
			req.next_addr = cursor;
		}
	}
	mmap_read_unlock(mm);

	req.entries_filled = filled;
	req.names_used = names_used;
	ret = lkmdbg_vma_copy_reply(argp, &req, entries, entries_bytes, names);

out:
	if (mm)
		mmput(mm);
	kfree(names);
	kfree(entries);
	return ret;
}
