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

	addr = lkmdbg_symbols.kallsyms_lookup_name("access_process_vm");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.access_process_vm =
		(int (*)(struct task_struct *tsk, unsigned long addr, void *buf,
			 int len, unsigned int gup_flags))addr;

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
	lkmdbg_symbols.access_process_vm = NULL;
}
