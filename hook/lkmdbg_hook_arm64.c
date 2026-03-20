#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/cpu.h>
#include <linux/vmalloc.h>

#include "lkmdbg_internal.h"

#define LKMDBG_HOOK_TRAMP_INSN_COUNT 4
#define LKMDBG_HOOK_RELO_MAX_INSN    64

#define LKMDBG_ARM64_NOP    0xD503201Fu
#define LKMDBG_ARM64_BTI_JC 0xD50324DFu

struct lkmdbg_inline_hook {
	struct list_head node;
	void *target;
	void *origin;
	void *replacement;
	void *trampoline;
	bool trampoline_is_exec;
	bool active;
	u32 original_insns[LKMDBG_HOOK_TRAMP_INSN_COUNT];
	u32 patch_insns[LKMDBG_HOOK_TRAMP_INSN_COUNT];
	u32 trampoline_insns[LKMDBG_HOOK_RELO_MAX_INSN];
	u32 patch_words;
	u32 trampoline_words;
};

static LIST_HEAD(lkmdbg_hook_list);
static DEFINE_MUTEX(lkmdbg_hook_lock);

enum lkmdbg_a64_inst_type {
	LKMDBG_INST_B = 0x14000000,
	LKMDBG_INST_BC = 0x54000000,
	LKMDBG_INST_BL = 0x94000000,
	LKMDBG_INST_ADR = 0x10000000,
	LKMDBG_INST_ADRP = 0x90000000,
	LKMDBG_INST_LDR_32 = 0x18000000,
	LKMDBG_INST_LDR_64 = 0x58000000,
	LKMDBG_INST_LDRSW = 0x98000000,
	LKMDBG_INST_PRFM = 0xD8000000,
	LKMDBG_INST_LDR_SIMD_32 = 0x1C000000,
	LKMDBG_INST_LDR_SIMD_64 = 0x5C000000,
	LKMDBG_INST_LDR_SIMD_128 = 0x9C000000,
	LKMDBG_INST_CBZ = 0x34000000,
	LKMDBG_INST_CBNZ = 0x35000000,
	LKMDBG_INST_TBZ = 0x36000000,
	LKMDBG_INST_TBNZ = 0x37000000,
	LKMDBG_INST_IGNORE = 0x00000000,
};

static const u32 lkmdbg_a64_inst_masks[] = {
	0xFC000000, 0xFF000010, 0xFC000000, 0x9F000000,
	0x9F000000, 0xFF000000, 0xFF000000, 0xFF000000,
	0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
	0x7F000000, 0x7F000000, 0x7F000000, 0x7F000000,
	0x00000000,
};

static const u32 lkmdbg_a64_inst_types[] = {
	LKMDBG_INST_B, LKMDBG_INST_BC, LKMDBG_INST_BL, LKMDBG_INST_ADR,
	LKMDBG_INST_ADRP, LKMDBG_INST_LDR_32, LKMDBG_INST_LDR_64,
	LKMDBG_INST_LDRSW, LKMDBG_INST_PRFM, LKMDBG_INST_LDR_SIMD_32,
	LKMDBG_INST_LDR_SIMD_64, LKMDBG_INST_LDR_SIMD_128, LKMDBG_INST_CBZ,
	LKMDBG_INST_CBNZ, LKMDBG_INST_TBZ, LKMDBG_INST_TBNZ,
	LKMDBG_INST_IGNORE,
};

static const u8 lkmdbg_a64_relo_words[] = {
	6, 8, 8, 4, 4, 6, 6, 6, 8, 8, 8, 8, 6, 6, 6, 6, 2,
};

struct lkmdbg_patch_ctx {
	struct lkmdbg_inline_hook *hook;
	const u32 *insns;
	u32 words;
};

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

static inline u32 lkmdbg_bits32(u32 n, u32 high, u32 low)
{
	return (u32)(n << (31u - high)) >> (31u - high + low);
}

static inline u64 lkmdbg_sign64_extend(u64 n, u32 len)
{
	if (((n >> (len - 1)) & 1u) == 0)
		return n;
	return n | (~0ULL << len);
}

