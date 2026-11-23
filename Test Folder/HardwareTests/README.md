# HardwareTests — start here

These are standalone diagnostic programs for your PhonoStethos Raspberry Pi 4. Each folder contains the source, a Makefile and a README with exact run steps. The original `DeviceDrivers/DD_AlsaTest` is preserved; `ALSA/AlsaTest.c` is a separate, checked version that reads WAV headers with libsndfile and checks partial transfers and errors.

| Folder | What you test | Your observation / automatic result |
|---|---|---|
| ALSA | Microphone capture and headphone output, 8 kHz mono PCM16 | Calls/counts checked automatically; you listen to the file. |
| USB | New WAV creation, sync/finalization, reopen and exact sample comparison on the FAT drive | FAT filesystem and exact readback checked automatically. |
| LEDs | Binary LED masks through `/dev/phonostethos_leds` | Writes checked; you confirm each physical LED. |
| Udev | Existing block devices and USB insertion/removal notifications | Event receipt checked; you identify the USB device nodes. |
| TCP | Connection between laptop and Pi, fragmented command and server reply | Exact bytes checked on both ends. |
| Scheduling | CPU affinity, FIFO priorities and SCHED_OTHER/nice requests | Requested settings are read back and compared. |

`SystemTests`, beside this directory, tests FFTW, the actual DSP/filter/gain implementation, pthreads/buffers, the protocol, FileIO and complete application coordination.

## 1. Compile on the laptop

Use the same Buildroot image/SDK pair as the application:

```sh
cd "/home/igor/Documents/Phonostethos/Test Folder"
make -C HardwareTests
make -C SystemTests
```

Default SDK: `/home/igor/buildroot/buildroot-2026.08`. To use another matching SDK:

```sh
make -C HardwareTests BUILDROOT_DIR=/absolute/path/to/buildroot
make -C SystemTests BUILDROOT_DIR=/absolute/path/to/buildroot
```

Each executable is in its own folder, for example `HardwareTests/ALSA/build-pi/AlsaTest`. You can build just that test with `make -C HardwareTests/ALSA`.

The Pi needs the same ALSA, libsndfile, FFTW3f, libudev and C++ runtime libraries as the server. You do not need a compiler, CMake, Python or alsa-utils on the Pi to run the compiled diagnostic tools.

## 2. Package and copy the Pi tests

```sh
cd "/home/igor/Documents/Phonostethos/Test Folder"
sh HardwareTests/package_pi.sh
scp HardwareTests/phonostethos-tests-pi.tar.gz root@PI_IP:/tmp/
ssh root@PI_IP
```

Replace `PI_IP` with the reachable Pi address, normally `10.42.0.3` on the dedicated link. The packaging script prints what it includes; it does not connect to the Pi or install anything.

On the Pi:

```sh
mkdir -p /tmp/phonostethos-tests
cd /tmp/phonostethos-tests
tar -xzf /tmp/phonostethos-tests-pi.tar.gz
cat README.md
```

The package keeps the same test-folder names but places each executable directly in its folder. Run `./AlsaTest` there; `build-pi/AlsaTest` is its laptop build location.

## 3. Stop the application before standalone audio/LED checks

Finish your recording and use the GUI's Eject before stopping the application. Close the GUI. On the Pi, stop the installed service if present:

```sh
if [ -x /etc/init.d/S90phonostethos ]; then
    /etc/init.d/S90phonostethos stop
fi
```

If you started the server in a foreground terminal, stop it there with Ctrl+C instead. This avoids competing for the audio devices and LED outputs. The service stop may unload the LED module; the LEDs README explains how to reload it for that test. The system's udev USB automounter can still mount a drive independently of this service.

## 4. Suggested order on the Pi

Start from `/tmp/phonostethos-tests`:

```sh
sh SystemTests/run_checks.sh
./HardwareTests/ALSA/AlsaTest list
./HardwareTests/USB/UsbTest /media/usb_storage/usb-test.wav
./HardwareTests/ALSA/AlsaTest play /media/usb_storage/usb-test.wav 'plughw:CARD=Headphones,DEV=0'
./HardwareTests/ALSA/AlsaTest rec /tmp/microphone-test.wav 5 'plughw:CARD=HiFiAudio,DEV=0'
./HardwareTests/ALSA/AlsaTest play /tmp/microphone-test.wav 'plughw:CARD=Headphones,DEV=0'
./HardwareTests/Scheduling/SchedulerTest
```

The USB command requires the FAT drive already mounted at `/media/usb_storage`. It creates a one-second 500 Hz tone without replacing an existing file. The test recording also refuses an existing output path. Change the filename or remove only the disposable test file before repeating.

Then follow the separate LEDs, Udev and TCP READMEs. After the standalone tests, run the complete server/GUI checks in `SystemTests/RecordingPlayback` and `SystemTests/ErrorHandling`.

## 5. Reading results

- Exit **0**: the stated automatic checks passed.
- Exit **1**: a checked operation or expected value failed; read the preceding message.
- Exit **2**: incorrect command arguments.
- Exit **3**: incomplete/inconclusive interactive test, such as no USB event or interrupted LED test.

Use `echo $?` immediately after a command to see its result. A successful ALSA write does not prove audible output; a successful LED write does not prove the LED illuminated. Both require your observation.

## Optional: laptop-only builds

On Ubuntu/Debian, install native development packages if they are missing:

```sh
sudo apt install build-essential pkg-config cmake python3 libasound2-dev libfftw3-dev libsndfile1-dev libudev-dev
make -C HardwareTests native
make -C SystemTests native
sh SystemTests/run_checks.sh host
# Run a laptop ALSA call check with the null PCM (no physical sound):
./HardwareTests/ALSA/build-host/AlsaTest rec /tmp/local-alsa-test.wav 1 null
./HardwareTests/ALSA/build-host/AlsaTest play /tmp/local-alsa-test.wav null
```

Native executables are under `build-host`. They are for the laptop and cannot be copied to the ARM64 Pi. The null-PCM and TCP loopback checks are described in the individual READMEs. They do not verify the actual Pi peripherals.


## Checks completed while preparing these files (23 November 2026)

All six hardware tools and all five compiled software tools built for ARM64 using the project's Buildroot SDK, and also built natively on the laptop. The laptop checks passed for WAV write/readback, existing-file/non-FAT rejection, ALSA null capture/playback and error paths, TCP loopback and fragmented transfer, udev enumeration and LED byte sequencing using a regular file. The five software component checks, complete-server controller test and seven Flutter protocol tests passed.

Physical microphone/headphone quality, the FAT32 pen drive, actual GPIO output, real USB add/remove events, the laptop/Pi cable link and the Pi's scheduling privileges still need the documented target checks. Laptop simulation results do not mark those physical checks as passed.
