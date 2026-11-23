# Same cross-compilation style as DeviceDrivers/DD_AlsaTest/Makefile.
TARGET := PthreadTest
SRC := PthreadTest.cpp 
BUILDROOT_DIR ?= /home/igor/buildroot/buildroot-2026.08
CROSS_COMPILE ?= $(BUILDROOT_DIR)/output/host/bin/aarch64-linux-
COMPILER := $(CROSS_COMPILE)g++
BUILD_DIR ?= build-pi
FLAGS := -std=c++17 -O2 -Wall -Wextra
CPPFLAGS := -I../../PhonoStethos_Code/app/include
LDLIBS :=  -pthread
PKG_CONFIG ?= pkg-config

.PHONY: all native clean
all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(SRC) $(wildcard ../../PhonoStethos_Code/app/include/phonostethos/*.h)
	mkdir -p $(BUILD_DIR)
	$(COMPILER) $(CPPFLAGS) $(FLAGS) $(SRC) -o $@ $(LDLIBS)

native:
	$(MAKE) CROSS_COMPILE= BUILD_DIR=build-host all

clean:
	rm -f build-pi/$(TARGET) build-host/$(TARGET)