static u64 lkmdbg_hook_follow_branch_once(u64 addr)
{
	u32 inst = *(u32 *)addr;

	if ((inst & 0xFC000000u) == LKMDBG_INST_B) {
		u64 imm26 = lkmdbg_bits32(inst, 25, 0);
		u64 off = lkmdbg_sign64_extend(imm26 << 2u, 28u);

		return addr + off;
	}

	if (inst == 0xD503245Fu || inst == 0xD503249Fu ||
	    inst == LKMDBG_ARM64_BTI_JC)
		return addr + sizeof(u32);

	return addr;
}

static u64 lkmdbg_hook_follow_branch(u64 addr)
{
	u64 next;

	for (;;) {
		next = lkmdbg_hook_follow_branch_once(addr);
		if (next == addr)
			return addr;
		addr = next;
	}
}

static u32 lkmdbg_emit_ret_absolute(u32 *buf, u64 addr)
{
	buf[0] = 0x58000051u;
	buf[1] = 0xD65F0220u;
	buf[2] = lower_32_bits(addr);
	buf[3] = upper_32_bits(addr);
	return 4;
}

static bool lkmdbg_addr_is_in_patch_window(struct lkmdbg_inline_hook *hook,
					   u64 addr)
{
	u64 start = (u64)hook->origin;
	u64 end = start + hook->patch_words * sizeof(u32);

	return addr >= start && addr < end;
}

static u64 lkmdbg_relo_fixup_addr(struct lkmdbg_inline_hook *hook, u64 addr)
{
	u64 start = (u64)hook->origin;
	u32 inst_index;
	u32 i;
	u64 fix_addr;

	if (!lkmdbg_addr_is_in_patch_window(hook, addr))
		return addr;

	inst_index = (addr - start) / sizeof(u32);
	fix_addr = (u64)hook->trampoline;

	for (i = 0; i < inst_index; i++) {
		u32 inst = hook->original_insns[i];
		u32 j;

		for (j = 0; j < ARRAY_SIZE(lkmdbg_a64_relo_words); j++) {
			if ((inst & lkmdbg_a64_inst_masks[j]) ==
			    lkmdbg_a64_inst_types[j]) {
				fix_addr += lkmdbg_a64_relo_words[j] *
					    sizeof(u32);
				break;
			}
		}
	}

	return fix_addr;
}

static int lkmdbg_relo_emit_b(struct lkmdbg_inline_hook *hook, u64 inst_addr,
			      u32 inst, u32 type)
{
	u32 *buf = hook->trampoline_insns + hook->trampoline_words;
	u64 target;

	if (type == LKMDBG_INST_BC) {
		u64 imm19 = lkmdbg_bits32(inst, 23, 5);

		target = inst_addr + lkmdbg_sign64_extend(imm19 << 2u, 21u);
		buf[0] = (inst & 0xFF00001Fu) | 0x40u;
		buf[1] = 0x14000006u;
		buf[2] = 0x58000051u;
		buf[3] = 0x14000003u;
	} else {
		u64 imm26 = lkmdbg_bits32(inst, 25, 0);

		target = inst_addr + lkmdbg_sign64_extend(imm26 << 2u, 28u);
		buf[0] = 0x58000051u;
		buf[1] = 0x14000003u;
	}

	target = lkmdbg_relo_fixup_addr(hook, target);

	if (type == LKMDBG_INST_BC) {
		buf[4] = lower_32_bits(target);
		buf[5] = upper_32_bits(target);
		buf[6] = 0xD65F0220u;
		buf[7] = LKMDBG_ARM64_NOP;
		return 0;
	}

	buf[2] = lower_32_bits(target);
	buf[3] = upper_32_bits(target);

	if (type == LKMDBG_INST_BL) {
		buf[4] = 0x1000001Eu;
		buf[5] = 0x910033DEu;
		buf[6] = 0xD65F0220u;
		buf[7] = LKMDBG_ARM64_NOP;
	}

	return 0;
}

static int lkmdbg_relo_emit_adr(struct lkmdbg_inline_hook *hook, u64 inst_addr,
				u32 inst, u32 type)
{
	u32 *buf = hook->trampoline_insns + hook->trampoline_words;
	u32 xd = lkmdbg_bits32(inst, 4, 0);
	u64 immlo = lkmdbg_bits32(inst, 30, 29);
	u64 immhi = lkmdbg_bits32(inst, 23, 5);
	u64 target;

	if (type == LKMDBG_INST_ADR) {
		target = inst_addr +
			 lkmdbg_sign64_extend((immhi << 2u) | immlo, 21u);
	} else {
		target = (inst_addr +
			  lkmdbg_sign64_extend((immhi << 14u) |
					       (immlo << 12u), 33u)) &
			 0xFFFFFFFFFFFFF000ULL;
		if (lkmdbg_addr_is_in_patch_window(hook, target))
			return -EINVAL;
	}

	buf[0] = 0x58000040u | xd;
	buf[1] = 0x14000003u;
	buf[2] = lower_32_bits(target);
	buf[3] = upper_32_bits(target);
	return 0;
}

