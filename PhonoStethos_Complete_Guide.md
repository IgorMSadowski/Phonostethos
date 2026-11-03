# PhonoStethos: complete beginner's guide to the program

This guide describes the checked-in source code in this workspace, not only the
intended design. It explains the C++ Raspberry Pi server, the Flutter/Dart host
application, the Linux LED driver, important types and variables, and how the
implementation compares with the formal report and presentation.

The project is a prototype, not a medical diagnostic device. The reports also
identify hardware checks and performance measurements that have not yet been
established. A feature shown in a design diagram is not automatically implemented
in the current source.

## 1. What the whole program does

The system has three main running parts:

1. **The Pi server**, written in C++, captures mono audio through ALSA, filters
   and amplifies it, sends it to the audio output, makes waveform/spectrum data,
   optionally records WAV files, and accepts commands over TCP.
2. **The host dashboard**, written in Dart using Flutter, connects to the Pi,
   displays telemetry, and sends mode, gain, recording, archive, playback, delete,
   and storage-eject commands.
3. **The Linux LED kernel driver**, written in C, exposes a character device
   named `/dev/phonostethos_leds`. The C++ server writes a one-byte light mask to
   it.

The main audio path is:

```text
microphone / USB audio capture
    -> ALSA capture
    -> C++ AudioEngine
    -> DspProcessor filter and gain
    -> ALSA headphone playback
    -> waveform and FFT telemetry
    -> TCP
    -> Flutter charts
```

During recording, processed PCM16 audio blocks are also queued for a file worker
and written as WAV. During playback, a file-reading thread supplies blocks to the
audio thread, which sends them to the ALSA output.

The default server address is `10.42.0.3:8080`, and the default external-storage
directory is `/media/usb_storage`. These are defaults; the server has command-line
options to override them.

## 2. Beginner vocabulary

### Source files, headers, and compilation

- A **`.h` header** declares types and functions so multiple `.cpp` files can use
  the same interface.
- A **`.cpp` source file** normally contains the function bodies that perform
  the work.
- `#include` inserts declarations from another file before compilation.
- The **compiler** turns a source file into machine-code object files. The
  **linker** combines object files and libraries into an executable.
- `#pragma once` tells the compiler to include a header only once in a
  translation unit.
- The CMake file defines the C++ standard, executable, dependencies, and tests.

### Namespaces

A **namespace** groups names to avoid collisions. For example,
`phono_app::AudioEngine` means the `AudioEngine` declared in `phono_app`.
`std` is the C++ standard-library namespace, containing `string`, `vector`,
`mutex`, `thread`, and other standard types.

Many current C++ files contain `using namespace std;`, so they can write
`string` rather than `std::string`. This also imports standard-library names
into the current scope; in a header it affects files that include that header,
too. `using namespace phono_app;` in a `.cpp` has the analogous effect for the
application namespace.

### Types and values

- A **type** says what kind of value something can hold and what operations it
  supports. Examples: `int16_t` is a signed 16-bit integer, `string` is text,
  `vector<uint8_t>` is a resizable list of bytes, and `filesystem::path` is a
  filesystem path.
- A **variable** is a named value. A **member variable** belongs to an object;
  a **parameter** is a value supplied to a function.
- A **struct** groups named data fields together. A **class** groups data and
  functions and can control which are public or private.
- An **enum** gives a name to a set of related integral choices. `enum class`
  is the scoped, strongly typed form. For example,
  `FilterMode::Heart` cannot be confused with an arbitrary integer without an
  explicit conversion. The `::` here selects a named value inside a scope; it
  is not related to `std::`.
- `const` means code may not change that value through that name.
- `&` in a function parameter means a **reference**: the function uses an
  existing object instead of making a copy. `const T&` is a read-only reference.
- `*` in a pointer type means the value can refer to an object or be null.
- `std::atomic<T>` is a value intended to be read and written safely by more
  than one thread. It protects that one value; it does not automatically make a
  group of values a single consistent snapshot.
- A **mutex** is a lock that prevents simultaneous access to a protected region.
  A **condition variable** lets a thread sleep until a condition may have
  changed.
- A **template** such as `CircularBuffer<T, Capacity>` is a type/function
  recipe that is specialized for a particular data type and capacity.
- `optional<T>` means a result may contain a `T` or may be empty.
- `promise<T>` and its matching future let one thread provide a result that
  another thread waits for.
- A **file descriptor** is an integer handle returned by Linux when a file,
  socket, or device is opened. It is not a GPIO number.
- A **frame** is one complete message in the TCP application protocol. TCP
  itself is a byte stream, so one `recv` call is not guaranteed to return one
  complete frame.
- **PCM16** means signed 16-bit pulse-code-modulated audio samples.
- **FFT** means Fast Fourier Transform: it calculates an estimate of which
  frequencies appear in a block of samples. In this application FFT is for
  visualization; the IIR filters perform the audio filtering.

## 3. Project map

The principal application files are under `PhonoStethos_Code/`.

| Location | Role |
|---|---|
| `app/include/phonostethos/` | C++ declarations and reusable template/header implementations |
| `app/src/` | C++ server implementation |
| `app/tests/` | Native C++ tests |
| `gui/phonostethos_gui/lib/` | Flutter screen and Dart protocol implementation |
| `gui/phonostethos_gui/test/` | Dart protocol tests |
| `kernel-driver/phonostethos_leds.c` | Handwritten Linux platform/character driver |
| `kernel-driver/phonostethos_leds.mod.c` | Kernel-build-generated module metadata; not the driver's source of behavior |
| `app/CMakeLists.txt` | CMake definition for the server and native tests |
| `Makefile` | Buildroot-oriented build, kernel module, overlay, and install tasks |

Generated Flutter Linux files under `gui/phonostethos_gui/linux/` connect
Flutter/Dart to the GTK Linux window. They are platform bootstrap glue rather
than the dashboard's business logic.

## 4. Shared C++ data types and constants

### `RuntimeTypes.h`

This header contains application-wide audio, storage, and state definitions.

- `kSampleRate`: `8000` samples per second.
- `kPeriodFrames`: `512` samples per audio block. At 8 kHz, one block spans
  64 ms; that is the block duration, not a measured end-to-end delay.
