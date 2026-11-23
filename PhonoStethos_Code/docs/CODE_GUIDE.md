# How PhonoStethos works

Start here if the source feels difficult to follow. You do not need to understand every error branch before understanding the normal recording path.

## 1. Read these files in this order

1. `app/src/main.cpp`: reads options, creates SystemController, starts the program.
2. Read `SystemController.cpp` for startup, the Main loop, GUI commands and worker replies. Its named sections separate those responsibilities inside one file. Follow `startRecording()` into `handleWorkerResult()` to see when Main enables recording.
3. `app/src/AudioEngine.cpp`: read `run()`. This is capture, filtering, gain, headphone output, recording and FFT.
4. `app/src/FileIoManager.cpp`: read `run()`, `process_request()`, `openRecordFile()`, `write_recording()` and `closeFile()`.
5. `app/src/NetworkManager.cpp`: read `run()`, `serve_client()` and `receiveCommand()`.
6. The three shared-data headers: `CommandQueue.h`, `CircularBuffer.h`, `DataQueue.h`.
7. `kernel-driver/phonostethos_leds.c`: the separate kernel module that changes the GPIO outputs.

The capitalisation is now **FileIoManager**, matching the class name in the report.

## 2. One application, four execution contexts

There is one server process on the Pi. Main is the original thread of that process; it creates three workers with `pthread_create()`.

| Thread | Its job | Scheduling requested |
|---|---|---|
| th_Main | Validate commands, coordinate workers, own system state and LED masks | Core 0, SCHED_OTHER, priority 0, nice 0 |
| th_AudioDSP | Own ALSA, run live DSP, output audio and calculate FFT | Core 1, SCHED_FIFO, priority 80 |
| th_FileIO | Own WAV handles, recording/playback file operations and storage management | Core 2, SCHED_FIFO, priority 50 |
| th_Network | Own the TCP listening/client sockets; receive commands and send complete frames | Core 3, SCHED_OTHER, priority 0, nice 0 |

Main and Network have the same scheduler policy and nice value. CPU affinity is a separate setting: it selects where a thread runs. The requested FIFO policies require appropriate privileges. A failed scheduling/affinity request is logged, so check the target's startup log.

Each worker has a `threadEntry(void*)` function because pthread_create expects that callback signature. Its argument is the address of the manager object. The callback converts that address back to the correct class and calls `run()`.

`start()` creates the pthread. `stop()` requests termination and uses `pthread_join()` to wait for it to finish before destroying shared objects. Those maintenance-stop functions do not add a GUI power-off command.

## 3. What happens during startup

Constructors prepare objects, buffers and mutexes; they do not start workers.

Main opens the LED device and calls `DspProcessor::initialize()` to create the FFTW plan. **FFTW planning finishes before any worker pthread is created.** SystemController owns this DSP object and passes it by reference to AudioEngine.

Main then starts AudioDSP and FileIO. AudioDSP opens/configures its own ALSA streams and reports AudioReady. FileIO checks storage and reports FileIOReady. Main handles those messages, and Network then starts listening. Missing storage disables storage-dependent controls while live audio remains possible. An unavailable LED device is reported without preventing audio startup.

## 4. How a click becomes an operation

For example, clicking Record sends five bytes: command number 3 followed by a four-byte value of zero.

1. Network receives the bytes. TCP may split a command, so `receiveCommand()` retains partial bytes until all five arrive.
2. Network decodes the command and places it in **CommandQueue** as a MainMessage.
3. Main takes the message. `processCommand()` validates it and calls `startRecording()`.
4. Main asks FileIO to open a WAV file through FileIO's control mailbox. Main records that the operation is pending and continues dispatching messages.
5. FileIO opens the WAV and puts a RecordingOpened result in CommandQueue.
6. Main handles that result, enables the recording buffer, updates RECORDING and the LEDs, and queues the GUI replies.
7. Network sends those replies to the GUI.

Main passes the selected file ID directly to `startPlayback(file_id)` and the archive page cursor to `listFiles(cursor)`. In the dispatch flowchart, these values come from `cmd.value`. A cursor of zero requests the first page.

A worker result is an internal message, not another TCP command. The report's OPEN_READY is named `WorkerResultCode::RecordingOpened` in code; RECORD_CLOSED is `RecordingClosed`. The meaning is the same.

## 5. Queues, buffers and mailboxes