static int lkmdbg_relo_emit_ldr(struct lkmdbg_inline_hook *hook, u64 inst_addr,
				u32 inst, u32 type)
{
	u32 *buf = hook->trampoline_insns + hook->trampoline_words;
	u32 rt = lkmdbg_bits32(inst, 4, 0);
	u64 imm19 = lkmdbg_bits32(inst, 23, 5);
	u64 target = inst_addr + lkmdbg_sign64_extend(imm19 << 2u, 21u);

	if (lkmdbg_addr_is_in_patch_window(hook, target) &&
	    type != LKMDBG_INST_PRFM)
		return -EINVAL;

	target = lkmdbg_relo_fixup_addr(hook, target);

	if (type == LKMDBG_INST_LDR_32 || type == LKMDBG_INST_LDR_64 ||
	    type == LKMDBG_INST_LDRSW) {
		buf[0] = 0x58000060u | rt;
		if (type == LKMDBG_INST_LDR_32)
			buf[1] = 0xB9400000u | rt | (rt << 5u);
		else if (type == LKMDBG_INST_LDR_64)
			buf[1] = 0xF9400000u | rt | (rt << 5u);
		else
			buf[1] = 0xB9800000u | rt | (rt << 5u);
		buf[2] = 0x14000004u;
		buf[3] = LKMDBG_ARM64_NOP;
		buf[4] = lower_32_bits(target);
		buf[5] = upper_32_bits(target);
		return 0;
	}

	buf[0] = 0xA93F47F0u;
	buf[1] = 0x58000091u;
	if (type == LKMDBG_INST_PRFM)
		buf[2] = 0xF9800220u | rt;
	else if (type == LKMDBG_INST_LDR_SIMD_32)
		buf[2] = 0xBD400220u | rt;
	else if (type == LKMDBG_INST_LDR_SIMD_64)
		buf[2] = 0xFD400220u | rt;
	else
		buf[2] = 0x3DC00220u | rt;
	buf[3] = 0xF85F83F1u;
	buf[4] = 0x14000004u;
	buf[5] = LKMDBG_ARM64_NOP;
	buf[6] = lower_32_bits(target);
	buf[7] = upper_32_bits(target);
	return 0;
}

static int lkmdbg_relo_emit_cb(struct lkmdbg_inline_hook *hook, u64 inst_addr,
			       u32 inst)
{
	u32 *buf = hook->trampoline_insns + hook->trampoline_words;
	u64 imm19 = lkmdbg_bits32(inst, 23, 5);
	u64 target = inst_addr + lkmdbg_sign64_extend(imm19 << 2u, 21u);

	target = lkmdbg_relo_fixup_addr(hook, target);

	buf[0] = (inst & 0xFF00001Fu) | 0x40u;
	buf[1] = 0x14000005u;
	buf[2] = 0x58000051u;
	buf[3] = 0xD65F0220u;
	buf[4] = lower_32_bits(target);
	buf[5] = upper_32_bits(target);
	return 0;
}

static int lkmdbg_relo_emit_tb(struct lkmdbg_inline_hook *hook, u64 inst_addr,
			       u32 inst)
{
	u32 *buf = hook->trampoline_insns + hook->trampoline_words;
	u64 imm14 = lkmdbg_bits32(inst, 18, 5);
	u64 target = inst_addr + lkmdbg_sign64_extend(imm14 << 2u, 16u);

	target = lkmdbg_relo_fixup_addr(hook, target);

	buf[0] = (inst & 0xFFF8001Fu) | 0x40u;
	buf[1] = 0x14000005u;
	buf[2] = 0x58000051u;
	buf[3] = 0xD65F0220u;
	buf[4] = lower_32_bits(target);
	buf[5] = upper_32_bits(target);
	return 0;
}