- `kFftFrames`: `1024` samples in the rolling waveform/FFT window (128 ms).
- `kFftBins`: `512` spectrum values transmitted to the client. FFTW's real
  transform has 513 output bins; the implementation sends indices 0 through
  511 and omits the Nyquist bin.
- `kCircularBufferSamples`: `4096`, the fixed sample capacity of each shared
  recording/playback FIFO.
- `kPi`: the numeric value of pi.
- `kLedDevice`: default device path `/dev/phonostethos_leds`.
- `running`: an `atomic<bool>` global set false during signal-driven shutdown.

`FilterMode : unsigned` has `General = 0`, `Heart = 1`, and `Lung = 2`. The
explicit underlying type is unsigned integer. The names make mode values safer
and easier to understand than scattered magic numbers.

`Options` contains configuration:

- `bind_address`: TCP IPv4 address, default `10.42.0.3`.
- `port`: TCP port, default `8080`.
- `capture_pcm` and `playback_pcm`: ALSA PCM device names, both default
  `default`.
- `storage`: external drive mount path, default `/media/usb_storage`.
- `led_device`: device-node path, default `kLedDevice`.
- `require_mounted_storage`: if true, storage must actually be mounted for
  storage operations.

`storage_ready(options)` checks that the configured path is a directory. When
`require_mounted_storage` is true, it canonicalizes that path and checks
`/proc/self/mountinfo` for an exact mounted path.

`Telemetry` has two fixed-size arrays:

- `waveform`: 1024 floats of processed, normalized audio history.
- `spectrum`: 512 floats of non-negative FFT magnitudes.

`ArchiveIdentity` is the server's record of a file offered to a client:

- `basename`: filename only, not a client-provided path.
- `device`, `inode`, `size`, and `modified`: filesystem identity details.

`same_file(identity, metadata)` checks that the path still refers to a regular
file with matching device, inode, size, and modification time. This helps reject
stale archive IDs if a file is replaced or changed.

### `protocol.h`

This header is shared protocol code and is in namespace `phono::protocol`.
Integers in the wire format are big-endian (network byte order). Float values
are IEEE-754 32-bit floats whose bits are sent big-endian. The wire is built
field by field; it does not depend on compiler-specific struct packing.

Message type constants:

- `kData = 0x01`: waveform and spectrum.
- `kStatus = 0x02`: fixed five-byte capability/status message.
- `kFileList = 0x03`: paginated recording list.
- `kState = 0x04`: fixed 20-byte state payload.
- `kCommandResult = 0x05`: fixed eight-byte result payload.

Size and limit constants:

- `kHeaderBytes = 5`: one type byte and a four-byte payload length.
- `kCommandBytes = 5`: one command byte and a four-byte command value.
- `kWaveformSamples = 1024`, `kSpectrumSamples = 512`.
- `kDataPayloadBytes`: `(1024 + 512) * sizeof(float)`, or 6144 bytes.
- `kStatusFrameBytes = 5`, `kStatePayloadBytes = 20`,
  `kCommandResultPayloadBytes = 8`.
- `kMaxPayloadBytes = 16384`.
- `kMaxFileListPayloadBytes = 8192`, `kMaxFileListEntries = 32`,
  `kMaxBasenameBytes = 128`.
- `kProtocolVersion = 1`.

Protocol enums:

- `Command : uint8_t`: `SetMode=1`, `SetGain=2`, `StartRecording=3`,
  `StopRecording=4`, `PlayAudio=5`, `StopPlayback=6`, `EjectStorage=7`,
  `GetArchive=8`, `DeleteRecording=9`.
- `Transport : uint8_t`: `Live=0`, `Recording=1`, `Playback=2`,
  `Stopping=3`, `Ejecting=4`, `Error=5`.
- `Result : uint8_t`: `Completed=0`, `AcceptedPending=1`, `Rejected=2`,
  `Failed=3`.
- `Error : uint8_t`: `None=0`, `DiskFull=1`, `StorageLost=2`, `MicLost=3`,
  `OpenError=4`, `WriteError=5`, `FinalizeError=6`, `ReadError=7`,
  `UnsupportedWav=8`, `RecordOverrun=9`, `NetworkError=10`, `DspError=11`,
  `PowerFault=12`, `UnmountError=13`, `ProtocolError=14`, `OutputError=15`,
  `Busy=16`, `InvalidArgument=17`, `StaleFileId=18`,
  `LedDriverError=19`.
- `StorageFlag` is a bit mask: `StoragePresent=1`, `StorageMounted=2`,
  `StorageReadable=4`, `StorageWritable=8`, and `StorageSpaceOk=16`. A bit is
  enabled by bitwise OR (`|`).

Protocol records:

- `CommandPacket`: `type` is the command enum; `value` is its 32-bit argument.
- `Status`: `storage_mounted`, `allow_record`, `allow_playback`, `error_code`
  are one-byte values.
- `State`: transport, selected filter, gain, storage bit flags, error, active
  recording session ID, dropped telemetry count, and recording-overrun count.
- `CommandResult`: which command completed/rejected/failed, its `Result`, and a
  32-bit detail value (normally an error code).
- `FileEntry`: server-issued numeric ID and archive basename.
- `FileList`: next-page cursor and current page of entries.
- `Frame`: frame type byte and decoded payload byte vector.

Protocol helper functions:

- `read_u32_be` and `read_u16_be` read unsigned integers from big-endian bytes.
- `append_u16_be` and `append_u32_be` append those integer representations.
- `read_float_be` reconstructs a 32-bit float from its bits and rejects builds
  whose `float` is not IEEE-754 binary32.
- `append_float_be` serializes a float's bit pattern, not a numeric float-to-int
  conversion.
- `make_command_packet` creates a five-byte client command.
- `decode_command` verifies the exact command length and command ID range, then
  fills a `CommandPacket`.
- `valid_command_value` checks mode 0–2, gain 1–10, zero-valued no-argument
  commands, and accepts the ID/cursor arguments used by play/archive/delete.
- `make_status_packet` makes the special fixed-size five-byte status message.
- `make_frame` prefixes a non-status payload with type and big-endian length and
  rejects status frames and oversized payloads.
