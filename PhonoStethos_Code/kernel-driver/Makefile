obj-m += phonostethos_leds.o

KDIR ?= /home/igor/buildroot/buildroot-2026.08/output/build/linux-custom
ARCH ?= arm64
CROSS_COMPILE ?= /home/igor/buildroot/buildroot-2026.08/output/host/bin/aarch64-linux-

.PHONY: all clean install overlay

all: overlay
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

install: all
	install -D -m 0644 phonostethos_leds.ko \
		$(DESTDIR)/lib/modules/$(shell $(MAKE) -s -C $(KDIR) kernelrelease)/extra/phonostethos_leds.ko

# Firmware loads this overlay before the module probes its GPIO descriptors.
DTC ?= /home/igor/buildroot/buildroot-2026.08/output/host/bin/dtc
OVERLAY := build/phonostethos-status-leds.dtbo

overlay: $(OVERLAY)

$(OVERLAY): phonostethos-status-leds-overlay.dts
	mkdir -p build
	$(CROSS_COMPILE)gcc -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		-I $(KDIR)/include $< -o build/phonostethos-status-leds.dts
	$(DTC) -@ -I dts -O dtb -o $@ build/phonostethos-status-leds.dts