| Object | Direction | Contents |
|---|---|---|
| CommandQueue | Network, FileIO and AudioDSP to Main | GUI commands or worker results/errors |
| ControlReplyQueue | Main to Network | Serialized status, State, CommandResult and archive frames |
| DataQueue | AudioDSP to Network | At most one waiting waveform/FFT snapshot |
| Recording CircularBuffer | AudioDSP pushes; FileIO pops | Processed PCM16 samples |
| Playback CircularBuffer | FileIO pushes; AudioDSP pops | PCM16 samples read from a WAV |
| FileIO control mailbox | Main to FileIO | One unprocessed storage request |
| Audio control mailbox | Main to AudioDSP | One unprocessed capture/playback transition |

A mailbox here is a plain `Request` plus a boolean (`has_request_` or `has_mailbox_request_`) protected by a pthread mutex. It is a single slot, not another class and not a queue of many requests. `request()` fails if that slot is occupied. Main can retain a deferred request and retry when the worker has taken the previous one.

The two CircularBuffer objects are separate instances of the same PCM16 buffer class. Each has its own array, head, tail, sample count, stop flag and mutex. Both hold 4096 samples.

ControlReplyQueue preserves the order of control frames. DataQueue deliberately replaces an older waiting visualization snapshot. Losing a visualization update does not remove samples from a recording or from headphone output. Only Network serializes/sends TCP data, so two threads cannot interleave frame bytes.

## 6. The pthread synchronization calls

- `pthread_mutex_init()` prepares a mutex. A mutex permits only one thread at a time to access protected data.
- `pthread_mutex_lock()` enters that short protected section; `pthread_mutex_unlock()` leaves it.
- `pthread_mutex_trylock()` tries once without waiting. AudioDSP uses this for telemetry so a busy visualization queue does not stall audio.
- `pthread_cond_init()` prepares a condition variable. It lets a thread sleep while waiting for a protected condition to change.
- `pthread_cond_signal()` wakes a waiter; `pthread_cond_broadcast()` wakes all waiters.
- `pthread_cond_timedwait()` releases the mutex while sleeping and reacquires it before returning. Always recheck the queue/buffer condition after waking. A signal is not the data itself.
- `pthread_setaffinity_np()` requests a CPU core. `pthread_setschedparam()` requests SCHED_OTHER or SCHED_FIFO and its priority.

`ThreadSupport.h` contains one small `PthreadLock` scope guard. Its constructor calls pthread_mutex_lock and its destructor calls pthread_mutex_unlock. In a block such as `{ PthreadLock lock(mutex_); ... }`, the mutex is therefore released at the closing brace, including an early return. It does not create another synchronization mechanism.

The timed waits use CLOCK_MONOTONIC. `waitDeadline(10)` converts ten milliseconds into the absolute deadline required by pthread_cond_timedwait.

## 7. The live audio loop

AudioEngine accumulates a complete block of 512 PCM16 samples. At 8000 samples/second that represents 64 ms of audio, not a measured end-to-end acoustic latency.

At a block boundary it reads mode and gain under the configuration mutex. It releases that mutex before doing DSP:

1. Convert the captured samples to floating point.
2. `applyBandpass()` filters them using the selected GENERAL, HEART or LUNG profile.
3. `applyGainWithLimiter()` applies gain and clamps to -32768 through 32767 before PCM16 conversion.
4. `writeFrames()` sends the samples to ALSA/headphones, continuing until partial writes are complete.
5. If recording is enabled, push the processed samples into the recording buffer.
6. `computeFFT()` updates the rolling history and generates visualization data after headphone output/recording work.

The filter uses a fourth-order Butterworth high-pass followed by a fourth-order Butterworth low-pass, eighth order overall. Each edge uses two biquads. The three profile cutoff frequencies are unchanged.

The FFT uses 1024 real samples, advances by 512 samples and first becomes ready after two valid blocks. It applies a Hann window to a separate copy. The unwindowed waveform is normalized PCM16. FFTW produces 513 complex values; the GUI receives magnitudes for bins 0 through 511, omitting the Nyquist bin.

## 8. Recording and Stop

`CircularBuffer::push()` checks the recording gate under the same mutex used to close that gate. It either accepts a whole block or reports that it was not stored; it never overwrites old recording samples.

FileIO calls `pop()`, which waits for samples if needed and returns their actual count. The buffer lock is already released when FileIO calls libsndfile. A slow write therefore does not hold the audio buffer mutex.