- `make_state_packet` writes version, transport, filter, gain, storage flags,
  error, two reserved zero bytes, and the three 32-bit counters/IDs.
- `make_command_result_packet` writes command, result, two reserved zero bytes,
  and detail.
- `make_file_list_packet` validates each archive name and ID, writes
  `next_cursor=<number>` and tab-separated ID/name records, then frames them.
- `valid_frame_payload` checks the allowed payload size for each known framed
  message type.
- `valid_utf8` checks UTF-8 byte sequences and rejects overlong encodings,
  surrogate values, invalid continuation bytes, and out-of-range code points.
- `FrameDecoder::append` accumulates partial TCP input and enforces a 1 MiB
  receive-buffer cap.
- `FrameDecoder::next` extracts one whole frame if enough bytes are present;
  it retains incomplete input, handles the special status frame, and marks
  malformed type/length combinations failed.
- `FrameDecoder::failed` reports that permanent failed state; `buffered_bytes`
  reports retained partial bytes.
- `parse_file_list` validates UTF-8, cursor, IDs, delimiters, and page limits,
  then fills a `FileList`.
- `decode_state` verifies version, enum/range values, reserved bytes and exact
  payload size before filling `State`.
- `decode_command_result` validates command/result ranges and reserved bytes
  before filling `CommandResult`.
- `make_data_packet` verifies all waveform/spectrum values are finite and
  magnitudes are non-negative, serializes floats, then builds a data frame.

### `CommandQueue.h` and `DataQueue.h`

`MainMessage` is the common message exchanged between Main, Network, AudioDSP,
and FileIO. `kind` distinguishes decoded GUI commands, normal worker results,
and worker errors. `command` contains a decoded five-byte command; the
`operation_id` correlates asynchronous results; `result_code`, `error_code`,
and `storage_flags` carry completion/status information; `archive` carries
FileIO's archive snapshot.

`WorkerResultCode` names events such as `AudioReady`, `RecordingOpened`,
`PlaybackFinished`, `ArchiveReady`, and `StorageChanged`. These are internal
thread messages, not new wire-protocol command IDs.

`CommandQueue` is an unbounded FIFO of `MainMessage` values. A mutex protects
its deque, and a condition variable wakes Main when a producer enqueues a
message. `dequeue()` waits for at most 10 ms so Main can also check udev.
`close()` rejects future messages and wakes waiters. Network puts decoded
commands on it; AudioDSP and FileIO put their results/errors on the same queue.

`DataQueue` is deliberately separate: it holds zero or one `Telemetry`
snapshot. AudioDSP uses a try-lock enqueue, replacing an older waiting snapshot
so slow network transmission cannot make the real-time audio loop wait.
Network dequeues the newest snapshot only when no control frame is waiting.
Disconnect clears the snapshot and disables enqueueing until another client
connects.

`ControlOutputQueue` is in `NetworkManager.h`; it is a separate FIFO for status,
state, command-result, and archive frames. Network always services these
control frames before optional telemetry. `AudioEngine` and `FileIOManager`
also each have a separate one-request mailbox; these are command handoff
mechanisms, not additional sample queues.

### `CircularBuffer.h`

`CircularBuffer<T, Capacity>` is a synchronized bounded FIFO; it does **not**
overwrite accepted samples. The controller owns two distinct instances, each
with 4096 PCM16 slots: the recording FIFO (AudioDSP producer, FileIO consumer)
and playback FIFO (FileIO producer, AudioDSP consumer). `mutex_` protects the
array, indices, count, and producer/stop gates; `data_available_` lets a
consumer wait for samples or stop.

- `begin_session()` opens the producer gate only when the old session is empty.
- `push(data, count)` inserts a whole block or rejects it when stopped/full;
  rejection is visible to the caller, allowing recording overrun reporting.
- `pop(destination, maximum, timeout)` waits up to the timeout and returns the
  number of actual samples removed, including a final partial block.
- `stop_producing()` closes the producer gate and wakes a waiting consumer;
  queued samples can still be drained.
- `clear()` discards remaining samples and leaves the FIFO stopped.
- `size()`, `isEmpty()`, `isFull()`, `available_capacity()`, and `stopped()`
  expose synchronized state.

The FFT history is a separate private array inside `DspProcessor`; it does not
use this cross-thread FIFO.

## 5. C++ headers: class interfaces and stored state

### `BandPassFilter.h`

`BiquadFilter` implements a second-order IIR section. The stored coefficients
`b0_`, `b1_`, `b2_`, `a1_`, `a2_` define the recurrence; `x1_`, `x2_` retain the
two previous inputs and `y1_`, `y2_` the two previous outputs.

- `configure_high_pass(cutoff, sample_rate)` configures a high-pass section.
- `configure_low_pass(cutoff, sample_rate)` configures a low-pass section.
- Private `configure(cutoff, sample_rate, high_pass)` calculates coefficients
  using a Butterworth Q of approximately 0.7071, angular frequency, sine,
  cosine, and normalized `a0`.
- `reset()` zeros the previous input/output history.
- `process(input)` evaluates the filter equation, shifts history, and returns
  the new output.

`BandPassFilter` owns `high_pass_` and `low_pass_`. `configure(low, high, rate)`
sets the high-pass cutoff to `low` and low-pass cutoff to `high`; `reset()`
resets both; `process(input)` passes the value through both sections in order.
The low/high endpoints used by the app are General 20–2000 Hz, Heart 20–600 Hz,
and Lung 100–1000 Hz.

### `DspProcessor.h`

`DspProcessor` owns:

- `history_`: 1024-sample circular history.
- `fft_input_`: windowed real FFT input.
- `waveform_`: ordered unwindowed sample history sent to the GUI.
- `fft_output_`: FFTW's complex output allocation (513 values).
- `fft_plan_`: FFTW's reusable real-to-complex plan; null until initialized.
- `filter_`: the live band-pass filter.
- `configured_mode_`: last mode applied; initialized to sentinel value 99 to
  force initial filter configuration.
- `frames_since_fft_`: count since the last telemetry analysis.

