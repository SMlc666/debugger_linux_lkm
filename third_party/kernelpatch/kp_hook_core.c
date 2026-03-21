/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from KernelPatch:
 * https://github.com/bmax121/KernelPatch
 * Original Copyright (C) 2023 bmax121.
 */

#include <linux/kernel.h>
#include <linux/types.h>

#include "kp_hook.h"

#define kp_bits32(n, high, low) \
	((u32)((n) << (31u - (high))) >> (31u - (high) + (low)))
#define kp_sign64_extend(n, len) \
	((((u64)((n) << (63u - ((len) - 1))) >> 63u) ? \
	  ((n) | (~0ULL << (len))) : (n)))

typedef u32 kp_inst_type_t;
typedef u32 kp_inst_mask_t;

#define KP_INST_B 0x14000000U
#define KP_INST_BC 0x54000000U
#define KP_INST_BL 0x94000000U
#define KP_INST_ADR 0x10000000U
#define KP_INST_ADRP 0x90000000U
#define KP_INST_LDR_32 0x18000000U
#define KP_INST_LDR_64 0x58000000U
#define KP_INST_LDRSW_LIT 0x98000000U
#define KP_INST_PRFM_LIT 0xD8000000U
#define KP_INST_LDR_SIMD_32 0x1C000000U
#define KP_INST_LDR_SIMD_64 0x5C000000U
#define KP_INST_LDR_SIMD_128 0x9C000000U
#define KP_INST_CBZ 0x34000000U
#define KP_INST_CBNZ 0x35000000U
#define KP_INST_TBZ 0x36000000U
#define KP_INST_TBNZ 0x37000000U
#define KP_INST_IGNORE 0x0U

#define KP_MASK_B 0xFC000000U
#define KP_MASK_BC 0xFF000010U
#define KP_MASK_BL 0xFC000000U
#define KP_MASK_ADR 0x9F000000U
#define KP_MASK_ADRP 0x9F000000U
#define KP_MASK_LDR_32 0xFF000000U
#define KP_MASK_LDR_64 0xFF000000U
#define KP_MASK_LDRSW_LIT 0xFF000000U
#define KP_MASK_PRFM_LIT 0xFF000000U
#define KP_MASK_LDR_SIMD_32 0xFF000000U
#define KP_MASK_LDR_SIMD_64 0xFF000000U
#define KP_MASK_LDR_SIMD_128 0xFF000000U
#define KP_MASK_CBZ 0x7F000000U
#define KP_MASK_CBNZ 0x7F000000U
#define KP_MASK_TBZ 0x7F000000U
#define KP_MASK_TBNZ 0x7F000000U
#define KP_MASK_IGNORE 0x0U

static kp_inst_mask_t kp_masks[] = {
	KP_MASK_B, KP_MASK_BC, KP_MASK_BL, KP_MASK_ADR, KP_MASK_ADRP,
	KP_MASK_LDR_32, KP_MASK_LDR_64, KP_MASK_LDRSW_LIT,
	KP_MASK_PRFM_LIT, KP_MASK_LDR_SIMD_32, KP_MASK_LDR_SIMD_64,
	KP_MASK_LDR_SIMD_128, KP_MASK_CBZ, KP_MASK_CBNZ,
	KP_MASK_TBZ, KP_MASK_TBNZ, KP_MASK_IGNORE,
};

static kp_inst_type_t kp_types[] = {
	KP_INST_B, KP_INST_BC, KP_INST_BL, KP_INST_ADR, KP_INST_ADRP,
	KP_INST_LDR_32, KP_INST_LDR_64, KP_INST_LDRSW_LIT,
	KP_INST_PRFM_LIT, KP_INST_LDR_SIMD_32, KP_INST_LDR_SIMD_64,
	KP_INST_LDR_SIMD_128, KP_INST_CBZ, KP_INST_CBNZ,
	KP_INST_TBZ, KP_INST_TBNZ, KP_INST_IGNORE,
};

static s32 kp_relo_len[] = {
	6, 8, 8, 4, 4, 6, 6, 6, 8, 8, 8, 8, 6, 6, 6, 6, 2,
};

static inline bool kp_is_bad_address(void *addr)
{
	return ((u64)addr & 0x8000000000000000ULL) !=
	       0x8000000000000000ULL;
}

static int kp_is_in_tramp(kp_hook_t *hook, u64 addr)
{
	u64 tramp_start = hook->origin_addr;
	u64 tramp_end = tramp_start + hook->tramp_insts_num * 4;

	return addr >= tramp_start && addr < tramp_end;
}

