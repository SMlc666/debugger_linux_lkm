#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/types.h>

#include "lkmdbg_internal.h"

static struct lkmdbg_inline_hook *lkmdbg_seq_read_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_seq_read_registry;
static ssize_t (*lkmdbg_seq_read_orig)(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos);
static atomic_t lkmdbg_seq_read_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_seq_read_waitq);

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

	pr_info("lkmdbg: runtime hook active target=seq_read origin=%px trampoline=%px\n",
		target, orig_fn);
	return 0;
}

int lkmdbg_runtime_hooks_init(void)
{
	int ret = 0;

	atomic_set(&lkmdbg_seq_read_inflight, 0);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_active = false;
	lkmdbg_state.seq_read_hook_hits = 0;
	lkmdbg_state.seq_read_hook_last_ret = 0;
	mutex_unlock(&lkmdbg_state.lock);

	if (!hook_seq_read)
		return 0;

	ret = lkmdbg_install_seq_read_hook();
	if (ret)
		pr_err("lkmdbg: runtime seq_read hook install failed ret=%d\n", ret);

	return ret;
}

void lkmdbg_runtime_hooks_exit(void)
{
	long remaining = 1;

	if (lkmdbg_seq_read_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_seq_read_hook)) {
			remaining = wait_event_timeout(
				lkmdbg_seq_read_waitq,
				atomic_read(&lkmdbg_seq_read_inflight) == 0,
				msecs_to_jiffies(1000));
			if (!remaining)
				pr_warn("lkmdbg: seq_read hook drain timed out inflight=%d\n",
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
	mutex_unlock(&lkmdbg_state.lock);
}