Functions are `initialize()` (create FFTW plan), destructor (destroy plan),
`reset()` (clear history/filter and force reconfiguration), `process_live(...)`
(filter and gain a PCM16 block), `analyze(...)` (add samples and optionally
return telemetry), and private `filter_limits(mode)` (map mode number to low/high
cutoff pair). Copy construction and assignment are deleted because an FFTW plan
is a managed resource.

### `AudioEngine.h`

`AudioEngine` owns the AudioDSP worker. The constructor receives shared atomics
for `mode_`, `gain_`, `recording_`, and `error_code_`; references to the common
`CommandQueue` and telemetry `DataQueue`; the configuration mutex; references
to the separate recording and playback `CircularBuffer`s; and counters for
dropped telemetry and recording overruns.

Its single-slot `request_` mailbox carries `SuspendCapture`, `ResumeCapture`,
`StartPlayback`, or `StopPlayback` plus an operation ID. `request_mutex_`
protects the optional request, and the worker checks it on its loop. `ready_`
and `stop_requested_` are atomics. `capture_pcm_` lets `stop()` interrupt a
blocking ALSA capture, while `capture_mutex_` protects its close path. `dsp_`
and `worker_` are private worker-owned state.

The public constructor starts the worker; destructor calls `stop()`;
`request()` offers one command if the mailbox is empty; `ready()` reports
successful ALSA/DSP startup; and `stop()` interrupts and joins the worker.
Private `configure_pcm`, `recover_pcm`, and `set_audio_thread_policy` configure
the ALSA stream, recover underruns/suspends, and request core 1/SCHED_FIFO 80.
`apply_request` performs the mailbox command on the audio thread; `run()` owns
both ALSA handles, routes live/playback samples, records to the shared FIFO,
and offers telemetry without waiting for the network.

### `FileIOManager.h`

`FileIOManager::RequestKind` includes `OpenRecording`, `CloseRecording`,
`PreparePlayback`, `StopPlayback`, `List`, `Delete`, `Eject`, `Mount`,
`DeviceRemoved`, and `RefreshStorage`. A `Request` contains its kind, operation
ID, path/filesystem type, or the identity of an archive file as appropriate.
Its optional one-request mailbox is guarded by `mailbox_mutex_`; the worker
wakes on `mailbox_ready_`. The worker exclusively owns both libsndfile handles
and their file descriptors, plus the active recording/playback state.

`request()` rejects an occupied mailbox; `stop()` stops both shared buffers,
wakes the worker, and joins it. `run()` waits in 10 ms intervals, dispatches
requests, drains recording samples, refills playback samples, and closes all
remaining handles at shutdown. `write_recording()` writes up to 512 PCM16
samples and records storage/write failures; `read_playback()` fills only
available playback-FIFO capacity. `post_storage_status()` computes presence,
mount, access, and free-space flags. Mount/eject/list/delete and safe removal
handling also run on this worker, so Main never performs file or mount I/O.

### `LedDriver.h`

`LedDriver` wraps the Linux device file. `path_` is the path, `fd_` is its open
file descriptor (`-1` means closed), and `available_` records whether `open()`
has succeeded.

- Constructor stores the requested path; destructor calls `close()`.
- `open()` opens write-only with close-on-exec.
- `write(command)` writes exactly one byte and reports failures.
- `set_mode(mode, recording)` constructs one complete bit mask for the selected
  mode and recording indicator.
- `close()` writes zero before closing the descriptor.

LED mask bits are Heart `0x01`, Lung `0x02`, General `0x04`, Recording `0x08`.
The driver also reserves `0xff` for the short error flash.

### `SystemController.h`

`SystemController` is the Main coordinator and owns authoritative transport,
mode, gain, recording, error, and active-session state. It constructs
`CommandQueue`, `DataQueue`, `ControlOutputQueue`, `LedDriver`, `FileIOManager`,
and `NetworkManager`; it creates `AudioEngine` during `initSystem()`. The
recording/playback FIFOs and configuration mutex are shared with the audio and
file workers. `archive_by_id_`, `archive_id_by_name_`, and `archive_entries_`
translate connection-local archive IDs to safe file identities. The `next_*`
fields generate session, operation, and archive IDs. `pending_` tracks one
asynchronous operation and collects its worker completions; `storage_flags_`
and `audio_ready_` drive reported capabilities.

`initSystem()` initializes udev, LEDs, audio, and networking. `run()` is the
Main-thread loop: it dequeues messages with a 10 ms timeout, checks USB events,
submits deferred storage work, and periodically refreshes storage status.
`process_command()` validates and routes each command; `handle_worker_result()`
advances the operation state machine; `complete_operation()` publishes the
final result and updated state. `publish_status()`, `publish_state()`, and
`publish_command_result()` put frames on Network's control FIFO. `update_leds()`
is the only place that commits the complete mode/recording mask.
`init_udev_monitor()`, `check_usb_events()`, and
`dispatch_deferred_mount()` keep device monitoring and mount requests on Main
while leaving the actual mount/file work to FileIO. `stop()` joins workers and
releases resources.

### `NetworkManager.h`

`ControlOutputQueue` is a mutex-protected FIFO of already-encoded control
frames. `NetworkManager` owns a dedicated worker thread and references the
`CommandQueue`, telemetry `DataQueue`, and control-output FIFO. `start()` and
`stop()` start/join the worker; `connected()` reports connection state; and
`queue_control()` posts a frame for the network thread to send. Private `run()`
owns the listening socket, while `serve_client()` handles a single connected
client. `send_all()` enforces the frame-send deadline, and
`set_network_thread_policy()` requests core 3 with SCHED_OTHER.

## 6. C++ implementation files and function behavior

### `DspProcessor.cpp`

- The destructor destroys `fft_plan_` only if it exists.
- `initialize()` returns immediately if a plan already exists; otherwise it
  asks FFTW for a 1024-point real-to-complex transform.
- `reset()` clears the rolling history and filter, resets the FFT interval, and
  sets the mode sentinel so the next live block configures the filter.
