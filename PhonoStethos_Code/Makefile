BUILDROOT_DIR ?= /home/igor/buildroot/buildroot-2026.08
BUILD_DIR ?= app/build-pi
TOOLCHAIN_FILE := $(abspath app/cmake/buildroot-aarch64.cmake)
KERNEL_DIR ?= $(BUILDROOT_DIR)/output/build/linux-custom
HOST_DIR := $(BUILDROOT_DIR)/output/host
OVERLAY_DIR ?= $(BUILDROOT_DIR)/output/target
BOOT_DIR ?= $(BUILDROOT_DIR)/output/images/rpi-firmware
KERNEL_RELEASE = $(shell $(MAKE) -s -C $(KERNEL_DIR) kernelrelease)

.PHONY: all configure build driver install clean

all: build driver

configure:
	cmake -S app -B $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		-DBUILDROOT_DIR=$(BUILDROOT_DIR) \
		-DCMAKE_BUILD_TYPE=Release

build: configure
	cmake --build $(BUILD_DIR) --parallel

driver:
	$(MAKE) -C kernel-driver \
		KDIR=$(KERNEL_DIR) \
		CROSS_COMPILE=$(HOST_DIR)/bin/aarch64-linux- \
		ARCH=arm64

install: build driver
	install -D -m 0755 $(BUILD_DIR)/phonostethos-server \
		$(OVERLAY_DIR)/usr/bin/phonostethos-server
	install -D -m 0644 kernel-driver/phonostethos_leds.ko \
		$(OVERLAY_DIR)/lib/modules/$(KERNEL_RELEASE)/extra/phonostethos_leds.ko
	install -D -m 0644 kernel-driver/phonostethos-leds.rules \
		$(OVERLAY_DIR)/etc/udev/rules.d/99-phonostethos-leds.rules
	install -D -m 0644 kernel-driver/phonostethos-usb.rules \
		$(OVERLAY_DIR)/etc/udev/rules.d/99-phonostethos-usb.rules
	install -D -m 0755 kernel-driver/deploy/phonostethos-usb-mount \
		$(OVERLAY_DIR)/usr/sbin/phonostethos-usb-mount
	install -D -m 0644 kernel-driver/build/phonostethos-status-leds.dtbo \
		$(BOOT_DIR)/overlays/phonostethos-status-leds.dtbo
	install -D -m 0755 kernel-driver/deploy/S90phonostethos \
		$(OVERLAY_DIR)/etc/init.d/S90phonostethos

clean:
	rm -rf app/build app/build-pi
	$(MAKE) -C kernel-driver KDIR=$(KERNEL_DIR) clean
