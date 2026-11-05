# How PhonoStethos works

Start here if the source feels difficult to follow. You do not need to understand every error branch before understanding the normal recording path.

## 1. Read these files in this order

1. `app/src/main.cpp`: reads options, creates SystemController, starts the program.
2. `app/src/SystemController.cpp`: read `initSystem()`, `run()`, `processCommand()`, `startRecording()` and `stopRecording()` first. Leave `handleWorkerResult()` until you understand the normal path.
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

`current_command_` retains the last validated command while Main calls the no-argument diagram methods `startPlayback()` and `listFiles()`. Only Main accesses it; it is not another queue.

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

A mailbox here is an `optional<Request>` protected by a pthread mutex. It is a single slot, not another class and not a queue of many requests. `request()` fails if that slot is occupied. Main can retain a deferred request and retry when the worker has taken the previous one.

The two CircularBuffer objects are separate instances of the same template class. Each has its own array, head, tail, sample count, stop flag and mutex. Both hold 4096 samples.

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

The existing filter implementation has a second-order high-pass stage followed by a second-order low-pass stage (fourth order overall). The existing frequency profiles and coefficients were retained. The report's phrase "second-order band-pass" should describe the implemented cascade accurately.

The FFT uses 1024 real samples, advances by 512 samples and first becomes ready after two valid blocks. It applies a Hann window to a separate copy. The unwindowed waveform is normalized PCM16. FFTW produces 513 complex values; the GUI receives magnitudes for bins 0 through 511, omitting the Nyquist bin.

## 8. Recording and Stop

`CircularBuffer::push()` checks the recording gate under the same mutex used to close that gate. It either accepts a whole block or reports that it was not stored; it never overwrites old recording samples.

FileIO calls `pop()`, which waits for samples if needed and returns their actual count. The buffer lock is already released when FileIO calls libsndfile. A slow write therefore does not hold the audio buffer mutex.

On STOP_REC, Main closes the recording gate first. FileIO writes the samples already accepted, including a final partial block, then checks synchronization and WAV closure. Main publishes completion after the FileIO result. A new recording starts with an empty buffer.

A normal push rejected because Stop already closed the gate is not an overflow. A real recording overflow stops that recording and reports an error. FileIO also reports a failed disk write directly rather than waiting for an audio-side overflow to reveal it.

## 9. Playback

FileIO opens and validates the selected mono 8 kHz PCM16 WAV, fills the playback buffer and reports readiness. Main also requests AudioDSP to suspend microphone capture. After preparation, AudioDSP pops playback samples and writes them to headphones. Stored audio bypasses the live filter/gain; FFT analysis uses the samples being played.

FileIO reads only as many samples as fit in the buffer. When it reaches EOF it closes the producer gate. AudioDSP consumes the remaining samples before reporting completion. If the buffer is unexpectedly empty before EOF, AudioDSP emits a silence block and reports a read/underflow fault to Main. Main coordinates file cleanup and return to live capture.

## 10. Why Main has a pending operation

Opening a file, preparing playback, closing a WAV and unmounting storage take time. Main must keep receiving results while those operations run.

`pending_` is Main's record of the current unfinished GUI operation. `operation_id` identifies that request so an old worker reply cannot complete a newer operation. `file_done` and `audio_done` record which worker has answered. `WorkflowStage` uses names instead of numeric phases: WaitingForWorkers, ResumingCapture and EjectingStorage.

This explains most of `handleWorkerResult()`: different results advance the current operation until Main can publish a final COMPLETED or FAILED response. The branches are needed because playback and eject involve more than one worker.

## 11. LEDs and the kernel boundary

Main creates the complete LED mask: Heart 01, Lung 02, General 04 and Recording 08. For example, Heart plus Recording is 09. Stopping recording removes bit 08 without removing the mode bit.

LedDriver writes one byte to `/dev/phonostethos_leds`. The kernel module receives that byte and uses GPIO descriptors from the device-tree overlay. It sets GPIO17, GPIO27, GPIO24 and GPIO22 respectively. FF requests a 100 ms error pulse, after which the cached mode/recording mask is restored.

A successful write means the driver interface accepted the command. There is no electrical feedback to prove an LED physically illuminated.

See `DEPLOYMENT.md` for the required GPIO overlay boot setting. The module uses the kernel gpiod API; it does not require the user-space libgpiod library.

## 12. Libraries versus project methods

| Library / facility | Used for |
|---|---|
| Pthreads | Worker creation, joining, affinity/scheduling, mutexes and condition variables |
| ALSA | Opening/configuring PCM streams, capture, headphone output and XRUN recovery |
| FFTW3f | Planning and executing the single-precision real-to-complex FFT |
| libsndfile | Reading/writing PCM16 WAV files and finalizing their headers |
| libudev | Detecting storage add/remove events in Main |
| POSIX sockets | TCP bind/listen/accept, receive, partial sends, polling and timeouts |
| Kernel GPIO descriptor API | Acquiring and driving the four LED outputs |
| Flutter / Dart / fl_chart | Laptop dashboard, TCP client, frame decoding and charts |

`readFrames()` is a project method that calls the external ALSA API `snd_pcm_readi()`. `computeFFT()` is a project method that calls `fftwf_execute()`. The distinction is useful when explaining API-call boxes in flowcharts.
