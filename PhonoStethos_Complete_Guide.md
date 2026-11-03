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
- A **template** such as `SynchronizedQueue<T, Capacity>` is a type/function
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
- `kMaxPendingAudioBlocks`: `7`, used as a queue-size limit for file work.
- `kPlaybackCapacityBlocks`: `8`, the capacity of the playback sample queue.
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

`storage_ready(options)` checks that the configured storage path is a directory.
If mounted storage is required, it scans `/proc/self/mounts` and requires an
entry whose mount point equals that path.

`Telemetry` has two fixed-size arrays:

- `waveform`: 1024 floats of processed, normalized audio history.
- `spectrum`: 512 floats of non-negative FFT magnitudes.

`PlaybackBlock` contains an array of 512 signed PCM16 samples and `count`, the
number of valid samples in that block. The final block of a WAV can be shorter
than 512.

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

### `SynchronizedQueue.h`, `CommandQueue.h`, and `DataQueue.h`

`SynchronizedQueue<T, Capacity>` is a bounded FIFO queue shared safely between
threads. Its private fields are `mutex_` (protects the deque and closed flag),
`data_available_` (wakes consumers), `space_available_` (wakes producers),
`queue_` (the stored values), and `closed_` (prevents new pushes).

- `push(value, cancelled)` waits until there is capacity, the queue closes, or
  the optional atomic cancellation flag becomes true. It moves the value into
  the queue and wakes one consumer. It returns false on close/cancel.
- `try_pop(value)` returns immediately; when nonempty it moves out the oldest
  value and wakes a producer.
- `wait_pop_for(value, wait, cancelled)` waits up to the supplied duration for
  data/close/cancel and then removes one item if present.
- `clear()` removes queued items and wakes producers.
- `notify_all()` wakes both producer and consumer waiters.
- `close()` sets `closed_` under the lock and wakes everyone.

`CommandQueue` specializes this template for `CommandPacket` with capacity 32.
`DataQueue` specializes it for `PlaybackBlock` with capacity eight. In the
current code, `CommandQueue` is used as a short-lived push/pop in the network
handler; it is not the report's worker-result mailbox. `DataQueue` is the
playback-audio queue, not a visualization queue.

### `CircularBuffer.h`

`CircularBuffer<T, Capacity>` is a fixed-size ring that overwrites its oldest
sample after becoming full. `data_` stores values; `next_` is the next position
to write; `size_` tracks the number of initialized elements.

- `clear()` fills the array with default values and resets indices.
- `push(value)` writes at `next_`, advances modulo capacity, and grows `size_`
  up to the capacity.
- `size()` returns the number of stored values.
- `copy_ordered(output)` copies oldest-to-newest into the start of the supplied
  array, then fills unused positions with default values.

The production use is FFT/waveform history in `DspProcessor`; this buffer does
not synchronize threads and is not the report's shared recording queue.

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

`AudioEngine` coordinates ALSA capture/output, DSP, recording, and playback.
Its constructor takes references to shared configuration/status values so it
does not create independent copies:

- `options_`: ALSA names and other settings.
- `mode_`, `gain_`, `recording_`, `error_code_`: atomic shared values.
- `telemetry_`: the current waveform/spectrum snapshot.
- `configuration_mutex_`, `telemetry_mutex_`: protect multi-step access to the
  respective shared values.
- `dropped_telemetry_`: count of snapshots skipped when the telemetry mutex is
  busy.
- `writer_`: file-work producer interface.
- `dsp_`: processor instance.
- `ready_`: audio devices and DSP initialized successfully.
- `stop_requested_`: shuts down the capture worker.
- `playback_requested_`, `playback_cancelled_`: playback state/cancellation.
- `capture_pcm_`: atomic pointer used so shutdown can interrupt capture.
- `capture_mutex_`: coordinates capture-handle closure with stop.
- `playback_data_`: bounded queue of audio blocks read from a WAV.
- `playback_eof_`: file reader reached end-of-file.
- `playback_mutex_`: protects playback thread start/join.
- `worker_`: main audio loop thread.
- `playback_thread_`: reads a WAV and feeds `playback_data_`.

