#include <linux/errno.h>
#include <linux/kallsyms.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "lkmdbg_internal.h"

static struct mutex *lkmdbg_module_mutex;
static struct list_head *lkmdbg_modules_head;
static struct kobject *lkmdbg_module_kobj_parent;

static bool lkmdbg_stealth_module_hidden_locked(void)
{
	return list_empty(&THIS_MODULE->list);
}

static bool lkmdbg_stealth_sysfs_hidden_locked(void)
{
	return !THIS_MODULE->mkobj.kobj.state_in_sysfs;
}

static int lkmdbg_stealth_validate_request(struct lkmdbg_stealth_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags & ~lkmdbg_stealth_supported_flags())
		return -EOPNOTSUPP;

	return 0;
}

u32 lkmdbg_stealth_current_flags(void)
{
	u32 flags = 0;

	mutex_lock(&lkmdbg_state.lock);
	if (lkmdbg_state.debugfs_active)
		flags |= LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE;
	if (lkmdbg_state.module_list_hidden)
		flags |= LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN;
	if (lkmdbg_state.sysfs_module_hidden)
		flags |= LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN;
	mutex_unlock(&lkmdbg_state.lock);

	return flags;
}

u32 lkmdbg_stealth_supported_flags(void)
{
	return READ_ONCE(lkmdbg_state.stealth_supported_flags);
}

static int lkmdbg_stealth_set_module_hidden(bool hidden)
{
	bool actual_hidden;
	int ret = 0;

	if (!lkmdbg_module_mutex || !lkmdbg_modules_head) {
		if (!hidden) {
			mutex_lock(&lkmdbg_state.lock);
			lkmdbg_state.module_list_hidden = false;
			mutex_unlock(&lkmdbg_state.lock);
			return 0;
		}
		return -EOPNOTSUPP;
	}

	if (hidden && !READ_ONCE(lkmdbg_state.proc_version_hook_active))
		return -EOPNOTSUPP;

	mutex_lock(lkmdbg_module_mutex);
	if (hidden) {
		if (!list_empty(&THIS_MODULE->list))
			list_del_init(&THIS_MODULE->list);
	} else if (list_empty(&THIS_MODULE->list)) {
		list_add(&THIS_MODULE->list, lkmdbg_modules_head);
	}
	actual_hidden = lkmdbg_stealth_module_hidden_locked();
	mutex_unlock(lkmdbg_module_mutex);

	if (actual_hidden != hidden) {
		pr_warn("lkmdbg: stealth module-list state mismatch requested=%u actual=%u\n",
			hidden, actual_hidden);
		ret = -EIO;
	}

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.module_list_hidden = actual_hidden;
	mutex_unlock(&lkmdbg_state.lock);

	return ret;
}

static int lkmdbg_stealth_set_sysfs_hidden(bool hidden)
{
	bool actual_hidden;
	int ret = 0;

	if (!lkmdbg_module_mutex || !lkmdbg_module_kobj_parent) {
		if (!hidden) {
			mutex_lock(&lkmdbg_state.lock);
			lkmdbg_state.sysfs_module_hidden = false;
			mutex_unlock(&lkmdbg_state.lock);
			return 0;
		}
		return -EOPNOTSUPP;
	}

	if (hidden && !READ_ONCE(lkmdbg_state.proc_version_hook_active))
		return -EOPNOTSUPP;

	mutex_lock(lkmdbg_module_mutex);
	if (hidden) {
		if (THIS_MODULE->mkobj.kobj.state_in_sysfs)
			kobject_del(&THIS_MODULE->mkobj.kobj);
	} else if (!THIS_MODULE->mkobj.kobj.state_in_sysfs) {
		ret = kobject_add(&THIS_MODULE->mkobj.kobj,
				  lkmdbg_module_kobj_parent, "%s",
				  module_name(THIS_MODULE));
	}
	actual_hidden = lkmdbg_stealth_sysfs_hidden_locked();
	mutex_unlock(lkmdbg_module_mutex);

	if (!ret && actual_hidden != hidden) {
		pr_warn("lkmdbg: stealth sysfs state mismatch requested=%u actual=%u\n",
			hidden, actual_hidden);
		ret = -EIO;
	}
	if (ret) {
		pr_warn("lkmdbg: stealth sysfs transition failed hidden=%u ret=%d\n",
			hidden, ret);
	}

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.sysfs_module_hidden = actual_hidden;
	mutex_unlock(&lkmdbg_state.lock);

	return ret;
}

