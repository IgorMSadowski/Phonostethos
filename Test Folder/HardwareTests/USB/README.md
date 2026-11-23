# USB / libsndfile test

Files: `UsbTest.c` and `Makefile`. Build with `make`; the Pi executable is `build-pi/UsbTest`.

The test requires a FAT filesystem by default, creates a NEW mono 8 kHz PCM16 WAV, writes one second of a 500 Hz tone, checks sync/close, reopens it and compares all 8000 samples. It keeps the file for the headphone test. It does not format, mount or eject the drive.

On the Pi, identify and mount your FAT32 pen drive at /media/usb_storage using your configured automounter or the existing deployment instructions. Check the mount first:

```sh
mount | grep '/media/usb_storage'
cd /tmp/phonostethos-tests/HardwareTests/USB
./UsbTest /media/usb_storage/usb-test.wav
echo $?
```

Expected: FAT mount detected, then PASS with 8000 samples written/read exactly. No file is overwritten: use another disposable filename on a repeat. The FAT magic value identifies the FAT family, so the check alone does not distinguish FAT32 from FAT16. Use the drive you prepared as FAT32.

Listen to the test file:

```sh
../ALSA/AlsaTest play /media/usb_storage/usb-test.wav 'plughw:CARD=Headphones,DEV=0'
```

Expected: a one-second 500 Hz tone. Eject/unmount the drive before removal. With the real application stopped and no process using the volume, `umount /media/usb_storage` unmounts it. With the application running, use its GUI Eject and wait for completion.

Main APIs: statfs checks the parent filesystem; open with O_EXCL refuses an existing path; sf_open_fd writes WAV metadata; sf_write_short/read_short return sample counts; sf_write_sync and sf_close check writing/finalization; fsync checks synchronization of the finalized file.

Laptop-only library check: `make native`, then `./build-host/UsbTest /tmp/usb-library-test.wav --local`. LOCAL MODE explicitly skips the FAT/USB requirement. It does not prove the physical pen drive works.

A read-only/full/unmounted drive should fail rather than print PASS. Error output explains whether creation, writing, finalizing or readback failed. This test does not prove recovery after power loss.

API reference: https://libsndfile.github.io/libsndfile/api.html
