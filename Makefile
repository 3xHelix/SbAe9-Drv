# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# Creative Sound Blaster AE-9 HDA controller module

snd-hda-codec-creative-ae9-objs := creative_ae9_core.o creative_ae9_corb.o creative_ae9_pcm.o creative_ae9_i2c.o creative_ae9_dsp.o creative_ae9_fx.o

obj-m += snd-hda-codec-creative-ae9.o

# hrtimer_setup() replaced hrtimer_init() in 6.13; some distro kernels
# (RHEL 10's 6.12) have it backported, Ubuntu 24.04's 6.8 doesn't: check
# the kernel's own header instead of its version
ccflags-y += $(shell grep -qs 'void hrtimer_setup\b' $(srctree)/include/linux/hrtimer.h && echo -DAE9_HAVE_HRTIMER_SETUP)

KDIR := /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

.PHONY: all clean install