On STOP_REC, Main closes the recording gate first. FileIO writes the samples already accepted, including a final partial block, then checks synchronization and WAV closure. Main publishes completion after the FileIO result. A new recording starts with an empty buffer.

A normal push rejected because Stop already closed the gate is not an overflow. A real recording overflow stops that recording and reports an error. FileIO also reports a failed disk write directly rather than waiting for an audio-side overflow to reveal it.

## 9. Playback

FileIO opens and validates the selected mono 8 kHz PCM16 WAV, fills the playback buffer and reports readiness. Main also requests AudioDSP to suspend microphone capture. After preparation, AudioDSP pops playback samples and writes them to headphones. Stored audio bypasses the live band-pass filter. The same gain control scales playback samples before headphone output and FFT analysis. Playback gain leaves the WAV unchanged.

FileIO reads only as many samples as fit in the buffer. When it reaches EOF it closes the producer gate. AudioDSP consumes the remaining samples, calls snd_pcm_drain to finish headphone output, then calls snd_pcm_prepare before reporting completion. The prepare call makes the output ready for live writes again; drain leaves it in SETUP. If the buffer is unexpectedly empty before EOF, AudioDSP emits a silence block and reports a read/underflow fault to Main. Main coordinates file cleanup and return to live capture.

## 10. Why Main has a pending operation

Opening a file, preparing playback, closing a WAV and unmounting storage take time. Main must keep receiving results while those operations run.

`pending_` is Main's record of the current unfinished GUI operation. `operation_id` identifies that request so an old worker reply cannot complete a newer operation. `file_done` and `audio_done` record which worker has answered. `WorkflowStage` uses names instead of numeric phases: WaitingForWorkers, ResumingCapture and EjectingStorage.

This explains most of `handleWorkerResult()`: different results advance the current operation until Main can publish a final COMPLETED or FAILED response. The branches are needed because playback and eject involve more than one worker.

## 11. LEDs and the kernel boundary

Main creates the complete LED mask: Heart `0x01`, Lung `0x02`, General `0x04` and Recording `0x08`. For example, Heart plus Recording is `0x09`. Stopping recording removes bit `0x08` without removing the mode bit.

LedDriver writes one byte to `/dev/phonostethos_leds`; each write replaces the full mask. The kernel module maps the Raspberry Pi 4 GPIO register block at physical address `0xFE200000` and controls GPIO17, GPIO27, GPIO24 and GPIO22 directly. To turn on Heart and General manually, send byte `0x05`:

```sh
printf '\005' > /dev/phonostethos_leds
```

Send `0x00` to turn all LEDs off:

```sh
printf '\000' > /dev/phonostethos_leds
```

Bits outside `0x0F` and writes that are not exactly one byte are rejected.

A successful write means the driver interface accepted the command. There is no electrical feedback to prove an LED physically illuminated.

This direct-register driver is specific to the Raspberry Pi 4's BCM2711 address map. It does not use a Device Tree overlay or the GPIO descriptor API.

## 12. Libraries versus project methods

| Library / facility | Used for |
|---|---|
| Pthreads | Worker creation, joining, affinity/scheduling, mutexes and condition variables |
| ALSA | Opening/configuring PCM streams, capture, headphone output and XRUN recovery |
| FFTW3f | Planning and executing the single-precision real-to-complex FFT |
| libsndfile | Reading/writing PCM16 WAV files and finalizing their headers |
| libudev | Detecting storage add/remove events in Main |
| POSIX sockets | TCP bind/listen/accept, receive, partial sends, polling and timeouts |
| ioremap / readl / writel | Mapping and accessing the Raspberry Pi GPIO registers |
| Flutter / Dart / fl_chart | Laptop dashboard, TCP client, frame decoding and charts |

`readFrames()` is a project method that calls the external ALSA API `snd_pcm_readi()`. `computeFFT()` is a project method that calls `fftwf_execute()`. The distinction is useful when explaining API-call boxes in flowcharts.

## 13. The C++ you need to read this project

