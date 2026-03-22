#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

#define LKMDBG_PHYS_MAX_XFER (256U * 1024U)
#define LKMDBG_PHYS_MAX_OPS 64U
#define LKMDBG_PHYS_MAX_BATCH_BYTES (1024U * 1024U)
#define LKMDBG_PHYS_MAX_WINDOW_BYTES (16U * PAGE_SIZE)

static int lkmdbg_validate_phys_op(const struct lkmdbg_phys_op *op)
{
	if (!op->length || op->length > LKMDBG_PHYS_MAX_XFER)
		return -EINVAL;

	if (!op->local_addr)
		return -EINVAL;

	if (op->phys_addr + op->length < op->phys_addr)
		return -EINVAL;

	if (op->flags & ~LKMDBG_PHYS_OP_FLAG_TARGET_VADDR)
		return -EINVAL;

	if (op->flags & LKMDBG_PHYS_OP_FLAG_TARGET_VADDR) {
		if (op->phys_addr >= (u64)TASK_SIZE_MAX)
			return -EINVAL;
		if (op->phys_addr + op->length > (u64)TASK_SIZE_MAX)
			return -EINVAL;
	}

	return 0;
}

static int lkmdbg_validate_phys_req(struct lkmdbg_phys_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->ops_addr || !req->op_count ||
	    req->op_count > LKMDBG_PHYS_MAX_OPS)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	return 0;
}

static bool lkmdbg_phys_range_is_ram(phys_addr_t base, size_t len)
{
	unsigned long start_pfn;
	unsigned long end_pfn;
	unsigned long pfn;

	if (!len)
		return false;

	start_pfn = PHYS_PFN(base);
	end_pfn = PHYS_PFN(base + len - 1);
	for (pfn = start_pfn; pfn <= end_pfn; pfn++) {
		if (!pfn_valid(pfn))
			return false;
	}

	return true;
}

static void *lkmdbg_phys_map_window(phys_addr_t base, size_t len,
				    bool *direct_map_out)
{
	void *addr;

	*direct_map_out = false;
	if (lkmdbg_phys_range_is_ram(base, len)) {
		*direct_map_out = true;
		return phys_to_virt(base);
	}

	addr = memremap(base, len, MEMREMAP_WB);
	if (!addr)
		return NULL;

	return addr;
}

static void lkmdbg_phys_unmap_window(void *addr, bool direct_map)
{
	if (!addr || direct_map)
		return;

	memunmap(addr);
}

