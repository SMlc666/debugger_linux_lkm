/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from KernelPatch:
 * https://github.com/bmax121/KernelPatch
 * Original Copyright (C) 2023 bmax121.
 */

#ifndef _LKMDBG_KP_HOOK_H
#define _LKMDBG_KP_HOOK_H

#include <linux/types.h>

typedef enum {
	KP_HOOK_NO_ERR = 0,
	KP_HOOK_BAD_ADDRESS = 4095,
	KP_HOOK_DUPLICATED = 4094,
	KP_HOOK_NO_MEM = 4093,
	KP_HOOK_BAD_RELO = 4092,
} kp_hook_err_t;

#define KP_HOOK_TRAMPOLINE_MAX_NUM 6
#define KP_HOOK_RELOCATE_INST_NUM (4 * 8 + 8 - 4)

#define KP_ARM64_NOP 0xd503201fU
#define KP_ARM64_BTI_C 0xd503245fU
#define KP_ARM64_BTI_J 0xd503249fU
#define KP_ARM64_BTI_JC 0xd50324dfU
#define KP_ARM64_PACIASP 0xd503233fU
#define KP_ARM64_PACIBSP 0xd503237fU

typedef struct {
	u64 func_addr;
	u64 origin_addr;
	u64 replace_addr;
	u64 relo_addr;
	s32 tramp_insts_num;
	s32 relo_insts_num;
	u32 origin_insts[KP_HOOK_TRAMPOLINE_MAX_NUM] __aligned(8);
	u32 tramp_insts[KP_HOOK_TRAMPOLINE_MAX_NUM] __aligned(8);
	u32 relo_insts[KP_HOOK_RELOCATE_INST_NUM] __aligned(8);
} kp_hook_t __aligned(8);

u64 kp_branch_func_addr(u64 addr);
s32 kp_branch_relative(u32 *buf, u64 src_addr, u64 dst_addr);
s32 kp_branch_absolute(u32 *buf, u64 addr);
s32 kp_ret_absolute(u32 *buf, u64 addr);
kp_hook_err_t kp_hook_prepare(kp_hook_t *hook);

#endif
