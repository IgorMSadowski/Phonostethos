# Simple, reproducible error tests

These deliberately cause ordinary failures with disposable files and a test drive. Check the printed error / GUI response and confirm that unrelated functions remain usable. Exact acoustic quality is assessed separately.

| Case | How to reproduce | Expected |
|---|---|---|
| Missing USB | Start/keep the system without the pen drive. | Live audio can continue; storage-dependent GUI actions are disabled. |
| Read-only storage | While idle, with no recording/playback/file operation active, use `mount -o remount,ro /media/usb_storage` on the test drive; wait for status refresh. Restore with `mount -o remount,rw /media/usb_storage` afterwards. | Recording is unavailable; the USB test's write fails on read-only media. Storage status recovers after writable access is restored. |
| Missing capture device | With the real server stopped, run `AlsaTest rec /tmp/should-not-record.wav 1 nonexistent-pcm-for-test`. | Nonzero exit and an ALSA device-open error; no PASS. |
| Invalid audio file | Create `/tmp/invalid-audio-test.wav` containing ordinary text, then run AlsaTest play on it. | WAV-open/format failure; it is not sent as audio samples. |
| Existing output filename | Run UsbTest twice with the same disposable filename. | First succeeds, second refuses to overwrite the existing file. |
| Unmounted USB path | After safely ejecting/unmounting, run UsbTest with a new filename under /media/usb_storage. | FAT check fails; it does not silently write onto the root filesystem instead. |
| Missing LED device | Run `LedTest /dev/phonostethos_missing_for_test`. | Device-open failure. A write API cannot detect a physically disconnected LED. |
| Link loss / recovery | Temporarily disconnect the laptop/Pi network link while the system is idle; reconnect it. | GUI displays disconnection and reconnects to the Pi's current state. |
| Delete cancelled | Choose a disposable file, request delete, then Cancel. | File remains; no deletion is performed. |

An invalid WAV fixture can be created on the Pi with:

```sh
printf 'This is a test, not audio.\n' > /tmp/invalid-audio-test.wav
/tmp/phonostethos-tests/HardwareTests/ALSA/AlsaTest play /tmp/invalid-audio-test.wav 'plughw:CARD=Headphones,DEV=0'
echo $?
```

The automatic Controller test also checks invalid gain arguments and stale file IDs; Protocol/GUI tests check malformed frames. Full-disk, in-progress device removal and deadline/performance testing are not claimed as covered by the simple checks above.

After a manual error test, restore the writable mount/network/device configuration and repeat a normal five-second recording/playback. This confirms recovery instead of checking only that an error message appeared.