static void lkmdbg_phys_accumulate_progress(const struct lkmdbg_phys_op *ops,
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

static long lkmdbg_phys_xfer_window(phys_addr_t phys_addr, u64 local_addr,
				    size_t length, bool write)
{
	size_t total_done = 0;

	while (total_done < length) {
		phys_addr_t window_phys;
		unsigned long window_offset;
		size_t window_len;
		size_t map_len;
		void *window_addr;
		void *copy_addr;
		void __user *user_addr;
		bool direct_map;

		window_phys = phys_addr + total_done;
		window_offset = offset_in_page(window_phys);
		window_len = min_t(size_t, length - total_done,
				   LKMDBG_PHYS_MAX_WINDOW_BYTES -
					   window_offset);
		map_len = PAGE_ALIGN(window_offset + window_len);

		window_addr = lkmdbg_phys_map_window(window_phys & PAGE_MASK,
						     map_len, &direct_map);
		if (!window_addr)
			break;

		copy_addr = (u8 *)window_addr + window_offset;
		user_addr = u64_to_user_ptr(local_addr + total_done);

		if (write) {
			if (copy_from_user(copy_addr, user_addr, window_len)) {
				lkmdbg_phys_unmap_window(window_addr, direct_map);
				return -EFAULT;
			}
		} else if (copy_to_user(user_addr, copy_addr, window_len)) {
			lkmdbg_phys_unmap_window(window_addr, direct_map);
			return -EFAULT;
		}

		lkmdbg_phys_unmap_window(window_addr, direct_map);
		total_done += window_len;
		cond_resched();
	}

	return (long)total_done;
}

static long lkmdbg_phys_xfer_target_vaddr(struct mm_struct *mm, u64 remote_addr,
					  u64 local_addr, size_t length,
					  bool write)
{
	size_t total_done = 0;

	while (total_done < length) {
		struct lkmdbg_target_pt_info pt_info;
		unsigned long addr;
		u64 leaf_size;
		u64 leaf_offset;
		size_t chunk_len;
		phys_addr_t phys_addr;
		long copied;
		int ret;

		addr = (unsigned long)(remote_addr + total_done);

		mmap_read_lock(mm);
		ret = lkmdbg_target_pt_lookup_locked(mm, addr, &pt_info);
		mmap_read_unlock(mm);
		if (ret)
			return ret;

		if (!(pt_info.flags & LKMDBG_TARGET_PT_FLAG_PRESENT) ||
		    !pt_info.page_shift)
			break;

		leaf_size = 1ULL << pt_info.page_shift;
		leaf_offset = addr & (leaf_size - 1);
		phys_addr = (phys_addr_t)pt_info.phys_addr;
		if (!(pt_info.flags & LKMDBG_TARGET_PT_FLAG_HUGE))
			phys_addr += leaf_offset;

		chunk_len = min_t(size_t, length - total_done,
				  (size_t)(leaf_size - leaf_offset));
		copied = lkmdbg_phys_xfer_window(phys_addr,
						 local_addr + total_done,
						 chunk_len, write);
		if (copied < 0)
			return copied;

		total_done += copied;
		if ((size_t)copied != chunk_len)
			break;
	}

	return (long)total_done;
}

static long lkmdbg_phys_xfer_ops(struct lkmdbg_phys_op *ops, u32 op_count,
				 struct mm_struct *target_mm, bool write)
{
	u64 batch_total = 0;
	u64 transferred = 0;
	u32 i;
	int ret;

	for (i = 0; i < op_count; i++) {
		ret = lkmdbg_validate_phys_op(&ops[i]);
		if (ret)
			return ret;

		if (batch_total + ops[i].length > LKMDBG_PHYS_MAX_BATCH_BYTES)
			return -E2BIG;

		batch_total += ops[i].length;
		ops[i].bytes_done = 0;
	}

	for (i = 0; i < op_count; i++) {
		long copied;

		if (ops[i].flags & LKMDBG_PHYS_OP_FLAG_TARGET_VADDR) {
			if (!target_mm)
				return -ENODEV;
			copied = lkmdbg_phys_xfer_target_vaddr(
				target_mm, ops[i].phys_addr, ops[i].local_addr,
				ops[i].length, write);
		} else {
			copied = lkmdbg_phys_xfer_window(
				(phys_addr_t)ops[i].phys_addr, ops[i].local_addr,
				ops[i].length, write);
		}
		if (copied < 0)
			return copied;

		ops[i].bytes_done = (u32)copied;
		transferred += copied;
		if ((u32)copied != ops[i].length)
			break;
	}

	return (long)transferred;
}

static long lkmdbg_phys_copy_reply(void __user *argp,
				   struct lkmdbg_phys_request *req,
				   struct lkmdbg_phys_op *ops, size_t ops_bytes,
				   long ret)
{
	if (copy_to_user(u64_to_user_ptr(req->ops_addr), ops, ops_bytes))
		return -EFAULT;

	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

static long lkmdbg_phys_xfer(struct lkmdbg_session *session, void __user *argp,
			     bool write)
{
	struct lkmdbg_phys_request req;
	struct lkmdbg_phys_op *ops;
	struct mm_struct *target_mm = NULL;
	size_t ops_bytes;
	u64 total_bytes = 0;
	u32 processed = 0;
	bool needs_target_mm = false;
	u32 i;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_phys_req(&req);
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

	for (i = 0; i < req.op_count; i++) {
		if (ops[i].flags & LKMDBG_PHYS_OP_FLAG_TARGET_VADDR) {
			needs_target_mm = true;
			break;
		}
	}

	if (needs_target_mm) {
		ret = lkmdbg_get_target_mm(session, &target_mm);
		if (ret) {
			kfree(ops);
			return ret;
		}
	}

	ret = lkmdbg_phys_xfer_ops(ops, req.op_count, target_mm, write);
	lkmdbg_phys_accumulate_progress(ops, req.op_count, &processed,
					&total_bytes);
	req.ops_done = processed;
	req.bytes_done = total_bytes;
	if (ret >= 0)
		ret = 0;
	ret = lkmdbg_phys_copy_reply(argp, &req, ops, ops_bytes, ret);

	if (target_mm)
		mmput(target_mm);
	kfree(ops);
	return ret;
}

long lkmdbg_phys_read(struct lkmdbg_session *session, void __user *argp)
{
	return lkmdbg_phys_xfer(session, argp, false);
}

long lkmdbg_phys_write(struct lkmdbg_session *session, void __user *argp)
{
	return lkmdbg_phys_xfer(session, argp, true);
}
