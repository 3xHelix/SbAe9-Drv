# SPDX-License-Identifier: GPL-2.0-only
# Creative Sound Blaster AE-9 HDA controller module

snd-hda-codec-creative-ae9-objs := creative_ae9_core.o creative_ae9_corb.o creative_ae9_pcm.o creative_ae9_i2c.o creative_ae9_dsp.o

obj-m += snd-hda-codec-creative-ae9.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

.PHONY: all clean install
