# Report and code alignment

DESIGNTOEDIT and the handwritten source interfaces were reconciled on 23 November
2026. The Google Slides presentation was read for comparison and deliberately not edited.

## Implemented design

| Area | Implementation |
|---|---|
| Ownership | Main directly owns the audio, DSP, FileIO, network and LED objects. Workers share the same two buffers/queues by reference. |
| Threads | Main is the original thread on Core 0; AudioDSP, FileIO and Network are pthread workers on Cores 1, 2 and 3. Main/Network request SCHED_OTHER 0 and nice 0; AudioDSP/FileIO request FIFO 80/50. |
| Worker control | AudioEngine and FileIoManager each own one request struct, occupied flag and mutex. FileIO additionally has a condition variable for idle wakeups. |
| Results | MainMessage with GUI_COMMAND, WORKER_RESULT or WORKER_ERROR travels through CommandQueue to Main. Operation IDs correlate deferred results. |
| Replies | Main queues serialized frames in ControlReplyQueue; only Network writes TCP. Replies are taken before the latest DataQueue snapshot. |
| Audio buffers | AudioDSP pushes recording and pops playback. FileIO pops recording and pushes playback. Each buffer contains 4096 PCM16 samples and owns its lock/gate. |
| DSP | 8 kHz mono, 512-sample blocks, fourth-order Butterworth HPF plus fourth-order LPF (four biquads in the complete cascade), gain ramp and PCM16 saturation. |
| FFT | 1024-sample rolling Hann window, 512-sample hop, 513 complex FFTW outputs, 512 transmitted magnitudes. GUI smoothing and decaying peaks affect display only. |
| LEDs | Main computes one complete mask. LedDriver exposes openDriver, closeDriver and checked writeByte; the kernel maps bits to GPIO17/27/24/22. No pulse or physical-feedback API. |
| Storage | FAT32/vfat pen drive at /media/usb_storage. Exclusive new-file creation prevents overwrite. Filename collision fails the attempt; another Start Record generates the next candidate. |
| Finalization | Recording stop checks libsndfile flush/error/close and descriptor close. Safe eject additionally checks syncfs and ordinary unmount before reporting safe removal. |
| GUI send | _request awaits socket.flush and handles socket/closed-stream failure; server execution is acknowledged only by the final CommandResult. |

## Matching names and order

`DESIGN_CODE_MAP.md` lists the actual public declarations in header order.
`Design_Code_Alignment/ClassDiagram_CodeAligned.drawio` provides an editable complete
view, one tab per class and a messages/protocol tab. The class diagram and Section
3.4.2 use the same principal method order as the source. Lifecycle/support methods
are described separately from the command/audio/storage operations.

## Slides to update later

- Replace the old class-diagram images with the code-aligned Draw.io version.
- Pass the file ID to startPlayback and the cursor to listFiles.
- Use the bool return values for suspendCapture, resumeCapture, writeByte and queue enqueue operations.
- Show CommandType, cmd_type, StatusPacket, FileListPacket and DataPacket with the implemented names.
- Show the direct-owned objects and the single transport enum rather than pointer members and redundant playback booleans.
- Remove old setModeLed, setRecordLed, triggerErrorPulse and handleUsbEvent methods; Main builds the full mask and checkUsbEvents handles udev dispatch.
- Show DataQueue's direct latest-snapshot storage and no condition variable.
- Keep the GUI send decision distinct from server command completion.

## Verification limits

Host tests exercise protocol, synchronization, DSP, WAV operations and controller
TCP paths using temporary files/ALSA null. ARM compilation checks the target toolchain.
Neither proves physical microphone/headphone timing, GPIO wiring, FAT32 hotplug or
the deployed kernel permissions. Run the existing HardwareTests/SystemTests on the Pi.
