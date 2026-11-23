# Run the packaged PhonoStethos diagnostics on the Pi

You are in `/tmp/phonostethos-tests`. Each test folder includes its README. These are ARM64 executables for the matching Buildroot image; they are not kernel modules.

1. Finish/eject the current recording from the GUI, then stop the real server before standalone ALSA/LED tests. `/etc/init.d/S90phonostethos stop` stops an installed service; Ctrl+C stops a foreground server.
2. Run `sh SystemTests/run_checks.sh` for the automatic algorithm/protocol/buffer/FileIO checks. They use temporary files and do not require a USB drive or sound device.
3. Run `./HardwareTests/ALSA/AlsaTest list`. The usual capture/output names are `plughw:CARD=HiFiAudio,DEV=0` and `plughw:CARD=Headphones,DEV=0`; use the names for your Pi.
4. Mount your FAT32 pen drive at `/media/usb_storage`, then run `./HardwareTests/USB/UsbTest /media/usb_storage/usb-test.wav`. This creates a new one-second 500 Hz tone and verifies every saved sample.
5. Play that tone: `./HardwareTests/ALSA/AlsaTest play /media/usb_storage/usb-test.wav 'plughw:CARD=Headphones,DEV=0'`.
6. Record your microphone: `./HardwareTests/ALSA/AlsaTest rec /tmp/microphone-test.wav 5 'plughw:CARD=HiFiAudio,DEV=0'`. Play it with the command above using `/tmp/microphone-test.wav`.
7. Read `HardwareTests/LEDs/README.md` to load the matching module and observe its six masks.
8. Read `HardwareTests/Udev/README.md` for the two-terminal insertion/removal check and `HardwareTests/TCP/README.md` for the laptop/Pi TCP check.
9. Run `./HardwareTests/Scheduling/SchedulerTest` as root. It checks four short test threads, not a running server's threads.
10. Restart the real server and use `SystemTests/RecordingPlayback/README.md` and `SystemTests/ErrorHandling/README.md` for complete manual acceptance tests.

A repeated test refuses to replace an existing WAV: use a new name or remove the specific disposable test file. These tools never format a drive or choose a partition for you. A FAT mount check identifies the FAT family; it cannot by itself distinguish FAT32 from FAT16.

Run `echo $?` after a test: 0 = automatic checks passed; 1 = failure; 2 = usage error; 3 = incomplete/inconclusive. Confirm sound and physical LEDs yourself. The controller Python test and Flutter tests run from the full source checkout on the laptop; their launchers are not part of this Pi-only package.
