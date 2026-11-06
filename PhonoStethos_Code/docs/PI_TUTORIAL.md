# Build and run PhonoStethos on your Raspberry Pi

Use the project’s existing **64-bit Buildroot image and matching SDK**. These commands assume SSH access as root on a development Pi and a writable place such as /tmp. If your Pi runs Raspberry Pi OS or a different kernel, use that system's toolchain and kernel headers instead of copying the Buildroot module.

## 1. Identify the Pi and its sound devices

From the Pi console, run:

```sh
uname -m
uname -r
ip -br addr
cat /proc/asound/cards
arecord -l
aplay -l
```

Expect aarch64 for this SDK. If arecord/aplay are absent, enable alsa-utils in Buildroot for diagnostics. The program defaults to plughw:CARD=HiFiAudio,DEV=0 for capture and plughw:CARD=Headphones,DEV=0 for output. Use the card IDs from your actual Pi. Plug in the audio devices before starting.

## 2. Build on the laptop

Open a terminal in PhonoStethos_Simplified:

```sh
make build
make driver
file app/build-pi/phonostethos-server
```

The outputs are app/build-pi/phonostethos-server, kernel-driver/phonostethos_leds.ko and kernel-driver/build/phonostethos-status-leds.dtbo. The binary needs ALSA, FFTW3f, libsndfile and libudev runtime libraries in the target root filesystem. Use the same SDK/image pair. The LED module must come from the build of the exact running kernel, including its configuration and symbol versions.

## 3. Copy and run a foreground test

On the laptop, replace PI_SSH_IP with the Pi's reachable address:

```sh
scp app/build-pi/phonostethos-server root@PI_SSH_IP:/tmp/
ssh root@PI_SSH_IP
```

On the Pi, stop the old service if installed, then check that the intended server address belongs to the Pi:

```sh
[ ! -x /etc/init.d/S90phonostethos ] || /etc/init.d/S90phonostethos stop
ip -br addr
chmod +x /tmp/phonostethos-server
/tmp/phonostethos-server --help
mkdir -p /media/usb_storage
```

The laptop GUI connects to 10.42.0.3:8080. Configure 10.42.0.3/24 on the Pi's dedicated link and a different address, for example 10.42.0.2/24, on the laptop's link. For a temporary setup, run `ip addr add 10.42.0.3/24 dev YOUR_PI_INTERFACE` on the Pi and the equivalent with 10.42.0.2 on the laptop, using root or sudo. If the address is already configured, keep it. Check that the laptop can reach the Pi before running Flutter.

```sh
/tmp/phonostethos-server --bind 10.42.0.3 --port 8080 \
  --capture-pcm 'plughw:CARD=HiFiAudio,DEV=0' \
  --playback-pcm 'plughw:CARD=Headphones,DEV=0' \
  --storage /media/usb_storage
```

Replace the two ALSA names where needed. Keep the terminal open for errors. Missing LED support can be diagnosed separately. If bind fails, check the address and whether the old service still owns port 8080. If a library is missing, rebuild the target image with that runtime library. If an executable exists but reports “not found,” also check its ELF interpreter and architecture.

## 4. Test the USB and GUI

Use a spare FAT32 USB drive. Confirm the partition with `lsblk` if available, or compare `cat /proc/partitions` before and after insertion. The deployed USB rule mounts the supported partition at /media/usb_storage. For a manual test if no automounter is active, mount the identified partition using `mount -t vfat /dev/sdX1 /media/usb_storage` and verify the mount in /proc/mounts. Do not use the development flag --allow-unmounted-storage for the USB acceptance test.

On the laptop, from the project folder:

```sh
cd gui/phonostethos_gui
flutter doctor
flutter pub get
flutter run -d linux
```

Use this order to isolate problems:

1. Listen on headphones and check the waveform. Change mode and gain.
2. Without storage, verify that Record and Play are disabled.
3. Insert the test drive. Record several seconds, press Stop, wait for completion, and open the archive.
4. Play the file to EOF and verify return to live capture. Repeat and press Stop Playback. Delete a disposable recording.
5. Close and reopen the dashboard. Verify that it receives the Pi's current state.
6. Use Eject and wait for confirmation. Verify /media/usb_storage disappeared from /proc/mounts before pulling the drive.
7. Check the startup log for failed affinity or scheduling calls. On a system with full procps, `ps -L -p PID -o tid,psr,cls,rtprio,ni,comm` shows threads. With BusyBox, inspect each thread under /proc/PID/task and use chrt/taskset if supplied.

End the foreground test with Ctrl+C after ejecting. Ejection protects the USB volume. Use the Pi's normal shutdown procedure while the boot/root filesystem's power-loss behavior is still unverified.

## 5. Enable LEDs and install the service

First locate the firmware boot partition containing config.txt. On an image mounted on the laptop, that is the firmware partition's root. Install phonostethos-status-leds.dtbo into its overlays directory and add this line under the applicable firmware section:

```ini
dtoverlay=phonostethos-status-leds
```

Put the matching phonostethos_leds.ko at /lib/modules/<kernel-release>/extra/phonostethos_leds.ko on the Pi root filesystem, then reboot to apply the overlay. Run `insmod /lib/modules/$(uname -r)/extra/phonostethos_leds.ko` if the service has not already loaded it. Check `dmesg | tail` and `ls -l /dev/phonostethos_leds`. Test mode LEDs and the recording LED from the GUI. A successful device write cannot detect an unplugged LED.

For a repeatable Buildroot image, create your own staging directories and run from the laptop:

```sh
make install OVERLAY_DIR="$PWD/pi-rootfs-stage" BOOT_DIR="$PWD/pi-boot-stage"
```

The root staging directory contains the server, matching module, udev rules, USB helper and /etc/init.d/S90phonostethos. Merge those files into the Buildroot rootfs overlay or image through your board configuration. Include kernel-driver/deploy/users.txt in BR2_ROOTFS_USERS_TABLE so udev can assign group phonostethos. Merge the DTBO into the boot image and retain the config.txt line in the board’s boot configuration. Configure the Ethernet address before S90 starts, and edit the service command if the Pi needs custom ALSA device names.

Once the image is booted, use `/etc/init.d/S90phonostethos start` and `stop` on the Pi. The script starts the server in the background and saves its PID. Use the foreground procedure whenever startup output is needed.