static u64 kp_relo_in_tramp(kp_hook_t *hook, u64 addr)
{
	u32 addr_inst_index;
	u64 fix_addr;
	u32 i;

	if (!kp_is_in_tramp(hook, addr))
		return addr;

	addr_inst_index = (addr - hook->origin_addr) / 4;
	fix_addr = hook->relo_addr;
	for (i = 0; i < addr_inst_index; i++) {
		u32 inst = hook->origin_insts[i];
		u32 j;

		for (j = 0; j < ARRAY_SIZE(kp_relo_len); j++) {
			if ((inst & kp_masks[j]) == kp_types[j]) {
				fix_addr += kp_relo_len[j] * 4;
				break;
			}
		}
	}

	return fix_addr;
}

static u64 kp_branch_func_addr_once(u64 addr)
{
	u32 inst = *(u32 *)addr;

	if ((inst & KP_MASK_B) == KP_INST_B) {
		u64 imm26 = kp_bits32(inst, 25, 0);
		u64 imm64 = kp_sign64_extend(imm26 << 2u, 28u);

		return addr + imm64;
	}

	if (inst == KP_ARM64_BTI_C || inst == KP_ARM64_BTI_J ||
	    inst == KP_ARM64_BTI_JC)
		return addr + 4;

	return addr;
}

u64 kp_branch_func_addr(u64 addr)
{
	u64 next;

	for (;;) {
		next = kp_branch_func_addr_once(addr);
		if (next == addr)
			return addr;
		addr = next;
	}
}

static kp_hook_err_t kp_relo_b(kp_hook_t *hook, u64 inst_addr, u32 inst,
			       kp_inst_type_t type)
{
	u32 *buf = hook->relo_insts + hook->relo_insts_num;
	u64 imm64;
	u64 addr;
	u32 idx = 0;

	if (type == KP_INST_BC) {
		u64 imm19 = kp_bits32(inst, 23, 5);

		imm64 = kp_sign64_extend(imm19 << 2u, 21u);
	} else {
		u64 imm26 = kp_bits32(inst, 25, 0);

		imm64 = kp_sign64_extend(imm26 << 2u, 28u);
	}

	addr = kp_relo_in_tramp(hook, inst_addr + imm64);

	if (type == KP_INST_BC) {
		buf[idx++] = (inst & 0xFF00001FU) | 0x40U;
		buf[idx++] = 0x14000006U;
	}

	buf[idx++] = 0x58000051U;
	buf[idx++] = 0x14000003U;
	buf[idx++] = lower_32_bits(addr);
	buf[idx++] = upper_32_bits(addr);

	if (type == KP_INST_BL) {
		buf[idx++] = 0x1000001EU;
		buf[idx++] = 0x910033DEU;
		buf[idx++] = 0xD65F0220U;
	} else {
		buf[idx++] = 0xD65F0220U;
	}

	buf[idx++] = KP_ARM64_NOP;
	return KP_HOOK_NO_ERR;
}

static kp_hook_err_t kp_relo_adr(kp_hook_t *hook, u64 inst_addr, u32 inst,
				 kp_inst_type_t type)
{
	u32 *buf = hook->relo_insts + hook->relo_insts_num;
	u32 xd = kp_bits32(inst, 4, 0);
	u64 immlo = kp_bits32(inst, 30, 29);
	u64 immhi = kp_bits32(inst, 23, 5);
	u64 addr;

	if (type == KP_INST_ADR) {
		addr = inst_addr + kp_sign64_extend((immhi << 2u) | immlo, 21u);
	} else {
		addr = (inst_addr +
			kp_sign64_extend((immhi << 14u) | (immlo << 12u),
					 33u)) &
		       0xFFFFFFFFFFFFF000ULL;
		if (kp_is_in_tramp(hook, addr))
			return -KP_HOOK_BAD_RELO;
	}

	buf[0] = 0x58000040U | xd;
	buf[1] = 0x14000003U;
	buf[2] = lower_32_bits(addr);
	buf[3] = upper_32_bits(addr);
	return KP_HOOK_NO_ERR;
}