static int lkmdbg_relo_emit_plain(struct lkmdbg_inline_hook *hook, u32 inst)
{
	u32 *buf = hook->trampoline_insns + hook->trampoline_words;

	buf[0] = inst;
	buf[1] = LKMDBG_ARM64_NOP;
	return 0;
}

static int lkmdbg_relocate_one(struct lkmdbg_inline_hook *hook, u64 inst_addr,
			       u32 inst)
{
	u32 j;
	u32 type = LKMDBG_INST_IGNORE;
	u32 words = 2;
	int ret;

	for (j = 0; j < ARRAY_SIZE(lkmdbg_a64_relo_words); j++) {
		if ((inst & lkmdbg_a64_inst_masks[j]) ==
		    lkmdbg_a64_inst_types[j]) {
			type = lkmdbg_a64_inst_types[j];
			words = lkmdbg_a64_relo_words[j];
			break;
		}
	}

	if (hook->trampoline_words + words >= LKMDBG_HOOK_RELO_MAX_INSN)
		return -E2BIG;

	switch (type) {
	case LKMDBG_INST_B:
	case LKMDBG_INST_BC:
	case LKMDBG_INST_BL:
		ret = lkmdbg_relo_emit_b(hook, inst_addr, inst, type);
		break;
	case LKMDBG_INST_ADR:
	case LKMDBG_INST_ADRP:
		ret = lkmdbg_relo_emit_adr(hook, inst_addr, inst, type);
		break;
	case LKMDBG_INST_LDR_32:
	case LKMDBG_INST_LDR_64:
	case LKMDBG_INST_LDRSW:
	case LKMDBG_INST_PRFM:
	case LKMDBG_INST_LDR_SIMD_32:
	case LKMDBG_INST_LDR_SIMD_64:
	case LKMDBG_INST_LDR_SIMD_128:
		ret = lkmdbg_relo_emit_ldr(hook, inst_addr, inst, type);
		break;
	case LKMDBG_INST_CBZ:
	case LKMDBG_INST_CBNZ:
		ret = lkmdbg_relo_emit_cb(hook, inst_addr, inst);
		break;
	case LKMDBG_INST_TBZ:
	case LKMDBG_INST_TBNZ:
		ret = lkmdbg_relo_emit_tb(hook, inst_addr, inst);
		break;
	case LKMDBG_INST_IGNORE:
	default:
		ret = lkmdbg_relo_emit_plain(hook, inst);
		break;
	}

	if (ret)
		return ret;

	hook->trampoline_words += words;
	return 0;
}

static int lkmdbg_prepare_hook(struct lkmdbg_inline_hook *hook)
{
	u32 i;

	hook->patch_words = lkmdbg_emit_ret_absolute(hook->patch_insns,
						      (u64)hook->replacement);
	hook->trampoline_words = 0;
	hook->trampoline_insns[hook->trampoline_words++] = LKMDBG_ARM64_BTI_JC;
	hook->trampoline_insns[hook->trampoline_words++] = LKMDBG_ARM64_NOP;

	for (i = 0; i < LKMDBG_HOOK_TRAMP_INSN_COUNT; i++) {
		u64 inst_addr = (u64)hook->origin + i * sizeof(u32);
		int ret;

		hook->original_insns[i] = *(u32 *)inst_addr;
		ret = lkmdbg_relocate_one(hook, inst_addr,
					  hook->original_insns[i]);
		if (ret)
			return ret;
	}

	if (hook->trampoline_words + 4 >= LKMDBG_HOOK_RELO_MAX_INSN)
		return -E2BIG;

	hook->trampoline_words += lkmdbg_emit_ret_absolute(
		hook->trampoline_insns + hook->trampoline_words,
		(u64)hook->origin + hook->patch_words * sizeof(u32));

	memcpy(hook->trampoline, hook->trampoline_insns,
	       hook->trampoline_words * sizeof(u32));
	return 0;
}

static void lkmdbg_free_trampoline(struct lkmdbg_inline_hook *hook)
{
	if (!hook->trampoline)
		return;

	if (hook->trampoline_is_exec && lkmdbg_symbols.module_memfree)
		lkmdbg_symbols.module_memfree(hook->trampoline);
	else
		vfree(hook->trampoline);

	hook->trampoline = NULL;
	hook->trampoline_is_exec = false;
}

