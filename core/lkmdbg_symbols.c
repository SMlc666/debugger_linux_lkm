#include <linux/kernel.h>
#include <linux/kprobes.h>

#include "lkmdbg_internal.h"

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
}
