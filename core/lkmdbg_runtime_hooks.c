#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/pid_namespace.h>
#include <linux/sched/signal.h>
#include <linux/types.h>

#include "lkmdbg_internal.h"

static struct lkmdbg_inline_hook *lkmdbg_seq_read_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_seq_read_registry;
static ssize_t (*lkmdbg_seq_read_orig)(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos);
static atomic_t lkmdbg_seq_read_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_seq_read_waitq);

static struct lkmdbg_inline_hook *lkmdbg_has_pid_permissions_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_has_pid_permissions_registry;
static bool (*lkmdbg_has_pid_permissions_orig)(struct pid_namespace *ns,
					       struct task_struct *task,
					       int hide_pid_min);
static atomic_t lkmdbg_has_pid_permissions_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_has_pid_permissions_waitq);
static pid_t lkmdbg_owner_proc_hidden_tgid;
static bool lkmdbg_owner_proc_hidden_enabled;

static void *lkmdbg_lookup_has_pid_permissions(void)
{
	unsigned long addr;

	addr = lkmdbg_lookup_runtime_symbol_any("has_pid_permissions");
	if (!addr)
		addr = lkmdbg_lookup_runtime_symbol_prefix("has_pid_permissions");
	return (void *)addr;
}

static ssize_t lkmdbg_seq_read_replacement(struct file *file, char __user *buf,
					   size_t count, loff_t *ppos)
{
	struct lkmdbg_hook_registry_entry *registry;
	ssize_t (*orig)(struct file *file, char __user *buf, size_t count,
			loff_t *ppos);
	ssize_t ret;

	atomic_inc(&lkmdbg_seq_read_inflight);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_hits++;
	mutex_unlock(&lkmdbg_state.lock);

	registry = READ_ONCE(lkmdbg_seq_read_registry);
	if (registry)
		lkmdbg_hook_registry_note_hit(registry);

	orig = READ_ONCE(lkmdbg_seq_read_orig);
	if (!orig)
		ret = -ENOENT;
	else
		ret = orig(file, buf, count, ppos);

	if (atomic_dec_and_test(&lkmdbg_seq_read_inflight))
		wake_up_all(&lkmdbg_seq_read_waitq);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_last_ret = (int)ret;
	mutex_unlock(&lkmdbg_state.lock);

	return ret;
}

static int lkmdbg_install_seq_read_hook(void)
{
	void *target;
	void *orig_fn = NULL;
	int ret;

	if (!lkmdbg_symbols.kallsyms_lookup_name)
		return -ENOENT;

	target = (void *)lkmdbg_symbols.kallsyms_lookup_name("seq_read");
	if (!target)
		return -ENOENT;

	lkmdbg_seq_read_registry = lkmdbg_hook_registry_register("seq_read",
							target,
							lkmdbg_seq_read_replacement);
	if (!lkmdbg_seq_read_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, lkmdbg_seq_read_replacement,
				  &lkmdbg_seq_read_hook, &orig_fn);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_last_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_seq_read_registry, ret);
		lkmdbg_seq_read_registry = NULL;
		return ret;
	}

	lkmdbg_seq_read_orig = orig_fn;
	lkmdbg_hook_registry_mark_installed(lkmdbg_seq_read_registry, target,
						orig_fn, 0);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_active = true;
	mutex_unlock(&lkmdbg_state.lock);

	lkmdbg_pr_info("lkmdbg: runtime hook active target=seq_read origin=%px trampoline=%px\n",
		target, orig_fn);
	return 0;
}

static bool lkmdbg_has_pid_permissions_replacement(struct pid_namespace *ns,
						   struct task_struct *task,
						   int hide_pid_min)
{
	struct lkmdbg_hook_registry_entry *registry;
	bool (*orig)(struct pid_namespace *ns, struct task_struct *task,
		     int hide_pid_min);
	bool ret = false;
	pid_t hidden_tgid;
	bool hidden_enabled;

	atomic_inc(&lkmdbg_has_pid_permissions_inflight);

	registry = READ_ONCE(lkmdbg_has_pid_permissions_registry);
	if (registry)
		lkmdbg_hook_registry_note_hit(registry);

	orig = READ_ONCE(lkmdbg_has_pid_permissions_orig);
	if (orig)
		ret = orig(ns, task, hide_pid_min);

	hidden_enabled = READ_ONCE(lkmdbg_owner_proc_hidden_enabled);
	hidden_tgid = READ_ONCE(lkmdbg_owner_proc_hidden_tgid);
	if (ret && hidden_enabled && hidden_tgid > 0 && task &&
	    task_tgid_nr(task) == hidden_tgid &&
	    task_tgid_nr(current) != hidden_tgid) {
		ret = false;
	}

	if (atomic_dec_and_test(&lkmdbg_has_pid_permissions_inflight))
		wake_up_all(&lkmdbg_has_pid_permissions_waitq);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_hits++;
	mutex_unlock(&lkmdbg_state.lock);

	return ret;
}

