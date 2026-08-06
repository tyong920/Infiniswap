ifneq ($(KERNELRELEASE),)

obj-m := infiniswap.o
infiniswap-y := is_configfs.o is_mq.o is_main.o

INFINISWAP_MAX_PAGES_PER_REQUEST ?= 32
INFINISWAP_BIO_PAGE_CAP ?= 32
INFINISWAP_MAX_REMOTE_MEMORY_GB ?= 32
INFINISWAP_DEVICE_SIZE_GB ?= 12
INFINISWAP_DEVICE_NAME ?= stackbd
INFINISWAP_BACKING_STORE ?= /dev/sda4
INFINISWAP_PROVIDER_SAMPLE_SIZE ?= 1

ccflags-y += \
  -DUSER_MAX_PAGE_NUM=$(INFINISWAP_MAX_PAGES_PER_REQUEST) \
  -DUSER_BIO_PAGE_CAP=$(INFINISWAP_BIO_PAGE_CAP) \
  -DUSER_MAX_REMOTE_MEMORY=$(INFINISWAP_MAX_REMOTE_MEMORY_GB) \
  -DUSER_STACKBD_SIZE=$(INFINISWAP_DEVICE_SIZE_GB) \
  -DUSER_STACKBD_NAME=\"$(INFINISWAP_DEVICE_NAME)\" \
  -DUSER_BACKUP_DISK=\"$(INFINISWAP_BACKING_STORE)\" \
  -DUSER_NUM_SERVER_SELECT=$(INFINISWAP_PROVIDER_SAMPLE_SIZE)

kernel_include_dirs := $(srctree)/include $(objtree)/include
ifneq ($(wildcard $(srctree)/include/linux/genhd.h),)
ccflags-y += -DINFINISWAP_HAVE_LINUX_GENHD_H
endif
ifneq ($(shell grep -s -l 'struct bdev_handle' \
  $(addsuffix /linux/blkdev.h,$(kernel_include_dirs)) 2>/dev/null),)
ccflags-y += -DINFINISWAP_HAVE_BDEV_HANDLE
endif
ifneq ($(shell grep -s -l 'bio_alloc_clone' \
  $(addsuffix /linux/bio.h,$(kernel_include_dirs)) 2>/dev/null),)
ccflags-y += -DINFINISWAP_HAVE_BIO_ALLOC_CLONE
endif

rdma_detected := $(shell $(src)/scripts/rdma-build-flags.sh detected \
  "$(KERNELRELEASE)" "$(INFINISWAP_RDMA_ROOT)")
rdma_cflags := $(shell $(src)/scripts/rdma-build-flags.sh cflags \
  "$(KERNELRELEASE)" "$(INFINISWAP_RDMA_ROOT)")
rdma_symvers := $(shell $(src)/scripts/rdma-build-flags.sh symvers \
  "$(KERNELRELEASE)" "$(INFINISWAP_RDMA_ROOT)")
rdma_required := $(strip $(rdma_detected) $(INFINISWAP_RDMA_ROOT))
ifneq ($(rdma_required),)
ifeq ($(strip $(rdma_cflags)),)
$(error MLNX_OFED headers not found for kernel $(KERNELRELEASE))
endif
ifeq ($(strip $(rdma_symvers)),)
$(error MLNX_OFED Module.symvers not found for kernel $(KERNELRELEASE))
endif
endif
NOSTDINC_FLAGS += $(rdma_cflags)
KBUILD_EXTRA_SYMBOLS += $(rdma_symvers)

else

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all modules clean install
all modules:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean

install:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules_install

endif
