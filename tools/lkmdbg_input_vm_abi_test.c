#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "../include/lkmdbg_ioctl.h"

_Static_assert(sizeof(struct lkmdbg_input_vm_insn) == 16,
	       "input VM instruction ABI changed");
_Static_assert(offsetof(struct lkmdbg_input_vm_insn, offset) == 4,
	       "input VM offset ABI changed");
_Static_assert(offsetof(struct lkmdbg_input_vm_insn, imm) == 8,
	       "input VM immediate ABI changed");
_Static_assert(sizeof(struct lkmdbg_input_vm_load_request) == 40,
	       "input VM load request ABI changed");

int main(void)
{
	struct lkmdbg_input_vm_insn pass = {
		.opcode = LKMDBG_INPUT_VM_OP_PASS,
	};
	struct lkmdbg_input_vm_insn jump = {
		.opcode = LKMDBG_INPUT_VM_OP_JNZ,
		.dst = 0,
		.offset = 1,
	};

	assert(pass.opcode == 13U);
	assert(jump.opcode == LKMDBG_INPUT_VM_OP_JNZ);
	assert(jump.offset > 0);
	assert(LKMDBG_INPUT_VM_MAX_INSNS == 256U);
	assert(LKMDBG_INPUT_VM_MAX_STATE == 64U);
	assert(LKMDBG_INPUT_VM_MAX_OUTPUTS == 8U);
	assert((LKMDBG_INPUT_CHANNEL_FLAG_CONTROLLER &
		LKMDBG_INPUT_CHANNEL_FLAG_RAW_EVENTS) == 0);
	puts("input VM ABI: ok");
	return 0;
}