Public functions: constructor starts the audio worker; destructor calls `stop`;
`ready()` reports startup success; `stop()` cancels playback and stops/joins
audio; `start_playback(path, descriptor)` launches a WAV reader and waits for
initial validation/readiness; `stop_playback()` cancels and joins that reader;
`playing()` checks `playback_requested_`.

Private functions: `configure_pcm(pcm)` sets ALSA PCM format/rate/channels/period
and thresholds; `recover_pcm(pcm, error)` attempts recovery from ALSA underrun
or suspend; `set_audio_thread_policy()` requests CPU 1 and SCHED_FIFO priority
80 (the calls can fail, e.g. due to permissions); `run()` owns ALSA capture and
output processing; `play_file(path, descriptor, ready)` validates and reads
mono 8 kHz PCM16 WAV data into the playback queue.

### `FileIOManager.h`

`FileWork` is a message handled by the file worker. Its scoped `Kind` enum is
`Start`, `Samples`, `Stop`, or `Barrier`. Fields:

- `kind`: operation kind.
- `path`: recording file path used for Start.
- `samples`: one 512-sample block used for Samples.
- `descriptor`: already-open file descriptor supplied for Start.
- `barrier`: promise completed when all preceding file work is processed.
- `completion`: bool promise used by Start/Stop callers.

`FileIOManager` owns a dedicated `worker_` thread and FIFO `queue_`. `mutex_`
protects the deque and flags; `ready_` wakes the worker; `recording_` is the
worker's protected recording state; `stopping_` tells it to drain and exit.
References `error_code_`, `recording_state_`, and `recording_overruns_` publish
results; `options_` supplies storage configuration.

- Constructor starts `run()` in the worker thread; destructor calls `stop()`.
- `start(path, descriptor)` enqueues Start and waits for the file to open.
- `push(samples)` enqueues a block while recording; if the bounded pending-work
  threshold has been reached it stops production, increments overrun count, and
  schedules Stop.
- `finish()` disables recording and queues Stop, waiting for close completion.
- `finish_and_wait()` calls finish and then enqueues a barrier to ensure previous
  work has passed the worker.
- `stop()` requests thread termination and joins it.
- `run()` consumes Start/Samples/Stop/Barrier work, uses libsndfile for WAV
  writing, classifies storage errors, and closes resources.

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

`SystemController` is the shared runtime data holder. Its constructor retains a
reference to `options`, constructs the LED wrapper and file worker, and the
server later constructs the audio engine.