| Syntax or type | Meaning here |
|---|---|
| Header and source file | A header declares what methods and data a class has. A .cpp file defines what those methods do. CMake compiles the source files and links them into one server. |
| class and struct | Both group data and methods. A class defaults to private members. A struct defaults to public members. A MainMessage is a data record, while SystemController owns behavior and state. |
| ClassName::method | The method belongs to that class. SystemController.cpp defines the methods declared in SystemController.h. |
| Constructor and destructor | The constructor prepares an object. The destructor releases resources. Members are destroyed in reverse declaration order. |
| T&, const T&, T* | A reference uses an existing object. A const reference allows reading it without copying. A pointer stores an address and may be null. AudioEngine receives the DSP object by reference. |
| void* and static_cast | pthread accepts an untyped pointer. The thread entry converts it back to the class pointer it was given before calling run. |
| Ordinary member object | SystemController owns AudioEngine directly, just like NetworkManager and FileIoManager. No dynamic allocation is needed for these managers. |
| enum class and switch | A named set of alternatives and a way to dispatch each alternative. Command and WorkerResultCode use this pattern. |
| Request plus bool | The struct stores a request. A separate boolean says whether the slot is occupied. Both are accessed under the mailbox mutex. |
| atomic<T> | Allows individual reads and writes across threads. It does not make a group of reads into one consistent snapshot. The configuration mutex protects the combined mode/gain snapshot. |
| CircularBuffer and array | Each buffer owns a fixed PCM16 array of 4096 samples. Recording and playback use separate objects. Tests can select a smaller working capacity to force wrap-around. |
| vector, map and string | A resizable list, key/value lookup and text. The archive uses maps to resolve file IDs and names and a vector to build an ordered listing. |
| std::move | Allows a value such as a request to transfer its owned data into another object. Moving a request does not start a thread. |
| RAII and scope | A local object's destructor runs when its scope ends. PthreadLock uses this to unlock on normal exit and early returns. |
| uint8_t, uint32_t, int16_t | Exactly 8-bit unsigned, 32-bit unsigned and 16-bit signed values. The TCP protocol writes fields explicitly in byte order. It does not transmit raw C++ object memory. |
| std::filesystem and std::chrono | Paths and directory operations, and time points/durations. A steady clock measures elapsed time independently of wall-clock adjustments. |

## 14. External calls by subsystem

Read the call site with this table. Names such as startRecording, readFrames and publish_state are your own project methods.

| API | Inputs, result and role |
|---|---|
| pthread_create / pthread_join | Create a thread with a callback and object pointer. Join waits until the thread has returned. Pthread functions normally return zero on success or an error number directly. |
| pthread_self / pthread_setaffinity_np / pthread_setschedparam / setpriority | Get the current pthread, request a CPU mask, request scheduler policy and static priority, and set the nice value. Affinity and priority are separate. |
| pthread_mutex_init / lock / trylock / unlock / destroy | Set up, acquire, attempt immediate acquisition, release and clean up a mutex. Shared queue and buffer fields are accessed under their own mutexes. |
| pthread_condattr_init / setclock / destroy | Set the clock for condition-variable deadlines and free the temporary attributes. |
| pthread_cond_init / timedwait / signal / broadcast / destroy | Initialize, wait with an absolute deadline, wake a waiter or all waiters, and release the condition variable. A timeout lets the worker check control requests. |
| snd_pcm_open / close | Open or release a capture or playback stream. A negative result is an ALSA error. snd_strerror turns that result into diagnostic text. |
| snd_pcm_hw_params_alloca / any / set_* / snd_pcm_hw_params | Allocate a temporary parameter object, start with supported settings, request interleaved PCM16, channels, rate and period/buffer sizes, then apply them. The near variants can negotiate a different value. |
| snd_pcm_sw_params_alloca / current / set_* / snd_pcm_sw_params | Read the current software settings and set thresholds controlling when playback starts or waits and when frames are available. Apply them to the stream. |
| snd_pcm_readi / writei | Transfer interleaved frames. A positive count can be smaller than requested, so the code advances by the actual count. Mono has one sample per frame. |
| snd_pcm_prepare / recover / drop / drain / stream | Prepare or recover a stream, stop immediately, finish queued samples, or query its direction. Recovery is used for ALSA stream errors such as an overrun or underrun. |
| fftwf_plan_dft_r2c_1d / execute / destroy_plan | Prepare a single-precision transform of a real array, run that prepared transform on its buffers, then release the plan after workers stop. |
| open / sf_open_fd | Open a recording exclusively, or open a playback file, then hand the descriptor to libsndfile with the format information. O_EXCL prevents replacing an existing recording. |
| sf_read_short / sf_write_short | Read or write signed 16-bit sample items and return the actual count. FileIO checks short transfers and uses sf_error for diagnosis. |
| sf_write_sync / sf_close / sf_strerror | Flush libsndfile buffers, finalize and close its handle, or obtain error text. A WAV close can fail, so its result matters. |
|  std::filesystem::space / syncfs / mount / umount2 / unlink | Query free space, synchronize the storage filesystem, mount/unmount storage, and remove a directory entry. Main reports safe eject only after FileIO's successful unmount result. |
| socket / setsockopt / bind / listen / accept | Create the TCP endpoint, configure socket behavior, assign the local IP/port, start listening, and obtain a separate socket for the connected GUI. |
| poll / recv / send / close | Wait for socket readiness, receive bytes, send bytes, and release a descriptor. recv returning zero means the peer closed. send can accept only part of a frame. Resume from the returned byte count. |
| udev_new / udev_monitor_new_from_netlink | Create the libudev context and subscribe to device events. |
| udev_monitor_filter_add_match_subsystem_devtype / enable_receiving / get_fd / receive_device | Select events, enable monitoring, get the descriptor to poll, and read a device event. |
| udev_device_get_action / get_devnode / get_property_value / get_parent_with_subsystem_devtype | Identify what happened, the device path, properties, and its parent where needed. unref calls release libudev references. |
| open / write / close in LedDriver | Open the character device, submit one mask byte, and release the descriptor. |
| Kernel I/O mapping and user-copy API | The module maps the fixed GPIO register block, copies one mask byte from the process, and drives the corresponding lines. |
| Dart Socket.connect / listen / add / flush | Connect, receive byte chunks, queue outgoing bytes, and obtain a Future for flushing them. The dashboard's command result comes from the server. |
| Dart ByteData / Flutter setState | Decode fixed-size fields with explicit byte order and ask Flutter to redraw after state changes. |