static kp_hook_err_t kp_relo_ldr(kp_hook_t *hook, u64 inst_addr, u32 inst,
				 kp_inst_type_t type)
{
	u32 *buf = hook->relo_insts + hook->relo_insts_num;
	u32 rt = kp_bits32(inst, 4, 0);
	u64 imm19 = kp_bits32(inst, 23, 5);
	u64 addr = inst_addr + kp_sign64_extend(imm19 << 2u, 21u);

	if (kp_is_in_tramp(hook, addr) && type != KP_INST_PRFM_LIT)
		return -KP_HOOK_BAD_RELO;

	addr = kp_relo_in_tramp(hook, addr);

	if (type == KP_INST_LDR_32 || type == KP_INST_LDR_64 ||
	    type == KP_INST_LDRSW_LIT) {
		buf[0] = 0x58000060U | rt;
		if (type == KP_INST_LDR_32)
			buf[1] = 0xB9400000U | rt | (rt << 5u);
		else if (type == KP_INST_LDR_64)
			buf[1] = 0xF9400000U | rt | (rt << 5u);
		else
			buf[1] = 0xB9800000U | rt | (rt << 5u);
		buf[2] = 0x14000004U;
		buf[3] = KP_ARM64_NOP;
		buf[4] = lower_32_bits(addr);
		buf[5] = upper_32_bits(addr);
		return KP_HOOK_NO_ERR;
	}

	buf[0] = 0xA93F47F0U;
	buf[1] = 0x58000091U;
	if (type == KP_INST_PRFM_LIT)
		buf[2] = 0xF9800220U | rt;
	else if (type == KP_INST_LDR_SIMD_32)
		buf[2] = 0xBD400220U | rt;
	else if (type == KP_INST_LDR_SIMD_64)
		buf[2] = 0xFD400220U | rt;
	else
		buf[2] = 0x3DC00220U | rt;
	buf[3] = 0xF85F83F1U;
	buf[4] = 0x14000004U;
	buf[5] = KP_ARM64_NOP;
	buf[6] = lower_32_bits(addr);
	buf[7] = upper_32_bits(addr);
	return KP_HOOK_NO_ERR;
}

static kp_hook_err_t kp_relo_cb(kp_hook_t *hook, u64 inst_addr, u32 inst)
{
	u32 *buf = hook->relo_insts + hook->relo_insts_num;
	u64 imm19 = kp_bits32(inst, 23, 5);
	u64 addr = inst_addr + kp_sign64_extend(imm19 << 2u, 21u);

	addr = kp_relo_in_tramp(hook, addr);
	buf[0] = (inst & 0xFF00001FU) | 0x40U;
	buf[1] = 0x14000005U;
	buf[2] = 0x58000051U;
	buf[3] = 0xD65F0220U;
	buf[4] = lower_32_bits(addr);
	buf[5] = upper_32_bits(addr);
	return KP_HOOK_NO_ERR;
}

static kp_hook_err_t kp_relo_tb(kp_hook_t *hook, u64 inst_addr, u32 inst)
{
	u32 *buf = hook->relo_insts + hook->relo_insts_num;
	u64 imm14 = kp_bits32(inst, 18, 5);
	u64 addr = inst_addr + kp_sign64_extend(imm14 << 2u, 16u);

	addr = kp_relo_in_tramp(hook, addr);
	buf[0] = (inst & 0xFFF8001FU) | 0x40U;
	buf[1] = 0x14000005U;
	buf[2] = 0x58000051U;
	buf[3] = 0xD61F0220U;
	buf[4] = lower_32_bits(addr);
	buf[5] = upper_32_bits(addr);
	return KP_HOOK_NO_ERR;
}

static kp_hook_err_t kp_relo_ignore(kp_hook_t *hook, u64 inst_addr, u32 inst)
{
	u32 *buf = hook->relo_insts + hook->relo_insts_num;

	buf[0] = inst;
	buf[1] = KP_ARM64_NOP;
	return KP_HOOK_NO_ERR;
}

static u32 kp_can_b_rel(u64 src_addr, u64 dst_addr)
{
#define KP_B_REL_RANGE ((1 << 25) << 2)
	return ((dst_addr >= src_addr) && (dst_addr - src_addr <= KP_B_REL_RANGE)) ||
	       ((src_addr >= dst_addr) && (src_addr - dst_addr <= KP_B_REL_RANGE));
}

s32 kp_branch_relative(u32 *buf, u64 src_addr, u64 dst_addr)
{
	if (kp_can_b_rel(src_addr, dst_addr)) {
		buf[0] = 0x14000000U |
			 (((dst_addr - src_addr) & 0x0FFFFFFFU) >> 2u);
		buf[1] = KP_ARM64_NOP;
		return 2;
	}

	return 0;
}

