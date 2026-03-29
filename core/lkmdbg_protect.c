#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/module.h>

#include "lkmdbg_internal.h"

#define AARCH64_RET_INSN 0xD65F03C0u
#define AARCH64_MOV_X0_1 0xD2800020u

static int write_insn(void *target, u32 insn)
{
	if (!lkmdbg_symbols.aarch64_insn_write || !lkmdbg_symbols.flush_icache_range)
		return -ENOENT;

	if (lkmdbg_symbols.aarch64_insn_write(target, insn))
		return -EIO;

	lkmdbg_flush_icache_runtime((unsigned long)target,
				    (unsigned long)target + sizeof(u32));
	return 0;
}

int lkmdbg_disable_kprobe_blacklist(void)
{
	struct list_head *kprobe_blacklist;
	struct kprobe_blacklist_entry *ent;
	int patched = 0;

	if (!lkmdbg_symbols.kallsyms_lookup_name)
		return -ENOENT;

	kprobe_blacklist = (struct list_head *)
		lkmdbg_symbols.kallsyms_lookup_name("kprobe_blacklist");
	if (!kprobe_blacklist)
		return -ENOENT;

	list_for_each_entry(ent, kprobe_blacklist, list) {
		if (!ent)
			continue;
		if (!ent->start_addr && !ent->end_addr)
			continue;
		ent->start_addr = 0;
		ent->end_addr = 0;
		patched++;
	}

	return patched;
}

int lkmdbg_cfi_bypass(void)
{
	static const char *targets[] = {
		"__cfi_slowpath",
		"__cfi_slowpath_diag",
		"_cfi_slowpath",
		"__cfi_check_fail",
		"__ubsan_handle_cfi_check_fail_abort",
		"__ubsan_handle_cfi_check_fail",
	};
	int patched = 0;
	int i;

	if (!lkmdbg_symbols.kallsyms_lookup_name)
		return -ENOENT;

	for (i = 0; i < ARRAY_SIZE(targets); i++) {
		unsigned long addr = lkmdbg_symbols.kallsyms_lookup_name(targets[i]);
		if (!addr)
			continue;
		if (write_insn((void *)addr, AARCH64_RET_INSN) == 0)
			patched++;
	}

	{
		unsigned long addr = lkmdbg_symbols.kallsyms_lookup_name("report_cfi_failure");
		if (addr) {
			u32 seq[] = {AARCH64_MOV_X0_1, AARCH64_RET_INSN};
			if (!write_insn((void *)addr, seq[0]) &&
			    !write_insn((void *)(addr + sizeof(u32)), seq[1]))
				patched++;
		}
	}

	return patched ? patched : -ENOENT;
}