- `process_live(input, output, count, mode, gain)` reconfigures only when mode
  changes. Each input sample passes through the IIR filter, is multiplied by
  integer gain in floating point, clamped to signed PCM16 range, then converted
  back to `int16_t`. This is fixed gain plus saturation, not automatic/dynamic
  compression.
- `analyze(samples, count)` normalizes PCM16 by dividing by 32768, adds each
  sample to the circular history, and waits until there is a full 1024-sample
  window and at least a 512-sample interval. It copies ordered waveform history,
  applies a Hann window for FFT input, executes FFTW, computes magnitude from
  real/imaginary parts, normalizes by window sum, and doubles non-DC bins. It
  returns `nullopt` until it has enough history and a complete interval.
- `filter_limits(mode)` selects the three frequency bands; unknown values fall
  back to General.

The waveform is not Hann-windowed before display. The spectrum is. During
playback, `AudioEngine` calls `analyze` on the playback samples too, so telemetry
can represent playback rather than microphone capture.

### `AudioEngine.cpp`

`configure_pcm(pcm)` requests interleaved mono signed PCM16 little-endian at
exactly 8 kHz, a period near 512 frames, and a buffer near four periods. It sets
software availability/stop thresholds and a playback start threshold, applies
the settings, and prepares the device. Failure is returned as an ALSA error.

`recover_pcm(pcm, error)` calls ALSA recovery only for `-EPIPE` (underrun) or
`-ESTRPIPE` (suspend). Other errors, or failed recovery, are logged and return
false.

`set_audio_thread_policy()` requests CPU core 1 and SCHED_FIFO priority 80.
These are requests, not guarantees; errors are logged.

`run()`:

1. Opens/configures capture and playback PCM streams and initializes DSP. It
   reports `AudioReady` through `CommandQueue`; initialization failure is
   reported with an explicit error.
2. Requests its core/real-time policy and marks itself ready.
3. On each pass, takes at most one command from its single-slot mailbox and
   applies suspend/resume/playback transitions on the audio thread.
4. In live mode, reads one 512-frame block, snapshots mode/gain under
   `configuration_mutex_`, and filters/amplifies it. In playback mode, consumes
   up to 512 samples from the playback `CircularBuffer`; temporary underflow
   produces silence, while stopped-and-empty means end of playback.
5. Writes the result to the playback PCM, analyzes it for telemetry, and uses
   the recording `CircularBuffer` when recording is active. A full recording
   FIFO stops recording and reports `RecordOverrun` rather than overwriting
   older samples.
6. Offers telemetry to the one-slot `DataQueue`; a try-lock/connection failure
   increments `dropped_telemetry_` without blocking the audio worker.
7. On shutdown, stops playback production, closes ALSA handles, and marks the
   worker not ready.

The audio worker never opens WAV files; FileIO owns libsndfile and fills the
playback FIFO. The audio thread owns both ALSA handles for its lifetime.

### `FileIOManager.cpp`

`storage_ready(options)` checks the configured directory and, by default, checks
`/proc/self/mountinfo` to require an exact mounted path. `mounted_source()` maps
that mountpoint back to the device node so a later udev removal can be matched.
The helper `unescape_mount_path()` decodes mountinfo's escaped spaces and
backslashes.

`run()` owns the recording/playback `SNDFILE*` handles and raw file descriptors.
It posts initial storage flags, waits on the one-request mailbox at 10 ms
intervals, dispatches requests, and services active FIFOs:

- `OpenRecording` uses exclusive file creation and opens mono 8 kHz PCM16 WAV.
  `RecordingOpened` tells Main whether start succeeded.
- `write_recording()` checks storage, pops a partial-or-full 512-sample segment,
  writes it, and retains `StorageLost`, `DiskFull`, or `WriteError` for final
  completion.
- `CloseRecording` closes the producer gate, drains remaining samples, syncs,
  checks libsndfile's error state, closes both handles, and posts
  `RecordingClosed`.
- `PreparePlayback` checks the archive identity before and after `open()` with
  `O_NOFOLLOW`, validates mono/8 kHz/WAV/PCM16, starts the playback FIFO, and
  posts `PlaybackReady`.
- `read_playback()` fills only free FIFO capacity; `StopPlayback` closes and
  clears that stream and posts `PlaybackClosed`.
- `List` returns sorted WAV identities; `Delete` verifies the same identity
  before unlinking; `Eject` syncs and unmounts only through this worker.
- `Mount` mounts the detected USB filesystem at the configured storage path.
  `DeviceRemoved` stops writers/readers, closes their handles, marks storage
  unavailable, and attempts cleanup of the mount.
- `RefreshStorage` recomputes mounted/readable/writable/space flags.

All completions and errors go to Main through `CommandQueue`; FileIO does not
send TCP frames or change Main's transport/LED state.

### `LedDriver.cpp`

The implementation uses Linux `open`, `write`, and `close` system calls. Each
write must transfer exactly one byte; short writes and errors are logged.
`set_mode` chooses one mode bit and optionally ORs in the recording bit.
`available_` must have been set by `open()` for writes to be attempted.

### `SystemController.cpp`

The constructor wires shared queues, two sample FIFOs, the LED device,
FileIOManager, and NetworkManager. `initSystem()` initializes a libudev block
monitor, initializes LEDs and AudioDSP, waits up to five seconds for audio
startup, then starts Network. `run()` requests Main core 0 and is the four-way
coordinator: it waits up to 10 ms for a `MainMessage`, checks USB device events,
dispatches deferred storage requests, and refreshes storage flags periodically.

`process_command()` validates each command and rejects a conflicting command
while an asynchronous operation is pending. Start/stop recording, playback,
archive listing/deletion, and eject each create a `PendingOperation` with a
unique ID; worker responses advance that state machine. `complete_operation()`
emits the final `CommandResult`, `Status`, and `State`. `handle_archive_result()`
maintains connection-local file IDs and paging. The `publish_*` functions add
already-encoded frames to Network's control queue. `set_error()` and
`update_leds()` maintain error indication and authoritative LED masks.

`init_udev_monitor()` creates and enables the block-device monitor;
`check_usb_events()` only consumes relevant USB block events; and
`dispatch_deferred_mount()` retries a storage request if the FileIO mailbox is
temporarily occupied. Main requests mount/removal work but never mounts or
opens WAV files directly. `stop()` stops and joins the network/audio/file
workers, frees udev objects, and closes LEDs.

