obj-m += phonostethos_leds.o

KDIR ?= /home/igor/buildroot/buildroot-2026.08/output/build/linux-custom
ARCH ?= arm64
CROSS_COMPILE ?= /home/igor/buildroot/buildroot-2026.08/output/host/bin/aarch64-linux-

.PHONY: all clean install

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

install: all
	install -D -m 0644 phonostethos_leds.ko \
		$(DESTDIR)/lib/modules/$(shell $(MAKE) -s -C $(KDIR) kernelrelease)/extra/phonostethos_leds.ko
