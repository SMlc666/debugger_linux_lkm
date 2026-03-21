#include <linux/cpu.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>

#include "kp_hook.h"
#include "lkmdbg_internal.h"

struct lkmdbg_inline_hook {
	struct list_head node;
	void *target;
	void *origin;
	void *replacement;
	bool active;
	kp_hook_t core;
};

struct lkmdbg_patch_ctx {
	void *addr;
	const u32 *insns;
	u32 words;
};

static LIST_HEAD(lkmdbg_hook_list);
static DEFINE_MUTEX(lkmdbg_hook_lock);

static int lkmdbg_write_text_insn(void *addr, u32 insn)
{
	if (!lkmdbg_symbols.aarch64_insn_write || !lkmdbg_symbols.flush_icache_range)
		return -ENOENT;

	if (lkmdbg_symbols.aarch64_insn_write(addr, insn))
		return -EIO;

	return 0;
}

static int lkmdbg_write_text_words(void *addr, const u32 *insns, u32 words)
{
	u32 i;
	int ret;
	u8 *dst = addr;

	for (i = 0; i < words; i++) {
		ret = lkmdbg_write_text_insn(dst + i * sizeof(u32), insns[i]);
		if (ret)
			return ret;
	}

	lkmdbg_symbols.flush_icache_range((unsigned long)addr,
					  (unsigned long)addr +
					  words * sizeof(u32));
	return 0;
}

static int lkmdbg_patch_stop_machine(void *data)
{
	struct lkmdbg_patch_ctx *ctx = data;

	return lkmdbg_write_text_words(ctx->addr, ctx->insns, ctx->words);
}

static int lkmdbg_patch_target_words(void *addr, const u32 *insns, u32 words)
{
	struct lkmdbg_patch_ctx ctx = {
		.addr = addr,
		.insns = insns,
		.words = words,
	};

	return stop_machine(lkmdbg_patch_stop_machine, &ctx, cpu_online_mask);
}

int lkmdbg_hooks_init(void)
{
	return 0;
}

void lkmdbg_hooks_exit(void)
{
	struct lkmdbg_inline_hook *hook;
	struct lkmdbg_inline_hook *tmp;

	mutex_lock(&lkmdbg_hook_lock);
	list_for_each_entry_safe(hook, tmp, &lkmdbg_hook_list, node) {
		list_del_init(&hook->node);
		mutex_unlock(&lkmdbg_hook_lock);
		lkmdbg_hook_remove(hook);
		mutex_lock(&lkmdbg_hook_lock);
	}
	mutex_unlock(&lkmdbg_hook_lock);
}

int lkmdbg_hooks_prepare_exec_pool(void)
{
	return 0;
}

int lkmdbg_hook_alloc_exec(struct lkmdbg_inline_hook *hook)
{
	if (!hook)
		return -EINVAL;

	return 0;
}

int lkmdbg_hook_create(void *target, void *replacement,
		       struct lkmdbg_inline_hook **hook_out,
		       void **orig_out)
{
	struct lkmdbg_inline_hook *hook;
	struct lkmdbg_inline_hook *iter;
	kp_hook_err_t ret;

	if (!target || !replacement || !hook_out || !orig_out)
		return -EINVAL;

	hook = kzalloc(sizeof(*hook), GFP_KERNEL);
	if (!hook)
		return -ENOMEM;

	INIT_LIST_HEAD(&hook->node);
	hook->target = target;
	hook->origin = (void *)kp_branch_func_addr((u64)target);
	hook->replacement = replacement;
	hook->core.func_addr = (u64)target;
	hook->core.origin_addr = (u64)hook->origin;
	hook->core.replace_addr = (u64)replacement;
	hook->core.relo_addr = (u64)hook->core.relo_insts;

	mutex_lock(&lkmdbg_hook_lock);
	list_for_each_entry(iter, &lkmdbg_hook_list, node) {
		if (iter->origin == hook->origin) {
			mutex_unlock(&lkmdbg_hook_lock);
			kfree(hook);
			return -EEXIST;
		}
	}
	mutex_unlock(&lkmdbg_hook_lock);

	ret = kp_hook_prepare(&hook->core);
	if (ret) {
		pr_err("lkmdbg: kp hook prepare failed target=%px origin=%px ret=%d\n",
		       hook->target, hook->origin, (int)ret);
		kfree(hook);
		return -EINVAL;
	}

	mutex_lock(&lkmdbg_hook_lock);
	list_add_tail(&hook->node, &lkmdbg_hook_list);
	mutex_unlock(&lkmdbg_hook_lock);

	*hook_out = hook;
	*orig_out = (void *)hook->core.relo_addr;
	return 0;
}

int lkmdbg_hook_prepare_exec(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	if (!hook)
		return -EINVAL;

	if (orig_out)
		*orig_out = (void *)hook->core.relo_addr;
	return 0;
}

int lkmdbg_hook_patch_target(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	int ret;

	if (!hook)
		return -EINVAL;

	if (hook->active)
		return -EALREADY;

	ret = lkmdbg_patch_target_words((void *)hook->core.origin_addr,
					hook->core.tramp_insts,
					hook->core.tramp_insts_num);
	if (ret) {
		pr_err("lkmdbg: kp hook install failed target=%px origin=%px ret=%d\n",
		       hook->target, hook->origin, ret);
		return ret;
	}

	mutex_lock(&lkmdbg_hook_lock);
	hook->active = true;
	mutex_unlock(&lkmdbg_hook_lock);

	if (orig_out)
		*orig_out = (void *)hook->core.relo_addr;

	pr_info("lkmdbg: kp hook installed target=%px origin=%px replacement=%px trampoline=%px\n",
		hook->target, hook->origin, hook->replacement,
		(void *)hook->core.relo_addr);
	return 0;
}

int lkmdbg_hook_activate(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	return lkmdbg_hook_patch_target(hook, orig_out);
}

int lkmdbg_hook_install(void *target, void *replacement,
			struct lkmdbg_inline_hook **hook_out,
			void **orig_out)
{
	struct lkmdbg_inline_hook *hook;
	int ret;

	ret = lkmdbg_hook_create(target, replacement, &hook, orig_out);
	if (ret)
		return ret;

	ret = lkmdbg_hook_activate(hook, orig_out);
	if (ret) {
		lkmdbg_hook_remove(hook);
		return ret;
	}

	*hook_out = hook;
	return 0;
}

void lkmdbg_hook_remove(struct lkmdbg_inline_hook *hook)
{
	int ret;

	if (!hook)
		return;

	if (hook->active) {
		ret = lkmdbg_patch_target_words((void *)hook->core.origin_addr,
						hook->core.origin_insts,
						hook->core.tramp_insts_num);
		if (ret)
			pr_warn("lkmdbg: kp hook rollback failed origin=%px ret=%d\n",
				hook->origin, ret);
	}

	mutex_lock(&lkmdbg_hook_lock);
	if (!list_empty(&hook->node))
		list_del_init(&hook->node);
	mutex_unlock(&lkmdbg_hook_lock);

	kfree(hook);
}
