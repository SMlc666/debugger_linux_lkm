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

#define LKMDBG_VMA_MAX_ENTRIES 256U
#define LKMDBG_VMA_MAX_NAMES_SIZE (128U * 1024U)
#define LKMDBG_VMA_HASH_INIT 0xcbf29ce484222325ULL
#define LKMDBG_VMA_HASH_PRIME 0x100000001b3ULL

static u64 lkmdbg_vma_hash_bytes(u64 hash, const void *buf, size_t len)
{
	const u8 *bytes = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= bytes[i];
		hash *= LKMDBG_VMA_HASH_PRIME;
	}

	return hash;
}

static u64 lkmdbg_vma_generation_locked(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	u64 hash = LKMDBG_VMA_HASH_INIT;

	for (vma = find_vma(mm, 0); vma; vma = find_vma(mm, vma->vm_end)) {
		struct lkmdbg_target_vma_info info;

		lkmdbg_target_vma_fill_info(mm, vma, &info);
		hash = lkmdbg_vma_hash_bytes(hash, &info, sizeof(info));
	}

	return hash ? hash : 1;
}

static int lkmdbg_validate_vma_query(struct lkmdbg_vma_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_VMA_MAX_ENTRIES)
		return -EINVAL;

	if (req->flags & ~LKMDBG_VMA_QUERY_FLAG_FULL_PATH)
		return -EINVAL;

	if (req->match_flags_value & ~req->match_flags_mask)
		return -EINVAL;
	if (req->match_prot_value & ~req->match_prot_mask)
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

static bool lkmdbg_vma_matches_filter(const struct lkmdbg_vma_entry *entry,
				      const struct lkmdbg_vma_query_request *req)
{
	if ((entry->flags & req->match_flags_mask) != req->match_flags_value)
		return false;

	if ((entry->prot & req->match_prot_mask) != req->match_prot_value)
		return false;

	return true;
}

static int lkmdbg_vma_fill_name(struct mm_struct *mm, struct vm_area_struct *vma,
				struct lkmdbg_vma_query_request *req,
				struct lkmdbg_vma_entry *entry, char *names,
				char *path_buf,
				u32 *names_used)
{
	const char *name = NULL;
	size_t name_len = 0;

	if (!req->names_size)
		return 0;

	if (vma->vm_file) {
		if (req->flags & LKMDBG_VMA_QUERY_FLAG_FULL_PATH) {
			char *resolved;

			if (!path_buf)
				return -ENOMEM;

			resolved = d_path(&vma->vm_file->f_path, path_buf, PATH_MAX);
			if (IS_ERR(resolved))
				return PTR_ERR(resolved);

			name = resolved;
		} else {
			name = vma->vm_file->f_path.dentry->d_name.name;
			name_len = vma->vm_file->f_path.dentry->d_name.len + 1;
		}
	} else {
		name = lkmdbg_target_vma_special_name(mm, vma, &entry->flags);
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
	struct lkmdbg_target_vma_info info;

	lkmdbg_target_vma_fill_info(mm, vma, &info);
	memset(entry, 0, sizeof(*entry));
	entry->start_addr = info.start_addr;
	entry->end_addr = info.end_addr;
	entry->pgoff = info.pgoff;
	entry->inode = info.inode;
	entry->vm_flags_raw = info.vm_flags_raw;
	entry->prot = info.prot;
	entry->flags = info.flags;
	entry->dev_major = info.dev_major;
	entry->dev_minor = info.dev_minor;
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
	char *path_buf = NULL;
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

		if (req.flags & LKMDBG_VMA_QUERY_FLAG_FULL_PATH) {
			path_buf = kzalloc(PATH_MAX, GFP_KERNEL);
			if (!path_buf) {
				ret = -ENOMEM;
				goto out;
			}
		}
	}

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret)
		goto out;

	cursor = req.start_addr;
	req.done = 1;
	req.next_addr = 0;
	mmap_read_lock(mm);
	req.generation = lkmdbg_vma_generation_locked(mm);
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
		if (!lkmdbg_vma_matches_filter(&entries[filled], &req)) {
			cursor = vma->vm_end;
			continue;
		}

		name_ret = lkmdbg_vma_fill_name(mm, vma, &req, &entries[filled],
						names, path_buf, &names_used);
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
	kfree(path_buf);
	kfree(names);
	kfree(entries);
	return ret;
}
