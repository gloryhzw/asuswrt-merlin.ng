obj-m += b53_ref.o b53_jump.o
KDIR ?= $(CURDIR)/../../../release/src-rt-5.04behnd.4916/kernel/linux-4.19
ccflags-y += -I$(src)/../../../release/src-rt-5.04behnd.4916/bcmdrivers/opensource/include/bcm963xx
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 CROSS_COMPILE=aarch64-linux- BUILD_NAME=RT-BE92U modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 CROSS_COMPILE=aarch64-linux- clean
