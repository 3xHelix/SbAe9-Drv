# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# Creative Sound Blaster AE-9 HDA controller module

snd-hda-codec-creative-ae9-objs := creative_ae9_core.o creative_ae9_corb.o creative_ae9_pcm.o creative_ae9_i2c.o creative_ae9_dsp.o creative_ae9_fx.o creative_ae9_seq.o creative_ae9_seq_run.o
# The Sound Blaster AE-7 (experimental, opt-in with the option ae7=1) is in
# every build: creative_ae7_core.c, _fx.c, _pcm.c, _seq.c and _run.c (with
# creative_ae7.h) are compiled as part of creative_ae9_core.c, _fx.c,
# _pcm.c, _seq.c and _seq_run.c, which #include them at their end (kbuild
# tracks them as those objects' dependencies). Every source file listed
# here and there must be in the module's source tree (DKMS, packages).

obj-m += snd-hda-codec-creative-ae9.o

# hrtimer_setup() replaced hrtimer_init() in 6.13; some distro kernels
# (RHEL 10's 6.12) have it backported, Ubuntu 24.04's 6.8 doesn't: check
# the kernel's own header instead of its version
ccflags-y += $(shell grep -qs 'void hrtimer_setup\b' $(srctree)/include/linux/hrtimer.h && echo -DAE9_HAVE_HRTIMER_SETUP)

# make AE9_TEST=1: a test build, with the fault-injection hooks
# rirb_inject, rirb_inject_us, hung_inject, dsp_fail_inject and
# scp_full_inject (docs/OPTIONS.md), and the AE-7's ae7_test and ae7_lanes.
# Release builds leave them out.
ifeq ($(AE9_TEST),1)
ccflags-y += -DAE9_TEST
endif

KDIR := /lib/modules/$(shell uname -r)/build

# A clang-built kernel (XanMod, CachyOS...) needs clang for its modules:
# DKMS adds LLVM=1 to the make line for one. LLVM=1 runs plain `clang`,
# which some systems only have as clang-21 or /usr/lib/llvm-21/bin/clang
# (Ubuntu/Mint's versioned packages). Without a plain clang, the kernel's
# own clang major (CONFIG_CLANG_VERSION 210100 -> 21) picks kbuild's
# LLVM=-21 (clang-21, ld.lld-21... on PATH) or LLVM=/usr/lib/llvm-21/bin/.
# A make without LLVM= (and CC=) on such a kernel gets the same choice.
# GCC-built kernels: nothing here applies. Outer pass only: kbuild's own
# pass over this file has KBUILD_EXTMOD set (KERNELRELEASE can't tell them
# apart: DKMS 3.x puts KERNELRELEASE= on the outer make's command line).
ifeq ($(KBUILD_EXTMOD),)
ae9_kconf := $(firstword $(wildcard $(KDIR)/include/config/auto.conf $(KDIR)/.config))
ifneq ($(ae9_kconf),)
ifeq ($(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(ae9_kconf) && echo y),y)
ifneq ($(origin LLVM)$(origin CC),undefinedcommand line)
ifeq ($(origin LLVM),undefined)
ae9_auto := 1
LLVM := 1
endif
endif
ifeq ($(LLVM),1)
ae9_cv := $(shell sed -n 's/^CONFIG_CLANG_VERSION=\([1-9][0-9]*\)[0-9]\{4\}$$/\1/p' $(ae9_kconf))
ifneq ($(shell command -v clang),)
ae9_llvm := 1
else ifneq ($(ae9_cv),)
ifeq ($(shell command -v clang-$(ae9_cv) >/dev/null && command -v ld.lld-$(ae9_cv) >/dev/null && echo y),y)
ae9_llvm := -$(ae9_cv)
else ifeq ($(shell test -x /usr/lib/llvm-$(ae9_cv)/bin/clang && test -x /usr/lib/llvm-$(ae9_cv)/bin/ld.lld && echo y),y)
ae9_llvm := /usr/lib/llvm-$(ae9_cv)/bin/
endif
endif
ifeq ($(ae9_llvm),)
ae9_v := $(or $(ae9_cv),N)
ae9_nollvm := $(KDIR) is a clang-built kernel, but no clang, clang-$(ae9_v) with ld.lld-$(ae9_v), or /usr/lib/llvm-$(ae9_v)/bin/clang was found: install clang and lld (Debian, Ubuntu, Mint: clang-$(ae9_v) and lld-$(ae9_v))
ifeq ($(ae9_auto),1)
$(warning $(ae9_nollvm); trying the default compiler)
LLVM :=
else
$(error $(ae9_nollvm))
endif
else ifneq ($(ae9_llvm)$(ae9_auto),1)
$(info clang-built kernel: building with LLVM=$(ae9_llvm))
endif
endif
endif
endif
endif

# On the kernel's make line only when chosen above (otherwise a command
# line LLVM= reaches it as given).
ae9_llvm_arg := $(if $(ae9_llvm),LLVM=$(ae9_llvm))

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(ae9_llvm_arg) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(ae9_llvm_arg) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(ae9_llvm_arg) modules_install
	depmod -a

.PHONY: all clean install
