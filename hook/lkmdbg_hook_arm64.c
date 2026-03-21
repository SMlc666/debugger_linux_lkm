#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>

#ifdef CONFIG_ARM64
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

#include "kp_hook.h"
#include "lkmdbg_internal.h"

struct lkmdbg_inline_hook {
	struct list_head node;
	void *target;
	void *origin;
	void *replacement;
	void *exec_buf;
	size_t exec_size;
	bool exec_ready;
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
static void *lkmdbg_alias_page;
static pte_t *lkmdbg_alias_ptep;
static pte_t lkmdbg_alias_pte;

static pte_t *lkmdbg_lookup_kernel_pte(unsigned long addr)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;

	if (!lkmdbg_symbols.init_mm)
		return NULL;

	pgdp = pgd_offset(lkmdbg_symbols.init_mm, addr);
	if (pgd_none(*pgdp) || pgd_bad(*pgdp))
		return NULL;

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(*p4dp) || p4d_bad(*p4dp))
		return NULL;

	pudp = pud_offset(p4dp, addr);
	if (pud_none(*pudp) || pud_bad(*pudp) || pud_sect(*pudp))
		return NULL;

	pmdp = pmd_offset(pudp, addr);
	if (pmd_none(*pmdp) || pmd_bad(*pmdp) || pmd_sect(*pmdp))
		return NULL;

	return pte_offset_kernel(pmdp, addr);
}

static phys_addr_t lkmdbg_text_phys_base(void *addr)
{
	unsigned long base = (unsigned long)addr & PAGE_MASK;
	struct page *page;

	if (is_vmalloc_or_module_addr((const void *)base)) {
		page = vmalloc_to_page((const void *)base);
		if (!page)
			return 0;
		return page_to_phys(page);
	}

	return __pa_symbol(base);
}

static int lkmdbg_alias_map_page(void *addr, void **alias_out)
{
	unsigned long offset;
	phys_addr_t phys;
	pte_t new_pte;

	if (!lkmdbg_alias_page || !lkmdbg_alias_ptep)
		return -EOPNOTSUPP;

	phys = lkmdbg_text_phys_base(addr);
	if (!phys)
		return -EFAULT;

	new_pte = pfn_pte(PHYS_PFN(phys), pte_pgprot(lkmdbg_alias_pte));
	set_pte(lkmdbg_alias_ptep, new_pte);
	flush_tlb_kernel_range((unsigned long)lkmdbg_alias_page,
			       (unsigned long)lkmdbg_alias_page + PAGE_SIZE);

	offset = (unsigned long)addr & ~PAGE_MASK;
	*alias_out = (void *)((unsigned long)lkmdbg_alias_page + offset);
	return 0;
}

static void lkmdbg_alias_unmap_page(void)
{
	if (!lkmdbg_alias_page || !lkmdbg_alias_ptep)
		return;

	set_pte(lkmdbg_alias_ptep, lkmdbg_alias_pte);
	flush_tlb_kernel_range((unsigned long)lkmdbg_alias_page,
			       (unsigned long)lkmdbg_alias_page + PAGE_SIZE);
}

static int lkmdbg_write_text_insn(void *addr, u32 insn)
{
	void *alias_addr;
	int ret;

	if (!lkmdbg_symbols.flush_icache_range)
		return -ENOENT;

	ret = lkmdbg_alias_map_page(addr, &alias_addr);
	if (ret)
		return ret;

	WRITE_ONCE(*(u32 *)alias_addr, insn);
	lkmdbg_symbols.flush_icache_range((unsigned long)addr,
					  (unsigned long)addr + sizeof(insn));
	lkmdbg_alias_unmap_page();
	return 0;
}

