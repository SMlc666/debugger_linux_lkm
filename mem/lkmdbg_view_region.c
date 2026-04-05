#include <linux/errno.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

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
};

static LIST_HEAD(lkmdbg_view_region_global_list);
static DEFINE_SPINLOCK(lkmdbg_view_region_global_lock);

static void lkmdbg_view_region_destroy(struct lkmdbg_view_region *region)
{
	if (!region)
		return;

	kfree(region->read_bytes);
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
		if (region->active_backend != LKMDBG_VIEW_BACKEND_EXTERNAL_READ ||
		    region->read_backing_type != LKMDBG_VIEW_BACKING_USER_BUFFER ||
		    !region->read_bytes) {
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
		if (copy_to_user(dst, region->read_bytes +
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
		memcpy(buf, region->read_bytes +
			      (size_t)(remote_addr - region->base_addr),
		       step);
		WRITE_ONCE(region->fault_count, region->fault_count + 1);
		remote_addr += step;
		buf += step;
		len -= step;
		lkmdbg_view_region_put(region);
	}
}

static int lkmdbg_view_region_prepare_backend(u32 requested_backend,
					      u32 *active_backend_out)
{
	u32 active_backend;
	int ret;

	switch (requested_backend) {
	case LKMDBG_VIEW_BACKEND_AUTO:
	case LKMDBG_VIEW_BACKEND_EXTERNAL_READ:
		active_backend = LKMDBG_VIEW_BACKEND_EXTERNAL_READ;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = lkmdbg_external_read_hooks_ensure();
	if (ret)
		return ret;

	*active_backend_out = active_backend;
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
	if (req->source_addr & ~PAGE_MASK)
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

	ret = lkmdbg_view_region_prepare_backend(req.backend, &active_backend);
	if (ret)
		return ret;

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

	if (region->active_backend != LKMDBG_VIEW_BACKEND_EXTERNAL_READ) {
		ret = -EOPNOTSUPP;
		goto out_put;
	}
	if (req.view_kind != LKMDBG_VIEW_KIND_READ) {
		ret = -EOPNOTSUPP;
		goto out_put;
	}

	switch (req.backing_type) {
	case LKMDBG_VIEW_BACKING_ORIGINAL:
		mutex_lock(&session->lock);
		old_backing = region->read_bytes;
		region->read_bytes = NULL;
		region->read_source_id = 0;
		region->read_backing_type = LKMDBG_VIEW_BACKING_ORIGINAL;
		region->state &= ~LKMDBG_VIEW_REGION_STATE_ACTIVE;
		mutex_unlock(&session->lock);
		kfree(old_backing);
		break;
	case LKMDBG_VIEW_BACKING_USER_BUFFER:
		if (!req.source_addr || req.source_length != region->length) {
			ret = -EINVAL;
			goto out_put;
		}
		backing = kmalloc(region->length, GFP_KERNEL);
		if (!backing) {
			ret = -ENOMEM;
			goto out_put;
		}
		if (copy_from_user(backing, u64_to_user_ptr(req.source_addr),
				   region->length)) {
			ret = -EFAULT;
			goto out_free;
		}
		mutex_lock(&session->lock);
		old_backing = region->read_bytes;
		session->next_view_source_id++;
		region->read_source_id = session->next_view_source_id;
		region->read_bytes = backing;
		backing = NULL;
		region->read_backing_type = LKMDBG_VIEW_BACKING_USER_BUFFER;
		region->state |= LKMDBG_VIEW_REGION_STATE_ACTIVE;
		mutex_unlock(&session->lock);
		kfree(old_backing);
		break;
	default:
		ret = -EOPNOTSUPP;
		goto out_put;
	}

	if (copy_to_user(argp, &req, sizeof(req))) {
		ret = -EFAULT;
		goto out_put;
	}
	goto out_put;

out_free:
	kfree(backing);
out_put:
	lkmdbg_view_region_put(region);
	return ret;
}

long lkmdbg_set_view_policy(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_view_policy_request req;
	struct lkmdbg_view_region *region;
	u32 active_backend;
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
	ret = lkmdbg_view_region_prepare_backend(req.backend, &active_backend);
	if (ret) {
		mutex_unlock(&session->lock);
		return ret;
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