## 15. Follow one command with the source open

Start in Flutter's _request. The GUI places the encoded five bytes in the socket and marks the command pending. On the Pi, trace receiveCommand, CommandQueue, processCommand, then startRecording. Stop at the point that posts OpenRecording to FileIO. The next step happens on another thread, inside FileIoManager::process_request. Find RecordingOpened and trace that message back to handleWorkerResult on Main. Finally find the control reply in Network and the Flutter CommandResult handler. Repeat with STOP_REC to see where the buffer is drained and where the GUI learns that the WAV is closed.

See PI_TUTORIAL.md for deployment and REPORT_ALIGNMENT.md for the audit findings.

## Gain and storage configuration

The GUI offers 0.5×, 0.75×, 0.8× and integer settings from 1× to 20×. The same gain applies to live output and playback. New recordings still store the processed live signal. Playback gain changes headphone output and telemetry without rewriting the WAV. At high gain the PCM16 limiter clips peaks to prevent integer wraparound. It cannot restore detail already clipped during recording.

SET_GAIN keeps its five-byte layout. Its value is 1–20 for integer gain, or 101, 102, 103 for 0.5×, 0.75×, 0.8× respectively. The State gain byte uses the same codes. Run the matching GUI and server.

The target uses a FAT32 USB drive mounted at /media/usb_storage. The Linux filesystem type is vfat.


## Readability revision

SystemController is now implemented in one source file. It previously had three source files for one class to separate startup, commands and replies. The section comments now serve that purpose.

There is no std::optional in the application. Each mailbox and deferred operation uses a plain record and a boolean. DataQueue::try_dequeue(packet) returns a bool and fills its output argument only on success. Descriptors still use -1 for “not open.” A request is a struct, so a bool is clearer than using a number as an empty request.

The LED driver follows the lesson's character-device structure with alloc_chrdev_region, cdev_init/cdev_add, class_create, device_create, write and module cleanup. It maps Raspberry Pi 4 GPIO registers at 0xFE200000. The lesson's 0x3F200000 base is for a different peripheral mapping and must not be copied to this Pi 4 driver. copy_from_user safely obtains the byte. The byte is binary, so shell “echo 1” is not this project's protocol.

The code remains C++17. std::array stores fixed buffers, std::vector stores variable lists and frames, std::map resolves file IDs, and std::filesystem handles paths. PthreadLock is a small lock/unlock scope guard, which prevents an early return from leaving a mutex locked. These types retain specific jobs in the implementation.

