# Building and deploying the revised project

The source is updated in this folder. Nothing was copied to a running Raspberry Pi by the code revision.

## Server and tests

Build on a development machine with ALSA, FFTW3f, libsndfile and libudev development packages:

```sh
cmake -S app -B app/build
cmake --build app/build
ctest --test-dir app/build --output-on-failure
```

Cross-build using the existing Buildroot SDK:

```sh
make build
make driver
```

The server is C++17 and uses direct pthread APIs for execution and synchronization. `find_package(Threads)` supplies the platform threading link options.

## Simple direct-register LED driver

The LED module maps the Raspberry Pi 4 BCM2711 GPIO register block at physical address `0xFE200000` using `ioremap`, then configures BCM17, BCM27, BCM24 and BCM22 as outputs. It creates `/dev/phonostethos_leds`; no GPIO overlay or `config.txt` change is needed. This fixed address is specific to the Raspberry Pi 4 and will not work unchanged on a Raspberry Pi 5 or a different board.

Build the module against the exact kernel that will run it, install or copy `phonostethos_leds.ko` to the Pi, then load it with `insmod`. Check `dmesg` and `/dev/phonostethos_leds`. The application writes one byte containing the full LED mask; for a manual test, `printf '\005' > /dev/phonostethos_leds` turns on Heart and General, and `printf '\000' > /dev/phonostethos_leds` turns all LEDs off.

This educational driver accesses hardware registers directly rather than using the kernel GPIO subsystem. Do not use another driver or program to control these same GPIO pins at the same time.

## Device permissions and service startup

The udev rule uses group `phonostethos` and mode 0660. Add `kernel-driver/deploy/users.txt` to the Buildroot `BR2_ROOTFS_USERS_TABLE` inputs to create the matching service user/group. Add a development user to that group if they need manual access to the LED character device.

The supplied init script retains its existing root execution model: mounting storage and requesting FIFO priorities need appropriate privileges. It does not silently change to an unprivileged service account. If the LED module cannot load, the service logs the problem and starts the audio application without LED indicators.

After boot, inspect logs and `/dev/phonostethos_leds`. The dashboard should report LED_DRIVER_ERROR if the character-device interface is unavailable. Check the real LED outputs and ALSA devices on the Pi; a host build cannot verify physical wiring, acoustic behaviour or target timing.

## Preserved design details

The existing filter cascade and listening profiles were retained. Some return types include success/failure and runtime helper methods in addition to the representative class-diagram labels. WAV filename collision retry, GUI acknowledgement details and other report observations outside the requested pthread/class/FFTW/LED work are not claimed to have all been redesigned by this revision.

## Checks performed in the previous application revision

- Full server cross-build with the project's aarch64 Buildroot SDK: passed.
- Previous GPIO-descriptor module build and Device Tree overlay build: passed; these were replaced by the direct-register driver.
- Five native tests and a native server TCP check: passed.
- Startup script syntax and diff whitespace checks: passed.

The native runtime check uses the system's shared libraries and copies of the matching development headers in the chat workspace. The TCP check uses temporary storage and a temporary regular file in place of the LED device. Its FIFO scheduling requests were denied by the host, as expected without real-time privileges; the target scheduling configuration must be verified on the Pi. No test result here asserts real microphone/headphone performance, physical LED illumination or on-Pi timing.

A source backup from before this revision is retained at:
`/home/igor/Documents/Codex/2026-10-31/if-x20/outputs/PhonoStethos_before_pthreads.zip`.