static int lkmdbg_patch_stop_machine(void *data)
{
	struct lkmdbg_patch_ctx *ctx = data;

	return lkmdbg_write_text_words(ctx->hook->origin, ctx->insns, ctx->words);
}

int lkmdbg_hooks_init(void)
{
	return 0;
}

int lkmdbg_hook_create(void *target, void *replacement,
		       struct lkmdbg_inline_hook **hook_out,
		       void **orig_out)
{
	struct lkmdbg_inline_hook *hook;
	struct lkmdbg_inline_hook *iter;
	int ret;

	if (!target || !replacement || !hook_out || !orig_out)
		return -EINVAL;

	hook = kzalloc(sizeof(*hook), GFP_KERNEL);
	if (!hook)
		return -ENOMEM;

	INIT_LIST_HEAD(&hook->node);
	hook->target = target;
	hook->origin = (void *)lkmdbg_hook_follow_branch((u64)target);
	hook->replacement = replacement;
	hook->trampoline = vmalloc(PAGE_SIZE);
	if (!hook->trampoline) {
		kfree(hook);
		return -ENOMEM;
	}

	mutex_lock(&lkmdbg_hook_lock);
	list_for_each_entry(iter, &lkmdbg_hook_list, node) {
		if (iter->origin == hook->origin) {
			mutex_unlock(&lkmdbg_hook_lock);
			lkmdbg_free_trampoline(hook);
			kfree(hook);
			return -EEXIST;
		}
	}
	mutex_unlock(&lkmdbg_hook_lock);

	ret = lkmdbg_prepare_hook(hook);
	if (ret) {
		pr_err("lkmdbg: inline hook prepare failed target=%px origin=%px ret=%d\n",
		       hook->target, hook->origin, ret);
		lkmdbg_free_trampoline(hook);
		kfree(hook);
		return ret;
	}

	*hook_out = hook;
	*orig_out = NULL;

	mutex_lock(&lkmdbg_hook_lock);
	list_add_tail(&hook->node, &lkmdbg_hook_list);
	mutex_unlock(&lkmdbg_hook_lock);

	return 0;
}

int lkmdbg_hook_prepare_exec(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	void *prepare_trampoline;
	void *exec_trampoline;
	bool can_use_exec_alloc;
	int ret;

	if (!hook)
		return -EINVAL;

	if (hook->active)
		return -EALREADY;

	if (hook->trampoline_is_exec) {
		if (orig_out)
			*orig_out = hook->trampoline;
		return 0;
	}

	if (!lkmdbg_symbols.flush_icache_range)
		return -ENOENT;

	pr_emerg("lkmdbg: hook_activate stage=exec_alloc_begin target=%px origin=%px\n",
		 hook->target, hook->origin);

	prepare_trampoline = hook->trampoline;
	exec_trampoline = NULL;
	can_use_exec_alloc = lkmdbg_symbols.module_alloc &&
			     lkmdbg_symbols.module_memfree;

	if (can_use_exec_alloc) {
		exec_trampoline = lkmdbg_symbols.module_alloc(PAGE_SIZE);
		if (!exec_trampoline) {
			pr_err("lkmdbg: module_alloc failed for trampoline target=%px origin=%px\n",
			       hook->target, hook->origin);
			return -ENOMEM;
		}
		pr_emerg("lkmdbg: hook_activate stage=exec_alloc_done target=%px origin=%px trampoline=%px\n",
			 hook->target, hook->origin, exec_trampoline);

		hook->trampoline = exec_trampoline;
		hook->trampoline_is_exec = true;
		pr_emerg("lkmdbg: hook_activate stage=exec_prepare_begin target=%px origin=%px trampoline=%px\n",
			 hook->target, hook->origin, hook->trampoline);
		ret = lkmdbg_prepare_hook(hook);
		if (ret) {
			pr_err("lkmdbg: exec trampoline prepare failed target=%px origin=%px ret=%d\n",
			       hook->target, hook->origin, ret);
			lkmdbg_free_trampoline(hook);
			hook->trampoline = prepare_trampoline;
			hook->trampoline_is_exec = false;
			return ret;
		}
		pr_emerg("lkmdbg: hook_activate stage=exec_prepare_done target=%px origin=%px trampoline=%px\n",
			 hook->target, hook->origin, hook->trampoline);

		vfree(prepare_trampoline);
	} else {
		pr_emerg("lkmdbg: hook_activate stage=no_exec_allocator target=%px origin=%px module_alloc=%u module_memfree=%u set_memory_x=%u\n",
			 hook->target, hook->origin,
			 !!lkmdbg_symbols.module_alloc,
			 !!lkmdbg_symbols.module_memfree,
			 !!lkmdbg_symbols.set_memory_x);
		return -ENOENT;
	}

	lkmdbg_symbols.flush_icache_range((unsigned long)hook->trampoline,
					  (unsigned long)hook->trampoline +
					  hook->trampoline_words * sizeof(u32));

	if (orig_out)
		*orig_out = hook->trampoline;

	return 0;
}