Comments increase the physical line count. The worker protocols, partial TCP transfers, WAV finalization and recovery paths still need executable code. They are the main reason the application is larger than the one-LED classroom example.


### Classroom source files

ledmodule.c contains the handwritten module callbacks. utils.c implements GPIO register operations, and utils.h declares the register layout and address. The Makefile asks the target kernel build system to compile them. led.mod.c is generated by that build system and should not be edited by hand.

### Reading the pending flags

Main calls begin_operation to fill pending_ and set has_pending_operation_. While this is true, another conflicting GUI operation is rejected. A worker reply is compared with pending_.operation_id before it can finish the operation. complete_operation copies the result, clears the flag and sends the final result to the GUI. Old values may remain in the struct after the flag is cleared, but the code checks the flag before using them.

A worker mailbox is similar. Main locks its mutex, checks the flag, copies in the request and sets the flag. The worker locks the same mutex, copies out the request, clears the flag and unlocks. The worker then executes the operation outside the lock. This is one slot rather than a queue of many requests.

### Driver unload and registration cleanup

alloc_chrdev_region reserves the device number. cdev_init links file_operations to the cdev object, and cdev_add registers it. class_create and device_create publish the node. On failure, the error labels undo the successful steps in reverse order. On unload, device_destroy, cdev_del, class_destroy and unregister_chrdev_region release registration. The GPIO outputs are turned off and returned to inputs before iounmap releases the register mapping.

The direct GPIO address targets Raspberry Pi 4 (BCM2711). The driver assumes that GPIO17, GPIO27, GPIO24 and GPIO22 are dedicated to the LED circuit and are not also controlled by another peripheral. Binary mask 0x01 selects Heart, 0x02 Lung, 0x04 General and 0x08 Recording. Combine bits, for example 0x09 for Heart plus Recording.


## Basic Flutter reading path

Read _connect, _receive, _handleMessage, _updatePlots and _request in main.dart in that order. _connect awaits Dart Socket.connect and installs socket.listen callbacks. The decoder in protocol.dart assembles complete binary frames. _handleMessage uses ordinary type checks to update the dashboard's fields. _updatePlots uses for loops to convert the waveform and average FFT power. _request records the pending command and sends five bytes. The server reply clears pending.

Flutter's setState schedules a redraw after state changes. showDialog returns the deletion confirmation. LineChart from fl_chart draws FlSpot points and does not calculate the FFT. API comment blocks are beside these calls. Nullable Socket/Timer references remain because Flutter creates and disposes these resources over the lifetime of the screen.

### Simplifications in this revision

AudioEngine is an ordinary SystemController member. DSP filter limits use two named output variables instead of a pair and structured binding. Status and State fields are filled one by one. Command completion uses explicit success and failure branches. Pthread startup failures print their error and exit instead of constructing a C++ exception object. Flutter message dispatch uses if/type checks and plot construction uses loops instead of functional collection chains. The wire layouts, coefficients, gain behavior and worker responsibilities stay as defined in the report.


## Why the application has 13 headers

A header tells another source file which class, structure or function it can use. A source file contains the operations. Keeping the class names visible makes the folder correspond to the class diagram.

| Header | Why it exists |
|---|---|
| SystemController.h | Main's state, pending operation and controller methods. |
| AudioEngine.h | ALSA worker and its control request. |
| DspProcessor.h | Filtering, gain and FFT methods. |
| FileIoManager.h | WAV/storage worker and its control request. |
| NetworkManager.h | TCP worker and its outgoing ControlReplyQueue. |
| LedDriver.h | Userspace wrapper for open, write and close. |
| CircularBuffer.h | Concrete PCM16 ring buffer, shared by audio and storage. |
| CommandQueue.h | MainMessage record and its synchronized queue. |
| DataQueue.h | Latest telemetry slot shared by audio and network. |
| BandPassFilter.h | Biquad calculations and the band-pass cascade, also tested independently. |
| RuntimeTypes.h | Shared Options, FilterMode, telemetry sizes and archive identity. |
| protocol.h | Wire field definitions, encoding and decoding. |
| ThreadSupport.h | Pthread initialization, deadline and short mutex guard. |

These headers are included from multiple source or test files. Combining them would mix unrelated definitions. Small queue implementations stay next to their declaration to avoid adding more source files.

## Further basic-code changes

CircularBuffer is now a normal class for int16_t PCM samples, with a plain array. The application uses its default capacity of 4096. There is no buffer template in the application.