s32 kp_branch_absolute(u32 *buf, u64 addr)
{
	buf[0] = 0x58000051U;
	buf[1] = 0xD61F0220U;
	buf[2] = lower_32_bits(addr);
	buf[3] = upper_32_bits(addr);
	return 4;
}

s32 kp_ret_absolute(u32 *buf, u64 addr)
{
	buf[0] = 0x58000051U;
	buf[1] = 0xD65F0220U;
	buf[2] = lower_32_bits(addr);
	buf[3] = upper_32_bits(addr);
	return 4;
}

static s32 kp_branch_from_to(u32 *tramp_buf, u64 src_addr, u64 dst_addr)
{
	return kp_ret_absolute(tramp_buf, dst_addr);
}

static kp_hook_err_t kp_relocate_inst(kp_hook_t *hook, u64 inst_addr, u32 inst)
{
	kp_hook_err_t rc = KP_HOOK_NO_ERR;
	kp_inst_type_t it = KP_INST_IGNORE;
	int len = 1;
	u32 j;

	for (j = 0; j < ARRAY_SIZE(kp_relo_len); j++) {
		if ((inst & kp_masks[j]) == kp_types[j]) {
			it = kp_types[j];
			len = kp_relo_len[j];
			break;
		}
	}

	switch (it) {
	case KP_INST_B:
	case KP_INST_BC:
	case KP_INST_BL:
		rc = kp_relo_b(hook, inst_addr, inst, it);
		break;
	case KP_INST_ADR:
	case KP_INST_ADRP:
		rc = kp_relo_adr(hook, inst_addr, inst, it);
		break;
	case KP_INST_LDR_32:
	case KP_INST_LDR_64:
	case KP_INST_LDRSW_LIT:
	case KP_INST_PRFM_LIT:
	case KP_INST_LDR_SIMD_32:
	case KP_INST_LDR_SIMD_64:
	case KP_INST_LDR_SIMD_128:
		rc = kp_relo_ldr(hook, inst_addr, inst, it);
		break;
	case KP_INST_CBZ:
	case KP_INST_CBNZ:
		rc = kp_relo_cb(hook, inst_addr, inst);
		break;
	case KP_INST_TBZ:
	case KP_INST_TBNZ:
		rc = kp_relo_tb(hook, inst_addr, inst);
		break;
	case KP_INST_IGNORE:
	default:
		rc = kp_relo_ignore(hook, inst_addr, inst);
		break;
	}

	hook->relo_insts_num += len;
	return rc;
}

kp_hook_err_t kp_hook_prepare(kp_hook_t *hook)
{
	int i;

	if (kp_is_bad_address((void *)hook->func_addr) ||
	    kp_is_bad_address((void *)hook->origin_addr) ||
	    kp_is_bad_address((void *)hook->replace_addr) ||
	    kp_is_bad_address((void *)hook->relo_addr))
		return -KP_HOOK_BAD_ADDRESS;

	for (i = 0; i < KP_HOOK_TRAMPOLINE_MAX_NUM; i++)
		hook->origin_insts[i] = *((u32 *)hook->origin_addr + i);

	if (hook->origin_insts[0] == KP_ARM64_PACIASP ||
	    hook->origin_insts[0] == KP_ARM64_PACIBSP) {
		hook->tramp_insts_num = kp_branch_from_to(&hook->tramp_insts[1],
							 hook->origin_addr,
							 hook->replace_addr);
		hook->tramp_insts[0] = KP_ARM64_BTI_JC;
		hook->tramp_insts_num++;
	} else {
		hook->tramp_insts_num = kp_branch_from_to(hook->tramp_insts,
							 hook->origin_addr,
							 hook->replace_addr);
	}

	for (i = 0; i < ARRAY_SIZE(hook->relo_insts); i++)
		hook->relo_insts[i] = KP_ARM64_NOP;

	for (i = 0; i < hook->tramp_insts_num; i++) {
		u64 inst_addr = hook->origin_addr + i * 4;
		u32 inst = hook->origin_insts[i];
		kp_hook_err_t rc = kp_relocate_inst(hook, inst_addr, inst);

		if (rc)
			return -KP_HOOK_BAD_RELO;
	}

	{
		u64 back_src_addr = hook->relo_addr + hook->relo_insts_num * 4;
		u64 back_dst_addr = hook->origin_addr + hook->tramp_insts_num * 4;
		u32 *buf = hook->relo_insts + hook->relo_insts_num;

		hook->relo_insts_num +=
			kp_branch_from_to(buf, back_src_addr, back_dst_addr);
	}

	return KP_HOOK_NO_ERR;
}