### `main.cpp`

`handle_signal` sets global `running` false for SIGINT/SIGTERM. `main` parses
options, constructs `SystemController`, calls `initSystem()`, runs Main's
message loop, and returns after the controller has stopped and joined workers.
`parse_options()` supports `--help`, `--allow-unmounted-storage`, `--bind`,
`--port`, `--capture-pcm`, `--playback-pcm`, `--storage`, and `--led-device`;
it rejects missing values, unknown flags, and ports outside 1–65535.

### `NetworkManager.cpp`

`ControlOutputQueue` keeps status/state/results/archive frames separate from
telemetry. `send_all()` transmits a complete frame within a cumulative 100 ms
deadline, retrying interrupts and polling briefly when the socket would block.
`serve_client()` polls one client, accumulates incoming bytes until complete
five-byte commands are available, decodes them, and puts them on
`CommandQueue`; it does not validate command state or access storage. Each
client gets fresh connection-local archive IDs. Network sends queued control
frames before the newest optional telemetry snapshot and clears per-client
queues on connect/disconnect.

`run()` requests core 3/SCHED_OTHER, creates an IPv4 listener with backlog one,
posts `NetworkStarted`, and accepts one client at a time. Socket send/receive
and poll failures are logged and surfaced as worker events. Only this worker
writes TCP sockets.

### C++ tests

- `architecture_test.cpp` checks non-overwriting FIFO behavior, MainMessage
  command dispatch, and latest-snapshot/disconnect semantics for telemetry.
- `band_pass_filter_test.cpp` synthesizes 8 kHz sine waves and calculates output
  RMS after a transient skip. It checks that Lung attenuates 50 Hz, Heart
  attenuates 1500 Hz, and all modes pass 500 Hz reasonably strongly.
- `protocol_test.cpp` checks big-endian integer/float encoding, command
  validation/decoding, status and file-list serialization, telemetry frame
  sizing, state/result decoding, fragmented frame assembly, non-finite float
  rejection, and malformed-frame rejection.

## 7. Flutter and Dart client

### `protocol.dart`

This file mirrors the server wire protocol.

Constants identify the five message types and maximum sizes. `Command` is a
Dart enum with explicit numeric command values. `Transport` and
`CommandOutcome` model protocol choices. `ServerMessage` is a sealed base class:
the allowed message subclasses are Status, State, CommandResult, FileList, and
Data. A sealed class allows Dart's switch to reason about all message variants.

Data classes:

- `StatusMessage`: storage mounted, record/playback capability, error number.
- `StateMessage`: transport, filter, gain, storage flags, error, active session,
  dropped telemetry, and recording overruns.
- `CommandResultMessage`: command, outcome, and detail.
- `FileEntry`: server ID and basename.
- `FileListMessage`: next cursor and entries.
- `DataMessage`: waveform and spectrum as double lists.

`_readUint32(bytes, offset)` reconstructs a big-endian 32-bit integer.

`FrameDecoder` keeps `_buffer` (partial bytes) and `_failed` (whether an invalid
frame has permanently failed this decoder):

- `clear()` empties bytes and resets failure.
- `add(bytes)` appends received TCP bytes, enforces a 1 MiB cap, extracts as
  many complete messages as possible, and keeps any incomplete tail.
- `_fail(message)` marks failed and throws `FormatException`.
- `_decodeStatus`, `_decodeState`, `_decodeCommandResult`, `_decodeFileList`,
  and `_decodeData` validate and convert each specific payload.

`encodeCommand(command, value)` verifies a 32-bit unsigned range and creates the
five-byte command. `spectrumDbfs(magnitude)` validates a finite nonnegative
magnitude, converts it to decibels relative to full scale using
`20*log10(magnitude)`, and floors near-zero input at -120 dBFS.

### `main.dart`

`main()` launches `PhonoStethosApp`. `PhonoStethosApp` is a stateless root
widget; its `build` creates a dark-themed `MaterialApp` with a
`DashboardScreen`.

`DashboardScreen` is stateful because network input changes what the screen
shows. `_DashboardScreenState` owns:

- `_host`, `_port`: server endpoint.
- `_decoder`: TCP message decoder.
- `_recordings`: archive list.
- `_socket`, `_subscription`: TCP connection and listener.
- `_reconnectTimer`, `_connecting`, `_connected`: reconnect/connection state.
- `_storageMounted`, `_allowRecord`, `_allowPlayback`: current server
  capabilities.
- `_archiveLoading`, `_archiveCursor`: archive pagination state.
- `_errorText`: banner message.
- `_pendingCommand`: the single command currently awaiting a response.
- `_mode`, `_gain`, `_transport`: controls reflected by server state.
- `_waveform`, `_spectrum`: plotted `FlSpot` values.

Lifecycle/network methods:

- `initState()` begins `_connect()`.
- `_connect()` opens a TCP socket with a three-second timeout, resets decoder
  and archive state, attaches a stream listener, and requests archive page zero.
- `_receive(bytes)` decodes all complete messages and turns malformed protocol
  data into a connection error.
- `_handleMessage(message)` updates state for status, state, telemetry, file
  list, and command results. It requests later archive pages as needed and
  refreshes after a successful delete.
- `_request(command, value)` sends a single command only when connected and no
  other command is pending.
- `_refreshArchive()` clears local archive state and requests its first page.
- `_connectionEnded(reason)` closes resources, clears pending state, and
  schedules reconnect.
- `_scheduleReconnect(reason)` shows an error and retries after three seconds.
- `dispose()` cancels reconnect/listener and destroys the socket.
- `_enabled` is true only while connected and with no pending command.

Widget methods:

- `build()` creates the app bar, connection/storage status chips, tabs, error
  banner, live dashboard, and archive.
- `_statusChip(label, color)` constructs a compact status indicator.
- `_buildListeningTab()` lays out charts and controls responsively, switching
  to a narrow layout below 850 pixels.
- `_buildControls()` creates filter, gain, recording, stop-playback, and safe
  eject controls, disabling commands when the state disallows them.