static int lkmdbg_stealth_apply_flags(u32 flags)
{
	u32 old_flags;
	int ret;

	old_flags = lkmdbg_stealth_current_flags();

	ret = lkmdbg_debugfs_set_visible(!!(flags &
					    LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE));
	if (ret)
		return ret;

	ret = lkmdbg_stealth_set_module_hidden(
		!!(flags & LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN));
	if (ret) {
		lkmdbg_debugfs_set_visible(!!(old_flags &
					      LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE));
		return ret;
	}

	ret = lkmdbg_stealth_set_sysfs_hidden(
		!!(flags & LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN));
	if (ret) {
		lkmdbg_stealth_set_module_hidden(
			!!(old_flags & LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN));
		lkmdbg_debugfs_set_visible(!!(old_flags &
					      LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE));
		return ret;
	}

	return 0;
}

int lkmdbg_stealth_init(void)
{
	u32 supported_flags = LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE;
	bool module_hidden = false;
	bool sysfs_hidden = false;

	lkmdbg_module_mutex = NULL;
	lkmdbg_modules_head = NULL;
	lkmdbg_module_kobj_parent = NULL;
	if (lkmdbg_symbols.kallsyms_lookup_name) {
		lkmdbg_module_mutex = (struct mutex *)
			lkmdbg_symbols.kallsyms_lookup_name("module_mutex");
		lkmdbg_modules_head = (struct list_head *)
			lkmdbg_symbols.kallsyms_lookup_name("modules");
		if (lkmdbg_module_mutex && lkmdbg_modules_head)
			supported_flags |=
				LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN;
	}
	if (lkmdbg_module_mutex && THIS_MODULE->mkobj.kobj.parent) {
		lkmdbg_module_kobj_parent = THIS_MODULE->mkobj.kobj.parent;
		supported_flags |= LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN;
	}

	if ((supported_flags & LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN) &&
	    lkmdbg_module_mutex && lkmdbg_modules_head) {
		mutex_lock(lkmdbg_module_mutex);
		module_hidden = lkmdbg_stealth_module_hidden_locked();
		mutex_unlock(lkmdbg_module_mutex);
		if (module_hidden)
			pr_warn("lkmdbg: module list is already hidden at init; will restore on exit\n");
	}
	if ((supported_flags & LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN) &&
	    lkmdbg_module_mutex) {
		mutex_lock(lkmdbg_module_mutex);
		sysfs_hidden = lkmdbg_stealth_sysfs_hidden_locked();
		mutex_unlock(lkmdbg_module_mutex);
		if (sysfs_hidden)
			pr_warn("lkmdbg: sysfs module kobject is already hidden at init; will restore on exit\n");
	}

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.module_list_hidden = module_hidden;
	lkmdbg_state.sysfs_module_hidden = sysfs_hidden;
	lkmdbg_state.stealth_supported_flags = supported_flags;
	mutex_unlock(&lkmdbg_state.lock);

	return 0;
}

void lkmdbg_stealth_exit(void)
{
	bool need_restore = READ_ONCE(lkmdbg_state.module_list_hidden);
	bool need_restore_sysfs = READ_ONCE(lkmdbg_state.sysfs_module_hidden);

	if (!need_restore &&
	    lkmdbg_module_mutex && lkmdbg_modules_head) {
		mutex_lock(lkmdbg_module_mutex);
		need_restore = lkmdbg_stealth_module_hidden_locked();
		mutex_unlock(lkmdbg_module_mutex);
	}
	if (!need_restore_sysfs &&
	    lkmdbg_module_mutex && lkmdbg_module_kobj_parent) {
		mutex_lock(lkmdbg_module_mutex);
		need_restore_sysfs = lkmdbg_stealth_sysfs_hidden_locked();
		mutex_unlock(lkmdbg_module_mutex);
	}

	if (need_restore_sysfs &&
	    lkmdbg_stealth_set_sysfs_hidden(false))
		pr_warn("lkmdbg: failed to restore sysfs module visibility during exit\n");

	if (need_restore && lkmdbg_stealth_set_module_hidden(false))
		pr_warn("lkmdbg: failed to restore module list visibility during exit\n");
}

long lkmdbg_set_stealth(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_stealth_request req;
	long ret;

	(void)session;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_stealth_validate_request(&req);
	if (ret)
		return ret;

	ret = lkmdbg_stealth_apply_flags(req.flags);
	if (ret)
		return ret;

	req.flags = lkmdbg_stealth_current_flags();
	req.supported_flags = lkmdbg_stealth_supported_flags();

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_get_stealth(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_stealth_request req;

	(void)session;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req))
		return -EINVAL;

	req.flags = lkmdbg_stealth_current_flags();
	req.supported_flags = lkmdbg_stealth_supported_flags();

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}