static int lkmdbg_make_page_executable(void *addr)
{
	unsigned long page_addr = (unsigned long)addr & PAGE_MASK;
	pte_t *ptep;
	pte_t pte;

	ptep = lkmdbg_lookup_kernel_pte(page_addr);
	if (!ptep)
		return -ENOENT;

	pte = READ_ONCE(*ptep);
#ifdef PTE_MAYBE_GP
	pte = __pte(pte_val(pte) | PTE_MAYBE_GP);
#endif
#ifdef PTE_PXN
	pte = __pte(pte_val(pte) & ~PTE_PXN);
#endif
	set_pte(ptep, pte);
	flush_tlb_kernel_range(page_addr, page_addr + PAGE_SIZE);
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
	lkmdbg_alias_page = vmalloc(PAGE_SIZE);
	if (!lkmdbg_alias_page) {
		pr_warn("lkmdbg: alias patch page allocation failed\n");
		return 0;
	}

	if (!lkmdbg_symbols.init_mm) {
		pr_warn("lkmdbg: init_mm lookup unavailable, alias patch backend disabled\n");
		vfree(lkmdbg_alias_page);
		lkmdbg_alias_page = NULL;
		return 0;
	}

	lkmdbg_alias_ptep = lkmdbg_lookup_kernel_pte((unsigned long)lkmdbg_alias_page);
	if (!lkmdbg_alias_ptep) {
		pr_warn("lkmdbg: alias patch pte lookup failed alias=%px\n",
			lkmdbg_alias_page);
		vfree(lkmdbg_alias_page);
		lkmdbg_alias_page = NULL;
		return 0;
	}

	lkmdbg_alias_pte = READ_ONCE(*lkmdbg_alias_ptep);
	pr_info("lkmdbg: alias patch backend ready alias=%px pte=%px\n",
		lkmdbg_alias_page, lkmdbg_alias_ptep);
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

	if (lkmdbg_alias_page) {
		lkmdbg_alias_unmap_page();
		vfree(lkmdbg_alias_page);
		lkmdbg_alias_page = NULL;
		lkmdbg_alias_ptep = NULL;
	}
}

int lkmdbg_hooks_prepare_exec_pool(void)
{
	return 0;
}

int lkmdbg_hook_alloc_exec(struct lkmdbg_inline_hook *hook)
{
	int ret;

	if (!hook)
		return -EINVAL;

	if (hook->exec_buf)
		return 0;

	hook->exec_buf = vmalloc(PAGE_SIZE);
	if (!hook->exec_buf)
		return -ENOMEM;

	memset(hook->exec_buf, 0, PAGE_SIZE);
	hook->exec_size = PAGE_SIZE;

	ret = lkmdbg_make_page_executable(hook->exec_buf);
	if (ret) {
		pr_err("lkmdbg: exec buffer not executable addr=%px ret=%d\n",
		       hook->exec_buf, ret);
		mutex_lock(&lkmdbg_state.lock);
		lkmdbg_state.inline_hook_last_ret = ret;
		mutex_unlock(&lkmdbg_state.lock);
		vfree(hook->exec_buf);
		hook->exec_buf = NULL;
		hook->exec_size = 0;
		return ret;
	}

	pr_info("lkmdbg: exec buffer ready target=%px exec=%px\n",
		hook->target, hook->exec_buf);
	return 0;
}

int lkmdbg_hook_create(void *target, void *replacement,
		       struct lkmdbg_inline_hook **hook_out,
		       void **orig_out)
{
	struct lkmdbg_inline_hook *hook;
	struct lkmdbg_inline_hook *iter;

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

	mutex_lock(&lkmdbg_hook_lock);
	list_for_each_entry(iter, &lkmdbg_hook_list, node) {
		if (iter->origin == hook->origin) {
			mutex_unlock(&lkmdbg_hook_lock);
			mutex_lock(&lkmdbg_state.lock);
			lkmdbg_state.inline_hook_last_ret = -EEXIST;
			mutex_unlock(&lkmdbg_state.lock);
			kfree(hook);
			return -EEXIST;
		}
	}
	mutex_unlock(&lkmdbg_hook_lock);

	mutex_lock(&lkmdbg_hook_lock);
	list_add_tail(&hook->node, &lkmdbg_hook_list);
	mutex_unlock(&lkmdbg_hook_lock);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.inline_hook_create_total++;
	lkmdbg_state.inline_hook_last_ret = 0;
	lkmdbg_state.inline_hook_last_target = (u64)hook->target;
	lkmdbg_state.inline_hook_last_origin = (u64)hook->origin;
	lkmdbg_state.inline_hook_last_replacement = (u64)hook->replacement;
	lkmdbg_state.inline_hook_last_trampoline = 0;
	mutex_unlock(&lkmdbg_state.lock);

	*hook_out = hook;
	*orig_out = NULL;
	return 0;
}

