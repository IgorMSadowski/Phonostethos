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

## GPIO descriptor driver: required boot overlay

The LED module now acquires named GPIO descriptors from Device Tree. The existing overlay maps HEART=17, LUNG=27, GENERAL=24 and RECORDING=22, active high. The old raw-register driver did not need this overlay; the new descriptor driver does.

`make driver` builds both the module and `kernel-driver/build/phonostethos-status-leds.dtbo`. `make install` stages the module and overlay using the configured Makefile paths. By default, BOOT_DIR is Buildroot's `output/images/rpi-firmware`; OVERLAY_DIR is its `output/target` root-filesystem tree. These are image staging paths, not an automatic deployment to the Pi.

For the target firmware partition:

1. Put `phonostethos-status-leds.dtbo` in its `overlays/` directory.
2. Add `dtoverlay=phonostethos-status-leds` to the applicable Raspberry Pi firmware `config.txt` section.
3. Install the matching `phonostethos_leds.ko` for the exact running kernel.
4. Rebuild/deploy the image or transfer the corresponding boot/rootfs files using your normal deployment procedure, then reboot.

With Buildroot, keep the overlay and config line in your board configuration/post-build process so a later image rebuild retains them. The project does not edit your external Buildroot configuration automatically.

The kernel must include GPIOLIB, Device Tree and the Pi GPIO controller. The supplied Buildroot kernel has these enabled and the revised module was compiled against that kernel.

## Device permissions and service startup

The udev rule uses group `phonostethos` and mode 0660. Add `kernel-driver/deploy/users.txt` to the Buildroot `BR2_ROOTFS_USERS_TABLE` inputs to create the matching service user/group. Add a development user to that group if they need manual access to the LED character device.

The supplied init script retains its existing root execution model: mounting storage and requesting FIFO priorities need appropriate privileges. It does not silently change to an unprivileged service account. If the LED module cannot load, the service logs the problem and starts the audio application without LED indicators.

After boot, inspect logs and `/dev/phonostethos_leds`. The dashboard should report LED_DRIVER_ERROR if the character-device interface is unavailable. Check the real LED outputs and ALSA devices on the Pi; a host build cannot verify physical wiring, acoustic behaviour or target timing.

## Preserved design details

The existing filter cascade and listening profiles were retained. Some return types include success/failure and runtime helper methods in addition to the representative class-diagram labels. WAV filename collision retry, GUI acknowledgement details and other report observations outside the requested pthread/class/FFTW/LED work are not claimed to have all been redesigned by this revision.

## Checks performed during this revision

- Full server cross-build with the project's aarch64 Buildroot SDK: passed.
- GPIO descriptor kernel module build against the configured target kernel: passed.
- Device-tree overlay preprocessing/compilation: passed.
- Five native tests: filter profiles, protocol, architecture utilities, concurrent pthread synchronization, and DSP/WAV/LED-command integration: passed.
- Native server TCP check using ALSA null devices: partial/coalesced commands, ordered control results, reconnect state, invalid argument rejection and archive listing: passed.
- Startup script syntax and diff whitespace checks: passed.

The native runtime check uses the system's shared libraries and copies of the matching development headers in the chat workspace. The TCP check uses temporary storage and a temporary regular file in place of the LED device. Its FIFO scheduling requests were denied by the host, as expected without real-time privileges; the target scheduling configuration must be verified on the Pi. No test result here asserts real microphone/headphone performance, physical LED illumination or on-Pi timing.

A source backup from before this revision is retained at:
`/home/igor/Documents/Codex/2026-10-31/if-x20/outputs/PhonoStethos_before_pthreads.zip`.