Command-line port parsing and archive ID parsing use loops that validate decimal digits and check the allowed range before storing a value. Archive cursor lookup uses a loop, and filename sorting uses a named comparison function passed to std::sort. Normal switch statements remain for worker requests, protocol frames and listening profiles. The structs still describe commands, results and pending operations.

More lines can be easier to read when a conditional expression becomes an if/else. The file size alone does not say how difficult the code is. The controller separates general notifications, errors, recording results and playback results into named methods. Playback and eject still wait for independent FileIO and AudioDSP replies before changing the final state.


## Find a presentation/flowchart box in the source

Comments starting with `FLOWCHART:` identify the corresponding behavior. `CONTROL:` identifies a request/reply path between threads. `API:` describes an external library or operating-system call. A project method such as `readFrames()` is a wrapper; the ALSA API is `snd_pcm_readi()` inside it.

| Diagram box / arrow | Code to open | What it does |
|---|---|---|
| Boot / initialization | `SystemController::initSystem()` | Open the LED interface, prepare FFTW and start workers. |
| th_Main queue wait | `SystemController::run()` -> `CommandQueue::dequeue()` | Wait up to 10 ms for a message, release the queue mutex, then dispatch. |
| message.kind | `SystemController::processMessage()` | Separate GUI commands from worker results/errors. |
| cmd.cmd_type | `SystemController::processCommand()` | A switch calls the named controller operation. The argument comes from `request.value`. |
| Main -> FileIO control | `request_file()` / `queue_file_request()` -> `FileIoManager::request()` | Copy a Request into the one-slot mailbox under its mutex. A deferred request is retained if necessary. |
| Main -> AudioDSP control | `request_audio()` -> `AudioEngine::request()` | Copy a capture/playback request into AudioDSP's mailbox. |
| FileIO checks control | `FileIoManager::run()` -> `process_request()` | Take the mailbox record, clear its occupied flag, then switch on RequestKind outside the mutex. |
| Open recording WAV | `FileIoManager::openRecordFile()` | Create a new file exclusively and open it with libsndfile. |
| Stop/finalize recording | `FileIoManager::finish_recording()` | Write remaining accepted samples, check closure, then report RecordingClosed. |
| Prepare playback | `FileIoManager::prepare_playback()` | Validate the selected file identity/format and start filling the playback buffer. |
| FileIO pop recording | `FileIoManager::write_recording()` | Pop samples, release the buffer lock, then call writeChunk. |
| FileIO push playback | `FileIoManager::read_playback()` | Read WAV samples and push them into playback_buffer_. At EOF, close its producer gate. |
| AudioDSP checks control | `AudioEngine::run()` -> `apply_request()` | Take the mailbox record at a block boundary and switch on RequestKind. |
| Capture / playback selection | First half of `AudioEngine::run()` | Accumulate microphone frames or pop playback_buffer_. |
| Band-pass filter | `DspProcessor::applyBandpass()` -> `BandPassFilter::process()` | Follow four explicitly ordered biquad sections. Playback bypasses this live filter. |
| Gain + limiter | `AudioEngine::apply_output_gain()` -> `DspProcessor::applyGainWithLimiter()` | Ramp a changed gain, multiply samples and clamp to PCM16 limits. Applies to live audio and playback. |
| Headphone output | `AudioEngine::write_output_block()` -> `writeFrames()` | Continue partial ALSA writes until the block is written or recovery fails. |
| AudioDSP push recording | `AudioEngine::store_recording_block()` | Push only live recording samples; report a genuine recording buffer overflow. |
| FFT -> DataQueue | `AudioEngine::publish_visualization()` | Update the rolling FFT, then publish a visualization snapshot without waiting for Network. |
| Worker -> Main result | Each worker's `post_result()` -> CommandQueue -> `handleWorkerResult()` | Carry a result code and operation_id back to Main. |
| Main updates after recording result | `handle_recording_result()` | Enable recording only after the WAV is open, or finish stopping after closure. |
| Main updates after playback result | `handle_playback_result()` | Wait for FileIO and AudioDSP readiness/cleanup before advancing the operation. |
| Main -> Network control replies | `broadcastStatus()` / `publish_command_result()` -> `queue_control()` | Serialize a control response and put it in ControlReplyQueue. Network owns socket writes. |
| Network accepts GUI | `NetworkManager::run()` -> `startListening()` / `serve_client()` | Open/bind/listen, accept a client and run its send/receive loop. |
| Select outgoing frame | `NetworkManager::send_next_frame()` | ControlReplyQueue first; otherwise the latest DataQueue snapshot. |
| Retain partial-send offset | `NetworkManager::send_all()` variable `bytes_sent` | Send from frame.data()+bytes_sent; increase the offset only by bytes actually accepted. |
| Receive five command bytes | `receiveCommand()` variable `bytes_received_` | Keep partial TCP input until the full command can be decoded. |
| GUI connect / receive | `_connect()` / `_receive()` / FrameDecoder.add() | Dart Socket.connect opens TCP; listen receives arbitrary chunks; the decoder assembles frames. |
| GUI action availability | `_enabled`, `_canToggleRecording`, button onPressed conditions | Use connection, pending state, transport and the Pi's allowRecord/allowPlayback flags. Disabled buttons have a null callback. |
| Confirm deletion | `_confirmDelete()` | Await showDialog; send the selected file ID only after confirmation. |
| GUI mark pending / send | `_request()` | Set _pendingCommand, encode the five bytes and submit them through Socket.add. |
| GUI handle result | `_handleCommandResult()` | AcceptedPending keeps controls pending; a final result clears pending and updates the archive if needed. |
| GUI FFT display | `_updatePlots()` | Convert magnitudes to displayed power/dBFS points and maintain recent peaks; it does not calculate another FFT. |
| LED mask output | `update_leds()` -> `LedDriver::writeByte()` -> kernel `led_write()` | One mask byte crosses the device boundary and selects the four GPIO outputs. |