- `_sendMode(value)` and `_sendGain(value)` optimistically update the selected
  value and send its command.
- `_buildModeButton(mode, label, activeColor)` creates one mode-selection
  button.
- `_modeName(mode)`, `_modeBand(mode)`, `_bandDescription(mode)` map numeric
  mode to user-facing name and frequency range.
- `_buildArchiveTab()` draws the archive list, refresh control, play and delete
  buttons.
- `_confirmDelete(file)` asks the user before sending permanent deletion.
- `_chartPanel(...)` creates a line chart with axes, optional frequency-range
  highlight, and style settings.

The UI shows charts and controls; it does not process the microphone stream
itself. The server sends samples/telemetry and performs the selected audio
filtering. The displayed frequency axis uses `index * (8000 / 1024)` Hz.

### `protocol_test.dart`

The Dart tests check five-byte big-endian command encoding, fragmented fixed
status decoding, state field validation, file IDs/basenames, big-endian float
telemetry and dBFS conversion, and rejection of unknown or wrongly sized frames.
`_frame(type, payload)` is a test helper that constructs a length-prefixed frame.

### Generated Flutter Linux host files

- `linux/main.cc` creates and runs the GTK `GApplication`.
- `linux/my_application.cc` creates a GTK window and embeds a Flutter view.
  Its lifecycle callbacks register plugins and pass command-line arguments to
  the Dart project.
- `linux/my_application.h` declares the generated application object.
- `linux/flutter/generated_plugin_registrant.*` registers plugins selected by
  Flutter tooling.

These are generated platform-entry files, not the dashboard's protocol or UI
implementation.

## 8. Linux LED driver

`kernel-driver/phonostethos_leds.c` is a Linux platform driver and character
device. It asks the device tree for GPIO descriptors named `heart`, `lung`,
`general`, and `recording`. It exposes `/dev/phonostethos_leds`.

The `phonostethos_leds` struct stores the parent device, four GPIO descriptors,
device number, character-device/class/device objects, a mutex, delayed error
work, and `cached_mask` (last normal LED mask).

Important functions:

- `phonostethos_leds_set(leds, mask)` maps each mask bit to its GPIO on/off
  state.
- `phonostethos_leds_restore_mask(work)` retrieves its owning driver data from
  the delayed-work object and restores `cached_mask` under the mutex.
- `phonostethos_leds_open(inode, file)` finds the driver's enclosing struct and
  stores its pointer in `file->private_data`.
- `phonostethos_leds_write(file, buffer, length, offset)` requires exactly one
  byte, safely copies it from user space, validates normal mask bits, and
  handles `0xff` as a 100 ms all-LED error pulse. Normal writes cancel an
  outstanding pulse and update cached state.
- `phonostethos_leds_release` has no extra per-open cleanup.
- `phonostethos_leds_fops` connects Linux open/write/release/seek operations to
  those functions.
- `phonostethos_leds_probe(pdev)` allocates driver state, obtains GPIOs,
  allocates a device number, adds the character device, creates its class/device
  node, and registers state with the platform device. Error labels undo earlier
  registration steps in reverse order.
- `phonostethos_leds_remove(pdev)` cancels delayed work, turns all LEDs off,
  and unregisters/destroys the character device resources.
- The device-tree match table associates compatible string
  `phonostethos,status-leds` with this driver.
- `module_platform_driver` registers/unregisters the platform driver with the
  kernel.

This kernel module controls output GPIOs; the filtering, WAV handling, and GUI
remain in user space.

## 9. Important protocol and runtime behavior

### Commands from the GUI

Each command is exactly five bytes: command ID (`u8`) followed by a big-endian
32-bit value. Mode values are 0 General, 1 Heart, 2 Lung; gain values are 1–10.
Commands with no argument use zero. Play/delete use the server-issued archive
ID. Archive page zero starts listing; later pages use the previous page's
`next_cursor`.

### Server messages

- **Status (`0x02`)** is five bytes total: type, mounted, can-record,
  can-playback, error.
- **State (`0x04`)** has a 20-byte payload: version, transport, filter, gain,
  storage bit flags, error, reserved zero `u16`, session ID, dropped telemetry,
  overrun count.
- **Command result (`0x05`)** has an eight-byte payload: command, outcome,
  reserved zero `u16`, detail.
- **Data (`0x01`)** has a 6144-byte payload: 1024 big-endian floats for
  waveform, then 512 big-endian floats for spectrum.
- **File list (`0x03`)** is UTF-8 text with a cursor line and tab-separated IDs
  and WAV basenames.

Long-running server operations first return `AcceptedPending`; Main posts a
final completed/failed result after the relevant FileIO/AudioDSP responses are
collected. Invalid or conflicting commands are rejected immediately.

### Recording and files

Recording names encode the selected mode at start, local date/time, and a
session number. The file is created exclusively with permissions `0600`.
Recorded samples are the processed audio output, so live filter and gain choices
affect what is saved. A file's mode prefix reflects the initial mode and does
not capture later mode changes.

Storage capability is checked using directory/mount state, read/write access,
free space, and current error state. The server also verifies identity before
play/delete and uses `O_NOFOLLOW` when opening a file selected for playback.
Eject synchronizes the filesystem and unmounts it; the process needs suitable
privileges for that Linux operation.

## 10. Comparison against the reports and design slides

The workspace has two distinct reports:

- `G7_PhonoStethos_Report-2.pdf` is the formal project report with analysis,
  design, architecture, protocol, hardware, and test-plan chapters.
- `G7_PhonoStethos_Report-1.pdf` is titled *PhonoStethos project explanation
  and presentation guide*. It summarizes design decisions and presentation
  talking points.
- `G7_PhonoStethos_Design.pptx.pdf` is the 38-slide design/presentation deck.

The comparison below focuses on design statements versus actual source. A match
means the code contains the described mechanism, not that it has been proven on
the final hardware.