static int lkmdbg_install_owner_proc_hide_hook(void)
{
	void *target;
	void *orig_fn = NULL;
	int ret;

	if (lkmdbg_has_pid_permissions_hook)
		return 0;

	target = lkmdbg_lookup_has_pid_permissions();
	if (!target)
		return -EOPNOTSUPP;

	lkmdbg_has_pid_permissions_registry = lkmdbg_hook_registry_register(
		"has_pid_permissions", target,
		lkmdbg_has_pid_permissions_replacement);
	if (!lkmdbg_has_pid_permissions_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, lkmdbg_has_pid_permissions_replacement,
				  &lkmdbg_has_pid_permissions_hook, &orig_fn);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_last_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_has_pid_permissions_registry,
						ret);
		lkmdbg_has_pid_permissions_registry = NULL;
		return ret;
	}

	lkmdbg_has_pid_permissions_orig = orig_fn;
	lkmdbg_hook_registry_mark_installed(lkmdbg_has_pid_permissions_registry,
					    target, orig_fn, 0);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_active = true;
	mutex_unlock(&lkmdbg_state.lock);

	lkmdbg_pr_info("lkmdbg: runtime hook active target=has_pid_permissions origin=%px trampoline=%px\n",
		       target, orig_fn);
	return 0;
}

bool lkmdbg_runtime_hook_owner_proc_hide_supported(void)
{
	return lkmdbg_lookup_has_pid_permissions() != NULL;
}

int lkmdbg_runtime_hook_set_owner_proc_hidden(pid_t owner_tgid, bool hidden)
{
	int ret;

	if (!hidden) {
		WRITE_ONCE(lkmdbg_owner_proc_hidden_enabled, false);
		WRITE_ONCE(lkmdbg_owner_proc_hidden_tgid, 0);
		mutex_lock(&lkmdbg_state.lock);
		lkmdbg_state.owner_proc_hidden = false;
		lkmdbg_state.owner_proc_hidden_tgid = 0;
		mutex_unlock(&lkmdbg_state.lock);
		return 0;
	}

	if (owner_tgid <= 0)
		return -EINVAL;

	ret = lkmdbg_install_owner_proc_hide_hook();
	if (ret)
		return ret;

	WRITE_ONCE(lkmdbg_owner_proc_hidden_tgid, owner_tgid);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_enabled, true);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hidden = true;
	lkmdbg_state.owner_proc_hidden_tgid = owner_tgid;
	lkmdbg_state.owner_proc_hide_hook_last_ret = 0;
	mutex_unlock(&lkmdbg_state.lock);
	return 0;
}

int lkmdbg_runtime_hooks_init(void)
{
	int ret = 0;

	atomic_set(&lkmdbg_seq_read_inflight, 0);
	atomic_set(&lkmdbg_has_pid_permissions_inflight, 0);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_tgid, 0);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_enabled, false);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_active = false;
	lkmdbg_state.seq_read_hook_hits = 0;
	lkmdbg_state.seq_read_hook_last_ret = 0;
	lkmdbg_state.owner_proc_hidden = false;
	lkmdbg_state.owner_proc_hidden_tgid = 0;
	lkmdbg_state.owner_proc_hide_hook_active = false;
	lkmdbg_state.owner_proc_hide_hook_hits = 0;
	lkmdbg_state.owner_proc_hide_hook_last_ret = 0;
	mutex_unlock(&lkmdbg_state.lock);

	if (!hook_seq_read)
		return 0;

	ret = lkmdbg_install_seq_read_hook();
	if (ret)
		lkmdbg_pr_err("lkmdbg: runtime seq_read hook install failed ret=%d\n", ret);

	return ret;
}

void lkmdbg_runtime_hooks_exit(void)
{
	long remaining = 1;

	WRITE_ONCE(lkmdbg_owner_proc_hidden_enabled, false);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_tgid, 0);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hidden = false;
	lkmdbg_state.owner_proc_hidden_tgid = 0;
	mutex_unlock(&lkmdbg_state.lock);

	if (lkmdbg_has_pid_permissions_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_has_pid_permissions_hook)) {
			remaining = wait_event_timeout(
				lkmdbg_has_pid_permissions_waitq,
				atomic_read(&lkmdbg_has_pid_permissions_inflight) == 0,
				msecs_to_jiffies(1000));
			if (!remaining)
				lkmdbg_pr_warn("lkmdbg: has_pid_permissions hook drain timed out inflight=%d\n",
					atomic_read(&lkmdbg_has_pid_permissions_inflight));
		}

		lkmdbg_hook_destroy(lkmdbg_has_pid_permissions_hook);
		lkmdbg_has_pid_permissions_hook = NULL;
	}
	if (lkmdbg_has_pid_permissions_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_has_pid_permissions_registry,
						0);
		lkmdbg_has_pid_permissions_registry = NULL;
	}
	lkmdbg_has_pid_permissions_orig = NULL;

	if (lkmdbg_seq_read_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_seq_read_hook)) {
			remaining = wait_event_timeout(
				lkmdbg_seq_read_waitq,
				atomic_read(&lkmdbg_seq_read_inflight) == 0,
				msecs_to_jiffies(1000));
			if (!remaining)
				lkmdbg_pr_warn("lkmdbg: seq_read hook drain timed out inflight=%d\n",
					atomic_read(&lkmdbg_seq_read_inflight));
		}

		lkmdbg_hook_destroy(lkmdbg_seq_read_hook);
		lkmdbg_seq_read_hook = NULL;
	}
	if (lkmdbg_seq_read_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_seq_read_registry, 0);
		lkmdbg_seq_read_registry = NULL;
	}

	lkmdbg_seq_read_orig = NULL;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_active = false;
	lkmdbg_state.owner_proc_hide_hook_active = false;
	mutex_unlock(&lkmdbg_state.lock);
}