### Example: follow Record through the code

Read this as one trace rather than opening every file at once:

1. GUI `_toggleRecording()` calls `_request(Command.startRecording, 0)`.
2. Network `receiveCommand()` produces a CommandPacket and enqueues it in CommandQueue.
3. Main `processMessage()` -> `processCommand()` -> `startRecording()`.
4. `startRecording()` records a pending operation and puts OpenRecording in the FileIO mailbox.
5. FileIO `run()` -> `process_request()` -> `openRecordFile()` -> `post_result(RecordingOpened, operation_id)`.
6. Main `handleWorkerResult()` -> `handle_recording_result()` opens the recording buffer gate and updates state/LEDs.
7. AudioDSP `store_recording_block()` pushes processed samples; FileIO `write_recording()` pops and saves them.
8. Main queues the completion/state replies; Network sends them; GUI `_handleCommandResult()` clears pending.

`operation_id` connects a worker result to the request that caused it. It is not a file ID or the gain code. The public class-diagram methods remain available; the extra private functions only separate their internal steps.

### Why the fractional gain codes are 101, 102 and 103

The protocol originally used integer values 1–20 to mean 1x–20x. SET_GAIN carries an integer and the State gain field is one byte. The three added fractional choices use unused identifiers without changing those layouts:

| C++ name | Dart name | Transmitted value | Actual multiplier |
|---|---|---|---|
| GAIN_HALF | gainHalf | 101 | 0.5x |
| GAIN_THREE_QUARTERS | gainThreeQuarters | 102 | 0.75x |
| GAIN_FOUR_FIFTHS | gainFourFifths | 103 | 0.8x |

`gain_multiplier()` performs the conversion on the Pi. `_gainLabel()` displays the real value in the GUI. Code 102 therefore never means multiplying audio by 102. These identifiers preserve compatibility; they are a protocol choice rather than an audio-processing requirement.


## Verification of this readability pass

The native tests cover filter response, framing/gain values, buffer gates/partial blocks, pthread synchronization, FFT scaling, WAV round-trip/finalization and LED mask bytes. `controller-tcp-test` additionally runs the real server with ALSA null and temporary storage to exercise commands across all four execution contexts, including playback EOF and return to live. The EOF regression uses an empty valid WAV to isolate the transition because ALSA null has no real-time pacing; the nonempty WAV round-trip is tested separately by runtime-test. The integration checks found a missing output preparation after snd_pcm_drain; AudioDSP now checks snd_pcm_prepare before reporting successful EOF. ALSA's stream states are documented at https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html.

Flutter protocol tests check command encoding, fragmented frames, all supported gain codes, file IDs and float decoding. Cross-compilation checks the Raspberry Pi executable. These checks do not replace listening tests, scheduling checks and GPIO/storage tests on the actual Pi.