int lkmdbg_hook_patch_target(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	struct lkmdbg_patch_ctx ctx;
	int ret;

	if (!hook)
		return -EINVAL;

	if (hook->active)
		return -EALREADY;

	ret = lkmdbg_hook_prepare_exec(hook, orig_out);
	if (ret)
		return ret;

	ctx.hook = hook;
	ctx.insns = hook->patch_insns;
	ctx.words = hook->patch_words;
	pr_emerg("lkmdbg: hook_activate stage=patch_begin target=%px origin=%px\n",
		 hook->target, hook->origin);
	ret = stop_machine(lkmdbg_patch_stop_machine, &ctx, cpu_online_mask);
	if (ret) {
		pr_err("lkmdbg: inline hook patch failed target=%px origin=%px ret=%d\n",
		       hook->target, hook->origin, ret);
		return ret;
	}
	pr_emerg("lkmdbg: hook_activate stage=patch_done target=%px origin=%px\n",
		 hook->target, hook->origin);

	mutex_lock(&lkmdbg_hook_lock);
	hook->active = true;
	mutex_unlock(&lkmdbg_hook_lock);

	if (orig_out)
		*orig_out = hook->trampoline;

	pr_info("lkmdbg: inline hook installed target=%px origin=%px replacement=%px trampoline=%px\n",
		hook->target, hook->origin, hook->replacement, hook->trampoline);
	return 0;
}

int lkmdbg_hook_activate(struct lkmdbg_inline_hook *hook, void **orig_out)
{
	return lkmdbg_hook_patch_target(hook, orig_out);
}

void lkmdbg_hook_remove(struct lkmdbg_inline_hook *hook)
{
	struct lkmdbg_patch_ctx ctx;
	int ret;

	if (!hook)
		return;

	if (hook->active) {
		ctx.hook = hook;
		ctx.insns = hook->original_insns;
		ctx.words = hook->patch_words;
		ret = stop_machine(lkmdbg_patch_stop_machine, &ctx,
				   cpu_online_mask);
		if (ret)
			pr_warn("lkmdbg: inline hook rollback failed origin=%px ret=%d\n",
				hook->origin, ret);

		mutex_lock(&lkmdbg_hook_lock);
		hook->active = false;
		mutex_unlock(&lkmdbg_hook_lock);
	}

	mutex_lock(&lkmdbg_hook_lock);
	if (!list_empty(&hook->node))
		list_del_init(&hook->node);
	mutex_unlock(&lkmdbg_hook_lock);

	lkmdbg_free_trampoline(hook);
	kfree(hook);
}

void lkmdbg_hooks_exit(void)
{
	struct lkmdbg_inline_hook *hook;
	struct lkmdbg_inline_hook *tmp;
	struct lkmdbg_patch_ctx ctx;
	int ret;

	mutex_lock(&lkmdbg_hook_lock);
	list_for_each_entry_safe(hook, tmp, &lkmdbg_hook_list, node) {
		list_del_init(&hook->node);
		mutex_unlock(&lkmdbg_hook_lock);

		if (hook->active) {
			ctx.hook = hook;
			ctx.insns = hook->original_insns;
			ctx.words = hook->patch_words;
			ret = stop_machine(lkmdbg_patch_stop_machine, &ctx,
					   cpu_online_mask);
			if (ret)
				pr_warn("lkmdbg: hook exit rollback failed origin=%px ret=%d\n",
					hook->origin, ret);
		}

		lkmdbg_free_trampoline(hook);
		kfree(hook);
		mutex_lock(&lkmdbg_hook_lock);
	}
	mutex_unlock(&lkmdbg_hook_lock);
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
