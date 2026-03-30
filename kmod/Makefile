# out-of-tree kernel module build
#
# usage:
#   make KDIR=/path/to/kernel/source CROSS_COMPILE=arm-none-linux-gnueabi-
#
# KDIR should point to the ht818 kernel source tree
# (materials/linux-4.9.0 in the RE workspace)

KDIR ?= $(HOME)/Nextcloud/ht818/materials/linux-4.9.0
CROSS_COMPILE ?= arm-none-linux-gnueabi-
ARCH ?= arm

obj-m := comatose_tdm.o

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean
