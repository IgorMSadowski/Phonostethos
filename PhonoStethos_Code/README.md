# PhonoStethos

Start with [How the code works](docs/CODE_GUIDE.md). Build/boot changes are in
[Deployment](docs/DEPLOYMENT.md).

The source tree separates the Raspberry Pi kernel driver, C++ application, and
Flutter host client:

```text
kernel-driver/
  Makefile
  phonostethos_leds.c
app/
  CMakeLists.txt
  include/phonostethos/
  src/
gui/
  phonostethos_gui/
```

## Application

The C++ server uses ALSA for PCM I/O, FFTW3f for the spectrum, libsndfile for
WAV files, and the custom LED character device. It processes mono PCM16 at
8 kHz in 512-sample periods. By default, capture uses the USB audio interface
named `HiFiAudio`, and playback uses the Pi headphone output named
`Headphones`. These ALSA names match the Pi 4 setup; override them with
`--capture-pcm` or `--playback-pcm` if the hardware reports different card
names. The `plughw` devices let ALSA adapt the hardware format to the server's
mono 8 kHz stream.

Install the ALSA, FFTW3f, and libsndfile development packages, then build and
test the protocol, pthread synchronization, DSP and file utilities:

```sh
cmake -S app -B app/build
cmake --build app/build
ctest --test-dir app/build --output-on-failure
```

Use the root `Makefile` to build the Pi application and kernel module against
the configured Buildroot toolchain and kernel tree.

## Kernel driver

The driver source is `kernel-driver/phonostethos_leds.c`. It creates
`/dev/phonostethos_leds` after probing the `phonostethos,status-leds` Device Tree
node. It uses the kernel GPIO descriptor (`gpiod`) interface. The LED mask is
HEART `0x01` / BCM17, LUNG `0x02` / BCM27, GENERAL `0x04` / BCM24, and recording
`0x08` / BCM22. Enable `dtoverlay=phonostethos-status-leds` in the target firmware
configuration; the overlay is required by this driver.

Build it against the exact target kernel headers with:

```sh
make -C kernel-driver KDIR=/path/to/linux \
  CROSS_COMPILE=/path/to/toolchain/bin/aarch64-linux- ARCH=arm64
```

The root `Makefile` stages the kernel module, GPIO overlay, udev permissions and
init script. The udev rule uses the `phonostethos` group; the Buildroot users-table
input is supplied in `kernel-driver/deploy/users.txt`. See the deployment guide
for the firmware config line and exact staging paths.

The server stores recordings on the drive mounted at `/media/usb_storage`.
The Buildroot overlay installs an eudev rule to mount USB FAT32
partitions there when connected and unmount them when removed. The root
`make install` stages the rule and mount helper; rebuild
and deploy the image for automatic mounting to take effect. Do not remove the
drive during recording or playback; if it is busy on removal, stop using it and
unmount it manually.

To check the mount status on the Pi:

```sh
cat /proc/mounts | grep /media/usb_storage
```

The connected GUI updates its storage status automatically, and the server can
remain running while the drive is mounted or unmounted.

## Flutter host

```sh
cd gui/phonostethos_gui
flutter pub get
flutter analyze
flutter test
flutter run -d linux
```

The GUI connects to the Pi at `10.42.0.3:8080`; configure the host USB Ethernet
interface as `10.42.0.10/24`. The connection is local and does not require an
Internet gateway.

The GUI is a prototype dashboard, not a medical diagnostic instrument.
Audio-device behavior, USB storage hot-plug, GPIO wiring, and acoustic
performance still require on-device validation.