| Topic | What the report/deck describes | What the checked-in source does |
|---|---|---|
| Overall pipeline | Capture, filter, amplify, headphone output, waveform/FFT, USB recording/playback | These paths are present in `AudioEngine`, `DspProcessor`, `FileIOManager`, and `NetworkManager`. Hardware/device success still depends on the target. |
| Audio format and block | Mono PCM16 at 8 kHz, 512-sample blocks | Matches `configure_pcm` and `kPeriodFrames`; block duration is 64 ms, not a latency measurement. |
| Filter bands | General 20–2000 Hz, Heart 20–600 Hz, Lung 100–1000 Hz | Matches `filter_limits` and the UI labels. The implementation uses cascaded high-pass and low-pass biquads, fixed gain, and PCM16 saturation. |
| FFT | 1024-sample history, Hann window, frequency spectrum | The code uses a 1024-point FFT with Hann weighting for spectrum and sends 512 magnitude bins. It omits the 513th real-FFT bin. |
| Main/network threading | Separate Main and Network threads, connected by decoded messages and worker results | Implemented: Main dispatches `MainMessage`s; the Network worker only handles sockets/protocol framing and enqueues decoded commands. |
| Thread count and placement | Four primary threads: Main/core 0, AudioDSP/core 1/FIFO 80, FileIO/core 2/FIFO 50, Network/core 3/SCHED_OTHER | Implemented as four primary worker contexts. The affinity/priority calls are best-effort and log failures; actual placement and real-time privileges still require target verification. |
| Command decoding and routing | Network assembles five-byte commands; Main validates, routes, and owns authoritative state | Implemented: `serve_client()` decodes and posts a `CommandPacket`; only `SystemController` processes command semantics and commits transport/LED state. |
| Pending results | `ACCEPTED_PENDING` followed by a final command result for asynchronous work | Implemented for stateful worker operations using operation IDs and a Main-side pending-operation state machine. |
| Recording buffer | Synchronized 4096-sample shared ring/FIFO; producer must not overwrite data; stop gate and final drain | Implemented with a dedicated 4096-sample non-overwriting FIFO. AudioDSP pushes; FileIO drains and reports full-buffer/write/storage failures. A distinct same-sized playback FIFO carries samples the other direction. |
| `DataQueue` | Single latest telemetry/display snapshot; never let a slow socket block AudioDSP | Implemented as a one-slot replaceable telemetry queue, enabled only for a connected client and accessed by AudioDSP with a try-lock. |
| Other queues | Main command/result FIFO, separate worker mailboxes, and Network control-output FIFO | Implemented as distinct queues with distinct payloads and responsibilities; control/status frames are serviced before telemetry. |
| File responsibilities | FileIO owns recording, playback preparation, archive, deletion, mount, sync, and eject | Implemented in FileIO. It owns WAV handles and storage operations; Network sends bytes only and Main coordinates requests/results. |
| Playback | FileIO prepares/fills playback data; AudioDSP consumes it | Implemented with FileIO as producer of the playback FIFO and AudioDSP as its consumer. There is no separate per-playback file-reader thread. |
| USB storage events | Main uses udev and requests FileIO mount/cleanup without doing storage I/O in the event loop | Implemented with libudev block-device events. Main filters for USB ancestry, submits mount/removal requests, and retries when FileIO's mailbox is occupied. Exact target udev rules, privileges, and supported filesystems must be verified on the Pi. |
| LED control | Report/deck describes a custom one-byte LED character driver and complete status masks | Implemented by `LedDriver.cpp` plus `phonostethos_leds.c`, with mode/recording masks and a temporary `0xff` error pulse. Physical LED illumination is not sensed. |
| TCP/protocol | Report specifies explicit endian-safe commands, state/status, telemetry, archive frames, and validation | C++ `protocol.h` and Dart `protocol.dart` implement matching layouts, length checks, values, and partial-frame handling. |
| GUI | Report/deck shows live plots, controls, and an archive | `main.dart` has waveform and dBFS spectrum charts, mode/gain controls, recording/playback/eject actions, and archive play/delete. |
| Safe eject | Report describes stopping work, flushing, then unmounting | `EjectStorage` stops playback, finalizes recording, calls `syncfs`, then `umount2`. Requires correct privileges and target filesystem support. |
| Buildroot/platform work | Report includes Buildroot image, kernel/driver, device-tree, startup and hardware assumptions | The repository has Buildroot-oriented Makefile paths, driver, overlay, and deployment files. The source alone does not prove that the configured Pi image, USB gadget, sound card, storage mount, GPIO wiring, or power arrangement is operational. |
| Tests | Report's test matrix describes manual audio, recording, playback, USB removal/eject, reconnect, and sustained-load tests | The three native C++ tests pass and the full server compiles. They do not prove the Pi's audio hardware, udev/mount permissions, real-time scheduling, sound quality, latency, USB removal safety, LED wiring, or clinical accuracy. |

### Match status and remaining verification

The source has now been refactored to follow the report's core thread ownership,
queue separation, command/result flow, and storage responsibilities. This is a
source-level architecture match, not a claim that the system has passed every
report acceptance test. In particular, hardware/Buildroot deployment, the
actual USB audio device, udev permissions and supported filesystem modules,
safe physical removal, LED circuitry, scheduling privileges, and sustained
audio latency still need Raspberry Pi testing. The report's manual test cases
remain the acceptance checklist.

## 11. Build and verification notes

The C++ app uses C++17 and depends on Threads, ALSA, FFTW3f, libsndfile, and
libudev.
The native test targets can be configured without the hardware server
dependencies:

```sh
cmake -S PhonoStethos_Code/app -B /tmp/phonostethos-build \
  -DPHONO_BUILD_SERVER=OFF -DBUILD_TESTING=ON
cmake --build /tmp/phonostethos-build --parallel
ctest --test-dir /tmp/phonostethos-build --output-on-failure
```

In this workspace, all three native C++ tests passed, and the complete server
compiled with the required development libraries. The Flutter/Dart tests and
the target Raspberry Pi/hardware acceptance tests were not run as part of this
refactor.

The root `Makefile` is specific to a configured Buildroot toolchain and kernel
tree. The Flutter project has its own `pubspec.yaml`, lockfile, Linux runner,
and test command. Actual device validation should separately check capture,
playback, filter response, safe storage removal, file integrity, GPIO/LED
behavior, CPU scheduling permissions, and end-to-end latency.
