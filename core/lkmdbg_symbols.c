#include <linux/kernel.h>
#include <linux/kprobes.h>

#include "lkmdbg_internal.h"

static unsigned long lkmdbg_lookup_runtime_symbol(const char *name)
{
	struct kprobe kp = {
		.symbol_name = name,
	};
	unsigned long addr = 0;

	if (lkmdbg_symbols.kallsyms_lookup_name)
		addr = lkmdbg_symbols.kallsyms_lookup_name(name);
	if (addr)
		return addr;

	if (register_kprobe(&kp))
		return 0;

	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

static int lkmdbg_resolve_runtime_symbols(void)
{
	static struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	unsigned long addr;

	if (lkmdbg_symbols.kallsyms_lookup_name)
		return 0;

	if (register_kprobe(&kp))
		return -ENOENT;

	lkmdbg_symbols.kallsyms_lookup_name =
		(unsigned long (*)(const char *name))kp.addr;
	unregister_kprobe(&kp);

	if (!lkmdbg_symbols.kallsyms_lookup_name)
		return -ENOENT;

	addr = lkmdbg_symbols.kallsyms_lookup_name("filp_open");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.filp_open =
		(struct file *(*)(const char *filename, int flags, umode_t mode))addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("filp_close");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.filp_close =
		(int (*)(struct file *file, fl_owner_t id))addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("aarch64_insn_write");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.aarch64_insn_write = (int (*)(void *, u32))addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("aarch64_insn_patch_text_nosync");
	if (addr)
		lkmdbg_symbols.aarch64_insn_patch_text_nosync =
			(int (*)(void *, u32))addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("caches_clean_inval_pou");
	if (!addr)
		addr = lkmdbg_symbols.kallsyms_lookup_name("__flush_icache_range");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.flush_icache_range =
		(void (*)(unsigned long, unsigned long))addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("set_memory_x");
	if (addr)
		lkmdbg_symbols.set_memory_x =
			(int (*)(unsigned long, int))addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("module_alloc");
	if (addr)
		lkmdbg_symbols.module_alloc =
			(void *(*)(unsigned long size))addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("module_memfree");
	if (addr)
		lkmdbg_symbols.module_memfree = (void (*)(void *region))addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("init_mm");
	if (addr)
		lkmdbg_symbols.init_mm = (struct mm_struct *)addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("task_work_add");
	if (addr)
		lkmdbg_symbols.task_work_add_sym = addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("task_work_cancel_match");
	if (addr)
		lkmdbg_symbols.task_work_cancel_match_sym = addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("task_work_cancel_func");
	if (addr)
		lkmdbg_symbols.task_work_cancel_func_sym = addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("task_work_cancel");
	if (addr)
		lkmdbg_symbols.task_work_cancel_sym = addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("register_user_step_hook");
	if (addr)
		lkmdbg_symbols.register_user_step_hook_sym = addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("unregister_user_step_hook");
	if (addr)
		lkmdbg_symbols.unregister_user_step_hook_sym = addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("user_enable_single_step");
	if (addr)
		lkmdbg_symbols.user_enable_single_step_sym = addr;

	addr = lkmdbg_symbols.kallsyms_lookup_name("user_disable_single_step");
	if (addr)
		lkmdbg_symbols.user_disable_single_step_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("for_each_kernel_tracepoint");
	if (addr)
		lkmdbg_symbols.for_each_kernel_tracepoint_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("tracepoint_probe_register");
	if (addr)
		lkmdbg_symbols.tracepoint_probe_register_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("tracepoint_probe_unregister");
	if (addr)
		lkmdbg_symbols.tracepoint_probe_unregister_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("perf_event_disable_local");
	if (addr)
		lkmdbg_symbols.perf_event_disable_local_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("do_page_fault");
	if (addr)
		lkmdbg_symbols.do_page_fault_sym = addr;

	return 0;
}

int lkmdbg_symbols_init(void)
{
	return lkmdbg_resolve_runtime_symbols();
}

void lkmdbg_symbols_exit(void)
{
	lkmdbg_symbols.kallsyms_lookup_name = NULL;
	lkmdbg_symbols.filp_open = NULL;
	lkmdbg_symbols.filp_close = NULL;
	lkmdbg_symbols.aarch64_insn_patch_text_nosync = NULL;
	lkmdbg_symbols.aarch64_insn_write = NULL;
	lkmdbg_symbols.flush_icache_range = NULL;
	lkmdbg_symbols.set_memory_x = NULL;
	lkmdbg_symbols.module_alloc = NULL;
	lkmdbg_symbols.module_memfree = NULL;
	lkmdbg_symbols.init_mm = NULL;
	lkmdbg_symbols.task_work_add_sym = 0;
	lkmdbg_symbols.task_work_cancel_match_sym = 0;
	lkmdbg_symbols.task_work_cancel_func_sym = 0;
	lkmdbg_symbols.task_work_cancel_sym = 0;
	lkmdbg_symbols.register_user_step_hook_sym = 0;
	lkmdbg_symbols.unregister_user_step_hook_sym = 0;
	lkmdbg_symbols.user_enable_single_step_sym = 0;
	lkmdbg_symbols.user_disable_single_step_sym = 0;
	lkmdbg_symbols.for_each_kernel_tracepoint_sym = 0;
	lkmdbg_symbols.tracepoint_probe_register_sym = 0;
	lkmdbg_symbols.tracepoint_probe_unregister_sym = 0;
	lkmdbg_symbols.perf_event_disable_local_sym = 0;
	lkmdbg_symbols.do_page_fault_sym = 0;
}
