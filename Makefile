ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(src)/include
ccflags-y += -I$(src)/third_party/kernelpatch/include

obj-m += lkmdbg.o

lkmdbg-core-y := \
	core/lkmdbg_hook_registry.o \
	core/lkmdbg_main.o \
	core/lkmdbg_runtime_hooks.o \
	core/lkmdbg_symbols.o \
	core/lkmdbg_protect.o

lkmdbg-hook-y := \
	third_party/kernelpatch/kp_hook_core.o \
	hook/lkmdbg_hook_arm64.o

lkmdbg-transport-y := \
	transport/lkmdbg_session.o \
	transport/lkmdbg_transport_proc.o

lkmdbg-mem-y := \
	mem/lkmdbg_mem.o \
	mem/lkmdbg_target.o \
	mem/lkmdbg_vma.o

lkmdbg-task-y := \
	task/lkmdbg_freeze.o

lkmdbg-ui-y := \
	ui/lkmdbg_debugfs.o

lkmdbg-y := \
	$(lkmdbg-core-y) \
	$(lkmdbg-hook-y) \
	$(lkmdbg-transport-y) \
	$(lkmdbg-mem-y) \
	$(lkmdbg-task-y) \
	$(lkmdbg-ui-y)
else
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
