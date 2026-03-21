#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/types.h>

#include "lkmdbg_internal.h"

static struct lkmdbg_inline_hook *lkmdbg_seq_read_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_seq_read_registry;
static ssize_t (*lkmdbg_seq_read_orig)(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos);

static ssize_t lkmdbg_seq_read_replacement(struct file *file, char __user *buf,
					   size_t count, loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_hits++;
	mutex_unlock(&lkmdbg_state.lock);
	lkmdbg_hook_registry_note_hit(lkmdbg_seq_read_registry);

	if (!lkmdbg_seq_read_orig)
		return -ENOENT;

	ret = lkmdbg_seq_read_orig(file, buf, count, ppos);

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
	if (lkmdbg_seq_read_hook) {
		lkmdbg_hook_remove(lkmdbg_seq_read_hook);
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
