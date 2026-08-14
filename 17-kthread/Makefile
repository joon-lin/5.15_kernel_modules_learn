obj-m += si_kthread17.o

# 这些变量通常由 `bitbake -c devshell virtual/kernel` 提供。
KERNEL_OUT ?= $(KBUILD_OUTPUT)
KERNEL_SRC ?= $(realpath $(KERNEL_OUT)/source)
ARCH ?= arm
CROSS_COMPILE ?= $(TARGET_PREFIX)
MODULE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
KBUILD_ARGS := ARCH=$(ARCH)
ifneq ($(strip $(CROSS_COMPILE)),)
KBUILD_ARGS += CROSS_COMPILE=$(CROSS_COMPILE)
endif

ifeq ($(strip $(KERNEL_OUT)),)
$(error KBUILD_OUTPUT is not set. Enter the kernel devshell first, or pass KERNEL_OUT=/path/to/kernel-build)
endif
ifeq ($(strip $(KERNEL_SRC)),)
$(error Kernel source is not set and $(KERNEL_OUT)/source was not found. Pass KERNEL_SRC=/path/to/kernel-source)
endif
ifeq ($(wildcard $(KERNEL_SRC)/Makefile),)
$(error Kernel source not found: $(KERNEL_SRC))
endif
ifeq ($(wildcard $(KERNEL_OUT)/.config),)
$(error Kernel output not found: $(KERNEL_OUT)/.config. Build virtual/kernel first.)
endif

.PHONY: all clean info

all:
	@echo "Kernel source : $(KERNEL_SRC)"
	@echo "Kernel output : $(KERNEL_OUT)"
	@echo "Module source : $(MODULE_DIR)"
	@echo "ARCH          : $(ARCH)"
	@echo "CROSS_COMPILE : $(CROSS_COMPILE)"
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) M=$(MODULE_DIR) \
		$(KBUILD_ARGS) \
		modules

clean:
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) M=$(MODULE_DIR) clean

info:
	@echo "Kernel source : $(KERNEL_SRC)"
	@echo "Kernel output : $(KERNEL_OUT)"
	@echo "Module source : $(MODULE_DIR)"
	@echo "ARCH          : $(ARCH)"
	@echo "CROSS_COMPILE : $(CROSS_COMPILE)"