int lkmdbg_hook_prepare_exec(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	kp_hook_err_t ret;

	if (!hook)
		return -EINVAL;

	if (hook->exec_ready) {
		if (orig_out)
			*orig_out = hook->exec_buf;
		return 0;
	}

	if (!hook->exec_buf) {
		ret = lkmdbg_hook_alloc_exec(hook);
		if (ret)
			return ret;
	}

	memset(hook->exec_buf, 0, hook->exec_size);
	memset(hook->core.origin_insts, 0, sizeof(hook->core.origin_insts));
	memset(hook->core.tramp_insts, 0, sizeof(hook->core.tramp_insts));
	memset(hook->core.relo_insts, 0, sizeof(hook->core.relo_insts));
	hook->core.relo_addr = (u64)hook->exec_buf;
	hook->core.tramp_insts_num = 0;
	hook->core.relo_insts_num = 0;

	ret = kp_hook_prepare(&hook->core);
	if (ret) {
		pr_err("lkmdbg: kp hook prepare failed target=%px origin=%px ret=%d\n",
		       hook->target, hook->origin, (int)ret);
		mutex_lock(&lkmdbg_state.lock);
		lkmdbg_state.inline_hook_last_ret = -EINVAL;
		mutex_unlock(&lkmdbg_state.lock);
		return -EINVAL;
	}

	if (hook->core.relo_insts_num * sizeof(u32) > hook->exec_size) {
		mutex_lock(&lkmdbg_state.lock);
		lkmdbg_state.inline_hook_last_ret = -E2BIG;
		mutex_unlock(&lkmdbg_state.lock);
		return -E2BIG;
	}

	memcpy(hook->exec_buf, hook->core.relo_insts,
	       hook->core.relo_insts_num * sizeof(u32));
	lkmdbg_symbols.flush_icache_range((unsigned long)hook->exec_buf,
					  (unsigned long)hook->exec_buf +
					  hook->core.relo_insts_num * sizeof(u32));
	hook->exec_ready = true;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.inline_hook_last_ret = 0;
	lkmdbg_state.inline_hook_last_trampoline = (u64)hook->exec_buf;
	mutex_unlock(&lkmdbg_state.lock);

	if (orig_out)
		*orig_out = hook->exec_buf;
	return 0;
}

