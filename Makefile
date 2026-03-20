ifneq ($(KERNELRELEASE),)
obj-m += lkmdbg.o
lkmdbg-y := lkmdbg_main.o lkmdbg_debugfs.o lkmdbg_session.o lkmdbg_mem.o lkmdbg_transport_proc.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