- `mode`: selected mode as an atomic unsigned integer (the enum's numeric value).
- `gain`: atomic gain multiplier.
- `recording`: atomic recording flag.
- `error_code`: atomic protocol error number.
- `led_error`: atomic indication that the device write failed.
- `transport`: current protocol transport enum.
- `active_session`: current recording ID, or zero when none.
- `dropped_telemetry`: number of skipped GUI snapshots.
- `recording_overruns`: number of file queue overrun events.
- `telemetry`: latest waveform and spectrum.
- `configuration_mutex`: protects mode/gain snapshot.
- `telemetry_mutex`: protects telemetry snapshot.
- `archive_mutex`: protects archive ID/name/identity maps.
- `archive_by_id`: server file ID to identity.
- `archive_id_by_name`: filename to server file ID.
- `next_file_id`, `next_session_id`: ID generators.
- `led_driver`, `writer`, `command_queue`: subsystem objects.
- `audio`: unique pointer to the AudioEngine.

`update_leds()` converts mode and recording values into an LED mask and stores
whether the driver write failed. `refresh_runtime_state()` returns transport to
Live when playback ends, or when recording has ended, and clears the finished
recording session.

### `NetworkManager.h`

This header forward-declares `SystemController` to avoid including its full
definition in every user. `NetworkManager::run(controller)` creates and runs the
TCP listening server. `NetworkManager::serve_client(socket_fd, controller)`
handles one connected client synchronously.

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

1. Opens capture and playback devices named by `Options`.
2. Configures both and initializes DSP. On failure it logs, stores an error, and
   returns; `ready_` becomes true only after successful setup.
3. Repeatedly checks stop/global-running state and whether playback is active.
4. On capture/playback transitions, resets DSP and drops/prepares ALSA queues;
   it closes/reopens capture around playback.
5. In live mode, accumulates capture reads into up to 512 samples, snapshots
   mode/gain under `configuration_mutex_`, and filters/gains them.
6. In playback mode, waits briefly for a queued block. It copies valid samples
   for output; an absent block at EOF drains output, and a temporary missing
   block is treated as an underflow/error.
7. Analyzes the output samples for telemetry and writes them to the playback
   PCM device, recovering from supported ALSA errors.
8. For live recording only, pushes the processed output block to `writer_`.
9. Publishes telemetry only if `telemetry_mutex_` can be acquired immediately;
   otherwise increments `dropped_telemetry_` rather than delaying the audio
   thread.
10. On exit, marks not ready, closes capture safely, drains/closes playback.

`play_file(path, descriptor, ready)` opens the already-secured file descriptor
using libsndfile, validates mono/8000 Hz/WAV/PCM16 format, repeatedly reads at
most one 512-sample block, and pushes it into `playback_data_`. The first
successful read (or immediate EOF) signals the readiness promise. It sets
`playback_eof_`, closes the sound-file object and descriptor, and publishes
read/finalization errors when necessary.

### `FileIOManager.cpp`

The worker's `run()` holds the libsndfile file handle locally, so only the file
thread owns that active recording handle.

- Start closes any prior active handle, configures WAV PCM16 mono 8 kHz, and
  opens the supplied descriptor. On failure it closes/removes the incomplete
  path, sets DiskFull or OpenError, clears recording state, and fulfills the
  promise false. On success it sets recording state and fulfills true.
- Samples checks the mount before writing. Storage removal closes the file and
  reports StorageLost. A short/failed libsndfile write closes the file and
  reports DiskFull or WriteError.
- Stop flushes/synchronizes and closes the file and descriptor, then fulfills
  the optional result promise.
- Barrier fulfills a void promise after all earlier work items have been
  processed.
- Thread exit flushes/closes any remaining file.

`start()` and `finish()` use promises because the caller needs to know whether a
start/stop completed. `push()` does not wait for disk I/O; it only enqueues
work. The queue is a deque bounded operationally by checks against
`kMaxPendingAudioBlocks`, not a separate `CircularBuffer`.

### `LedDriver.cpp`

The implementation uses Linux `open`, `write`, and `close` system calls. Each
write must transfer exactly one byte; short writes and errors are logged.
`set_mode` chooses one mode bit and optionally ORs in the recording bit.
`available_` must have been set by `open()` for writes to be attempted.

### `SystemController.cpp`

The constructor stores the `Options` reference and initializes `led_driver`
from its device path and `writer` from the shared error/recording/overrun
variables.

`update_leds()` loads atomic mode/recording values, writes the complete LED
mask, and stores the inverse success result in `led_error`.

`refresh_runtime_state()` changes Playback to Live once `audio->playing()` is
false. It changes Recording to Live when the file worker clears `recording`,
clears `active_session`, and updates LEDs.

### `main.cpp`

`parse_options(argc, argv, options)` handles `--help`, `--allow-unmounted-storage`,
`--bind`, `--port`, `--capture-pcm`, `--playback-pcm`, `--storage`, and
`--led-device`. It checks port conversion/range and rejects missing or unknown
options.

`handle_signal` sets global `running` false. `main` parses options, registers
SIGINT/SIGTERM handling, constructs `SystemController`, attempts to open/update
LEDs, creates the audio engine, runs the network server, then stops audio,
finalizes/stops the writer, clears session state, closes LEDs, and returns the
server result.

### `NetworkManager.cpp`

The file-local `send_all(fd, data)` keeps sending until every byte is sent or
the 100 ms deadline expires. It retries interrupted calls, uses `poll` when a
nonblocking/temporarily blocked socket cannot accept bytes, suppresses SIGPIPE,
and returns false on disconnect or terminal error.

`storage_has_space(options)` requires at least 1 MiB free.
`list_recordings(options)` returns sorted `.wav` filenames only when storage is
ready. `status_packet(runtime)` calculates mounted/record/playback capability
flags and the highest relevant error. `state_packet(runtime)` snapshots mode
and gain under a mutex, calculates storage bits, clamps serialized values to
protocol ranges, and emits transport/session/counter state.

`send_archive(fd, runtime, cursor, error_code)` scans recordings, verifies
regular-file metadata, assigns or refreshes connection-local file IDs, detects
changed/stale entries, constructs a bounded page (up to 32 entries/8192 bytes),
and sends it. The cursor is the last file ID from the previous page; zero means
start. It reports StaleFileId when a nonzero cursor is not in the current
archive.

`reserve_recording_path(runtime)` creates a timestamped filename of the form
`MODE_YYYYMMDD_HHMMSS_SESSION.wav`. It uses `O_CREAT|O_EXCL` to avoid
overwriting an existing recording, increments the session ID, and stores the
active session when creation succeeds.

`handle_command(request, runtime)` validates arguments and transport before
acting:

- SetMode and SetGain update protected configuration (SetMode also updates
  LEDs).
- StartRecording requires a ready microphone and Live transport, verifies
  storage, reserves a unique path, synchronously waits for `writer.start`, then
  marks recording/transport and LEDs.
- StopRecording changes to Stopping, stops production, waits for file finalizing,
  returns to Live and clears the active session.
- PlayAudio looks up the connection-local ID, validates path/metadata, opens
  with `O_NOFOLLOW`, verifies descriptor metadata, starts playback and moves to
  Playback.
- StopPlayback cancels playback and returns to Live.
- EjectStorage stops playback/recording, waits for file completion, syncs the
  filesystem, closes its descriptor, and calls `umount2`.
- GetArchive returns an ordinary completed result; the archive page is sent by
  `serve_client`.
- DeleteRecording validates ID/metadata and Live/storage state before removing
  the file.

`serve_client(fd, runtime)` configures socket timeouts, resets archive IDs for
the new client, sends initial status/state, then polls one TCP client. Incoming
bytes are accumulated; each five-byte command is decoded and handled. For
GetArchive, it sends a page before the command result. Approximately every
64 ms, it sends status, state, and telemetry. It refreshes runtime state and LED
error indication while connected and closes the client on exit.

`NetworkManager::run(controller)` creates an IPv4 TCP socket, binds to the
configured address/port, listens with backlog one, and polls/accepts clients.
It calls `serve_client` directly, so it serves one client at a time rather than
creating a separate Network thread.

### C++ tests

- `architecture_test.cpp` checks ring-buffer ordering after overwrite, command
  queue push/pop, and playback-block queue push/pop.
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

The protocol defines `AcceptedPending`, but the current server's command
handlers generally wait for their operation and reply synchronously. The GUI
still has logic for a pending result because the protocol can represent one.

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
| Main/network threading | Report describes separate `th_Main` and `th_Network`, with command/result dispatch through a `MainMessage` queue | There is no `MainMessage` type or distinct Network worker. `main()` calls `NetworkManager::run()` on the main thread; it accepts and serves one client directly. |
| Thread count | Report specifies four primary threads: Main, AudioDSP, FileIO, Network | Current baseline is main/network together, AudioEngine worker, and FileIO worker. Playback starts an additional file-reader thread. Audio requests core 1/FIFO 80; FileIO does not request core 2/FIFO 50; Network has no dedicated core. |
| Command decoding and routing | Report's NetworkManager receives/assembles five command bytes and decodes them into a `CommandPacket`; Main then validates/routes the decoded command and coordinates worker requests/results. | `serve_client` calls `decode_command` and then calls `handle_command` itself. So the current NetworkManager performs both byte-level decoding and command decisions; there is no separate Main command dispatcher. It pushes then immediately pops `CommandQueue` in that same call path; this is not cross-thread dispatch. |
| Pending results | Report allows `ACCEPTED_PENDING` followed by a final result | `Result::AcceptedPending` is defined and decoded, but current C++ command paths return completed/rejected/failed after their synchronous work; no path was found that emits AcceptedPending. |
| Recording buffer | Report describes a synchronized 4096-sample circular recording buffer shared with FileIO | Current `CircularBuffer` is a plain, non-thread-safe FFT history buffer. Recording blocks are copied into `FileIOManager`'s mutex-protected deque; `kMaxPendingAudioBlocks` is 7. Queue overload stops recording and reports an overrun instead of silently overwriting. |
| `DataQueue` | The report's architecture guide calls it a telemetry/display-snapshot queue | In the source, `DataQueue` is the eight-block playback-audio queue used by `AudioEngine`. Telemetry is one shared `Telemetry` snapshot protected by a mutex. |
| File responsibilities | Report assigns broad storage/archive/eject work to FileIO | `FileIOManager` owns active WAV recording writes. `NetworkManager` itself lists archives, checks file identity, handles playback file opening flow, deletes files, syncs, and unmounts. This differs from the report's single FileIO storage owner. |
| Playback | Report describes FileIO preparing playback data and AudioDSP consuming it | The current `AudioEngine::play_file` thread opens/reads the WAV through libsndfile and places samples in `playback_data_`; the audio worker consumes those blocks. |
| LED control | Report/deck describes a custom one-byte LED character driver and complete status masks | Implemented by `LedDriver.cpp` plus `phonostethos_leds.c`, with mode/recording masks and a temporary `0xff` error pulse. Physical LED illumination is not sensed. |
| TCP/protocol | Report specifies explicit endian-safe commands, state/status, telemetry, archive frames, and validation | C++ `protocol.h` and Dart `protocol.dart` implement matching layouts, length checks, values, and partial-frame handling. |
| GUI | Report/deck shows live plots, controls, and an archive | `main.dart` has waveform and dBFS spectrum charts, mode/gain controls, recording/playback/eject actions, and archive play/delete. |
| Safe eject | Report describes stopping work, flushing, then unmounting | `EjectStorage` stops playback, finalizes recording, calls `syncfs`, then `umount2`. Requires correct privileges and target filesystem support. |
| Buildroot/platform work | Report includes Buildroot image, kernel/driver, device-tree, startup and hardware assumptions | The repository has Buildroot-oriented Makefile paths, driver, overlay, and deployment files. The source alone does not prove that the configured Pi image, USB gadget, sound card, storage mount, GPIO wiring, or power arrangement is operational. |
| Tests | Report's test matrix marks hardware tests “Not tested” and describes planned manual checks | Three C++ tests and Dart protocol tests exist. Passing software tests do not validate microphone hardware, sound quality, latency, USB removal, LED wiring, or clinical accuracy. |

### The most important architectural differences

The formal report and the explanation guide describe a planned ownership model:
Network assembles and decodes the wire command into a `CommandPacket`, then queues
it; Main validates and routes that command, specialized workers perform operations,
and Main commits the final state. The checked-in implementation is simpler and more
synchronous:

- `NetworkManager::run` and `serve_client` execute on the original main thread.
- `handle_command` performs command work directly from `serve_client`.
- `CommandQueue` currently does not transfer a request to a separate Main
  dispatcher.
- There is no `MainMessage`, worker-result protocol, separate Network thread,
  USB event monitor, or shared recording `CircularBuffer` in the checked-in
  application.
- File recording is asynchronous at the write-block level, but Start and Stop
  wait for promises from the file worker before reporting their result.

Thus, parts of the report are best read as the target architecture/specification,
not as a literal explanation of every current implementation detail. If the
report is intended to claim the four-thread asynchronous design is already
implemented, it should be revised to match the code or the code should be
extended to match the report.

## 11. Build and verification notes

The C++ app uses C++17 and depends on Threads, ALSA, FFTW3f, and libsndfile.
The native test targets can be configured without the hardware server
dependencies:

```sh
cmake -S PhonoStethos_Code/app -B /tmp/phonostethos-build \
  -DPHONO_BUILD_SERVER=OFF -DBUILD_TESTING=ON
cmake --build /tmp/phonostethos-build --parallel
ctest --test-dir /tmp/phonostethos-build --output-on-failure
```

In this workspace, those three native C++ tests passed after the namespace
change. A full server configuration was not possible in the host environment
because the ALSA development package was missing. The Flutter/Dart tests and
the target Raspberry Pi build were not run as part of creating this guide.

The root `Makefile` is specific to a configured Buildroot toolchain and kernel
tree. The Flutter project has its own `pubspec.yaml`, lockfile, Linux runner,
and test command. Actual device validation should separately check capture,
playback, filter response, safe storage removal, file integrity, GPIO/LED
behavior, CPU scheduling permissions, and end-to-end latency.