int lkmdbg_hook_patch_target(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	int ret;

	if (!hook)
		return -EINVAL;

	if (hook->active)
		return -EALREADY;

	if (!hook->exec_ready) {
		ret = lkmdbg_hook_prepare_exec(hook, orig_out);
		if (ret)
			return ret;
	}

	pr_info("lkmdbg: kp hook patch backend=%s target=%px origin=%px\n",
		lkmdbg_alias_page ? "alias-vmalloc" : "unavailable",
		hook->target, hook->origin);

	ret = lkmdbg_patch_target_words((void *)hook->core.origin_addr,
					hook->core.tramp_insts,
					hook->core.tramp_insts_num);
	if (ret) {
		pr_err("lkmdbg: kp hook install failed target=%px origin=%px ret=%d\n",
		       hook->target, hook->origin, ret);
		mutex_lock(&lkmdbg_state.lock);
		lkmdbg_state.inline_hook_last_ret = ret;
		mutex_unlock(&lkmdbg_state.lock);
		return ret;
	}

	mutex_lock(&lkmdbg_hook_lock);
	hook->active = true;
	mutex_unlock(&lkmdbg_hook_lock);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.inline_hook_install_total++;
	lkmdbg_state.inline_hook_active++;
	lkmdbg_state.inline_hook_last_ret = 0;
	lkmdbg_state.inline_hook_last_target = (u64)hook->target;
	lkmdbg_state.inline_hook_last_origin = (u64)hook->origin;
	lkmdbg_state.inline_hook_last_replacement = (u64)hook->replacement;
	lkmdbg_state.inline_hook_last_trampoline = (u64)hook->exec_buf;
	mutex_unlock(&lkmdbg_state.lock);

	if (orig_out)
		*orig_out = hook->exec_buf;

	pr_info("lkmdbg: kp hook installed target=%px origin=%px replacement=%px trampoline=%px\n",
		hook->target, hook->origin, hook->replacement,
		hook->exec_buf);
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

		mutex_lock(&lkmdbg_state.lock);
		if (!ret && lkmdbg_state.inline_hook_active)
			lkmdbg_state.inline_hook_active--;
		lkmdbg_state.inline_hook_last_ret = ret;
		mutex_unlock(&lkmdbg_state.lock);
	}

	mutex_lock(&lkmdbg_hook_lock);
	if (!list_empty(&hook->node))
		list_del_init(&hook->node);
	mutex_unlock(&lkmdbg_hook_lock);

	if (hook->exec_buf)
		vfree(hook->exec_buf);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.inline_hook_remove_total++;
	lkmdbg_state.inline_hook_last_target = (u64)hook->target;
	lkmdbg_state.inline_hook_last_origin = (u64)hook->origin;
	lkmdbg_state.inline_hook_last_replacement = (u64)hook->replacement;
	lkmdbg_state.inline_hook_last_trampoline = (u64)hook->exec_buf;
	mutex_unlock(&lkmdbg_state.lock);

	kfree(hook);
}
#else
#include "lkmdbg_internal.h"

struct lkmdbg_inline_hook {
	int unsupported;
};

int lkmdbg_hooks_init(void)
{
	return 0;
}

void lkmdbg_hooks_exit(void)
{
}

int lkmdbg_hooks_prepare_exec_pool(void)
{
	return 0;
}

int lkmdbg_hook_alloc_exec(struct lkmdbg_inline_hook *hook)
{
	return hook ? -EOPNOTSUPP : -EINVAL;
}

int lkmdbg_hook_create(void *target, void *replacement,
		       struct lkmdbg_inline_hook **hook_out,
		       void **orig_out)
{
	if (!target || !replacement || !hook_out || !orig_out)
		return -EINVAL;

	*hook_out = NULL;
	*orig_out = NULL;
	return -EOPNOTSUPP;
}

int lkmdbg_hook_prepare_exec(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	if (!hook)
		return -EINVAL;

	if (orig_out)
		*orig_out = NULL;
	return -EOPNOTSUPP;
}

int lkmdbg_hook_patch_target(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	if (!hook)
		return -EINVAL;

	if (orig_out)
		*orig_out = NULL;
	return -EOPNOTSUPP;
}

int lkmdbg_hook_activate(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	return lkmdbg_hook_patch_target(hook, orig_out);
}

int lkmdbg_hook_install(void *target, void *replacement,
			struct lkmdbg_inline_hook **hook_out,
			void **orig_out)
{
	if (!target || !replacement || !hook_out || !orig_out)
		return -EINVAL;

	*hook_out = NULL;
	*orig_out = NULL;
	return -EOPNOTSUPP;
}

void lkmdbg_hook_remove(struct lkmdbg_inline_hook *hook)
{
	(void)hook;
}
#endif
