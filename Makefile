obj-m := unart.o

unart-y := \
	unart_module.o \
	unart_tty.o \
	unart_rx.o \
	unart_tx.o

ccflags-y := -Wno-declaration-after-statement

ifneq ($(KERNEL_SRC),)
	KDIR = $(KERNEL_SRC)
endif
KDIR ?= /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

dtbs:
	dtc -@ -I dts -O dtb -o example/unart.dtbo example/unart-overlay.dts

checkpatch:
	@$(KDIR)/scripts/checkpatch.pl --no-tree --terse \
		--ignore LINUX_VERSION_CODE,CONSTANT_COMPARISON \
		--ignore LINE_SPACING \
		$(shell git ls-files '*.h' '*.c')

.PHONY: all clean modules_install dtbs checkpatch
