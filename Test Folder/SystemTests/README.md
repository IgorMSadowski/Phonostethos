# SystemTests

These tests exercise software behavior separately from the standalone peripheral checks in `HardwareTests`. The class-based C++ tests include the project's real classes/helpers rather than a second implementation of the filter, buffers or protocol. Each folder has its own README.

| Folder | Executable / procedure | Checks |
|---|---|---|
| FFTW | FftwTest | Library plan/execution; known 1000 Hz sine at FFT bin 128. |
| DSP | DspTest | Three actual filters, gain/attenuation/limiter, project's Hann-window FFT. |
| Pthreads | PthreadTest | Real circular buffer, ordered samples, EOF/reuse, CommandQueue and DataQueue. |
| Protocol | ProtocolTest | Five-byte commands, endianness, gain codes, fragmented/combined frames, file IDs. |
| FileIO | FileIOTest | Actual FileIO worker/mailbox, recording drain, partial final block, playback and delete. |
| Controller | run.sh | Existing end-to-end server test across Main/AudioDSP/FileIO/Network using ALSA null. |
| GUI | run.sh + manual checklist | Actual Flutter protocol tests, then visible controls/graphs with the Pi. |
| RecordingPlayback | Manual procedure | Real capture, USB recording, playback gain/EOF/Stop and return to live. |
| ErrorHandling | Manual procedure | Missing storage, read-only storage, failed connection, bad device/file and LED interface. |

## Build and run

From the laptop checkout:

```sh
cd "/home/igor/Documents/Phonostethos/Test Folder"
make -C SystemTests
```

This builds the five compiled executables for the Buildroot Pi image. Their locations are `FFTW/build-pi/FftwTest`, `DSP/build-pi/DspTest`, and so on. To build laptop executables instead, install the native development packages listed in HardwareTests/README.md and run:

```sh
make -C SystemTests native
sh SystemTests/run_checks.sh host
```

The Pi package is produced by `sh HardwareTests/package_pi.sh`. After extracting it on the Pi:

```sh
cd /tmp/phonostethos-tests
sh SystemTests/run_checks.sh
```

This runs the five compiled checks in order and stops on failure. It does not start the microphone, write LED masks or mount/eject a USB drive. FileIOTest uses its own temporary `/tmp` directory.

## Complete application and GUI tests

Controller/run.sh and GUI/run.sh run from this full source checkout on the laptop. The controller check needs Python 3 and a native server build. The GUI check needs Flutter and its downloaded dependencies. Follow their READMEs; they do not run inside the Pi binary-only package.

Run the RecordingPlayback and ErrorHandling procedures with the real Pi, its FAT32 drive and the real dashboard after the automatic tests pass. These manual tests are required to assess audible quality, LED wiring, USB removal and the GUI. Numerical filter/FFT checks measure the digital processing separately from the physical audio path.

## Notes when reading the source

- `DspTest` calls DspProcessor from `PhonoStethos_Code/app/src/DspProcessor.cpp`.
- `PthreadTest` uses the actual CircularBuffer/CommandQueue/DataQueue headers.
- `FileIOTest` links the actual FileIoManager source, uses its real worker and control mailbox, and compares all samples.
- `ProtocolTest` uses the actual protocol.h helpers. GUI tests use the actual Dart protocol implementation.
- `FftwTest` tests FFTW directly; `DspTest` separately tests the project's FFT processing, including window/scaling.

FFTW is a plain C library test; the DSP/buffer/protocol/FileIO tests use C++ to call the actual project classes. The source comments explain the test inputs, expected results and external API calls. Production application code is not changed by these tests.
