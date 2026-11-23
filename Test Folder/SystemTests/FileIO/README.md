# FileIO worker / recording and playback buffers test

Files: `FileIOTest.cpp` and `Makefile`. This links the real FileIoManager source and real buffers/mailbox. Build with `make`; output: `build-pi/FileIOTest`.

On the packaged Pi:

```sh
cd /tmp/phonostethos-tests/SystemTests/FileIO
./FileIOTest
```

Expected: three PASS lines. The test creates a private directory under /tmp and deletes its test file/directory after a successful run. It does not touch your recordings or require the USB drive.

Recording trace: post OPEN_RECORD through the worker mailbox -> wait for OPEN_READY -> push 1041 known samples into recordingBuffer -> close its producer gate -> post CLOSE_RECORD -> wait for RECORD_CLOSED -> independently reopen the WAV and compare every sample.

1041 = 512 + 512 + 17. This deliberately checks that the final partial block is saved when stopping.

Playback trace: resolve the saved file's identity -> post PREPARE_PLAYBACK -> FileIO reads WAV and pushes playbackBuffer -> test pops as AudioDSP would -> compare all 1041 samples and observe EOF -> post STOP_PLAYBACK and wait for PLAYBACK_CLOSED.

Finally it asks the worker to delete the same validated file, stops/joins the worker and removes its private directory. It checks storage workflow and thread communication, not headphone output. Use HardwareTests/USB for the actual FAT drive and the complete-system procedure for sound.

A scheduling warning on an unprivileged laptop is separate from sample correctness. Check requested priorities on the Pi with HardwareTests/Scheduling.
