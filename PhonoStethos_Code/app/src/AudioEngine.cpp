#include "phonostethos/AudioEngine.h" // Include the AudioEngine class declaration and related types

#include "phonostethos/protocol.h" // Include protocol definitions (error codes, gain helpers, etc.)

#include <algorithm> // Provide standard algorithms such as std::min
#include <array> // Provide fixed-size std::array containers
#include <cerrno> // Provide errno constants such as -EINVAL, -EPIPE, and -EIO
#include <cmath> // Provide math functions
#include <cstring> // Provide strerror for converting error numbers into messages
#include <iostream> // Provide std::cerr for logging errors to standard error
#include <pthread.h> // Provide POSIX threads, mutexes, CPU affinity, and scheduling APIs
#include <sched.h> // Provide scheduling policies and structures such as SCHED_FIFO

using namespace std; // Bring standard library identifiers into the current scope

namespace phono_app { // Open the main PhonoStethos application namespace
AudioEngine::AudioEngine( // Construct AudioEngine with configuration, DSP, atomics, queues, and ring buffers
    const Options& options, DspProcessor& dsp, atomic<unsigned>& mode, atomic<unsigned>& gain, // Options, DSP processor, filter mode, and gain
    atomic<bool>& recording, atomic<unsigned>& error_code, // Recording flag and global error code
    CommandQueue& messages, DataQueue& telemetry, // Control message queue and telemetry queue
    pthread_mutex_t& configuration_mutex, // Mutex protecting shared mode/gain configuration
    CircularBuffer& recording_buffer, // Ring buffer for recorded samples
    CircularBuffer& playback_buffer, // Ring buffer for playback samples
    atomic<uint32_t>& dropped_telemetry, // Atomic counter for dropped telemetry packets
    atomic<uint32_t>& recording_overruns) // Atomic counter for recording buffer overruns
    : options_(options), // Store reference to runtime options
      mode_(mode), // Store reference to atomic filter mode
      gain_(gain), // Store reference to atomic gain code
      recording_(recording), // Store reference to atomic recording flag
      error_code_(error_code), // Store reference to atomic error code
      messages_(messages), // Store reference to main control queue
      telemetry_(telemetry), // Store reference to telemetry queue
      configuration_mutex_(configuration_mutex), // Store reference to configuration mutex
      recording_buffer_(recording_buffer), // Store reference to recording ring buffer
      playback_buffer_(playback_buffer), // Store reference to playback ring buffer
      dropped_telemetry_(dropped_telemetry), // Store reference to dropped telemetry counter
      recording_overruns_(recording_overruns), // Store reference to recording overrun counter
      dsp_(dsp) { // Store reference to DspProcessor
    initializeMutex(request_mutex_); // Initialize mutex guarding the single-slot request mailbox
    initializeMutex(capture_mutex_); // Initialize mutex guarding capture PCM teardown/reopen
}

AudioEngine::~AudioEngine() { // Destroy AudioEngine instance
    stop(); // Stop worker thread and wait for completion before cleanup
    /*
     * pthread_mutex_destroy:
     * Releases resources associated with an initialized, unlocked mutex.
     */
    pthread_mutex_destroy(&request_mutex_); // Destroy the request mailbox mutex
    pthread_mutex_destroy(&capture_mutex_); // Destroy the capture PCM mutex
}

// pthread callback: context is the AudioEngine whose start() created this worker.
void* AudioEngine::threadEntry(void* context) { // Static entry point passed to pthread_create
    static_cast<AudioEngine*>(context)->run(); // Cast context back to AudioEngine* and run the worker loop
    return nullptr; // Return null pointer upon thread exit
}

bool AudioEngine::start() { // Spawn the audio worker thread
    if (worker_started_) {
        return true; // Avoid creating a second thread if already started
    }
    /*
     * pthread_create:
     * Starts a new thread in the current process executing threadEntry with `this` as argument.
     * Returns 0 on success or a non-zero error number on failure.
     */
    /* API: pthread_create
     * Start a POSIX worker. Pass its pthread_t address, attributes (NULL for defaults), entry callback and context pointer. Returns 0 or an error number.
     */
    const int result = pthread_create(&worker_, nullptr, &AudioEngine::threadEntry, this); // Create worker thread
    if (result != 0) { // Check if thread creation failed
        cerr << "Cannot start AudioEngine: " << strerror(result) << '\n'; // Report error to stderr
        return false; // Indicate startup failure
    }
    worker_started_ = true; // Mark worker thread as running
    return true; // Indicate startup success
}

// A mailbox holds one unprocessed control request; Main never changes ALSA directly.
// CONTROL: Main deposits one request here; run() takes it before the next block.
bool AudioEngine::request(Request request) { // Queue a single control request for the audio thread
    PthreadLock lock(request_mutex_); // Lock request_mutex_ for the duration of this scope
    if (stop_requested_.load() || has_request_) {
        return false; // Reject if stopping or mailbox is full
    }
    request_ = request;
    has_request_ = true; // Store pending request in the mailbox
    return true; // Request accepted
}

bool AudioEngine::ready() const { return ready_.load(); } // Atomically check whether ALSA initialization completed

void AudioEngine::stop() { // Request worker shutdown and join the thread
    stop_requested_.store(true); // Signal worker loops to stop
    { // Limit lock lifetime to the capture PCM drop check
        PthreadLock lock(capture_mutex_); // Lock capture_mutex_ while reading capture_pcm_
        snd_pcm_t* capture = capture_pcm_.load();
        if (capture != nullptr) { // Check if capture PCM handle is active
            /*
             * snd_pcm_drop (ALSA):
             * Immediately stops the PCM stream and discards buffered frames without waiting for drain,
             * unblocking any active read/write call on the device.
             */
            /* API: snd_pcm_drop
             * Stop this ALSA stream immediately and discard queued frames. This also helps unblock capture during maintenance shutdown.
             */
            (void)snd_pcm_drop(capture); // Drop capture PCM to unblock any pending read
        } // Release capture_mutex_
    }
    if (worker_started_) { // Join only if the thread was started
        /*
         * pthread_join:
         * Blocks the calling thread until the target thread (`worker_`) terminates.
         */
        /* API: pthread_join
         * Wait for this worker to return before releasing the objects it uses. Pass the pthread handle and NULL when its return value is unused.
         */
        pthread_join(worker_, nullptr); // Wait for audio worker thread to exit
        worker_started_ = false; // Reset started flag
    }
}

int AudioEngine::configure_pcm(snd_pcm_t* pcm) { // Configure ALSA hardware and software parameters for a PCM stream
    snd_pcm_hw_params_t* params; // Pointer to ALSA hardware parameter container
    /*
     * snd_pcm_hw_params_alloca & snd_pcm_hw_params_any (ALSA):
     * Allocates a hardware parameter structure on the stack and fills it with the full
     * configuration space supported by the PCM device.
     */
    snd_pcm_hw_params_alloca(&params); // Allocate hw_params on stack
    int err = snd_pcm_hw_params_any(pcm, params); // Populate full hardware parameter space
    if (err < 0) {
        return err; // Return negative ALSA error code on failure
    }
    /*
     * snd_pcm_hw_params_set_access / set_format / set_channels (ALSA):
     * Constrains the PCM stream to interleaved read/write access, signed 16-bit little-endian
     * samples (SND_PCM_FORMAT_S16_LE), and mono audio (1 channel).
     */
    if ((err = snd_pcm_hw_params_set_access( // Set interleaved access mode
             pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 || // Check for error
        (err = snd_pcm_hw_params_set_format( // Set 16-bit signed little-endian sample format
             pcm, params, SND_PCM_FORMAT_S16_LE)) < 0 || // Check for error
        (err = snd_pcm_hw_params_set_channels(pcm, params, 1)) < 0) { // Set single mono channel
        return err; // Return failure code
    }
    unsigned rate = kSampleRate; // Desired sample rate in Hz
    /*
     * snd_pcm_hw_params_set_rate_near (ALSA):
     * Sets the sample rate closest to `rate` and updates `rate` with the actual rate chosen.
     */
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr)) < // Negotiate sample rate
        0) { // Check for error
        return err; // Return failure code
    }
    if (rate != kSampleRate) {
        return -EINVAL; // Reject if exact sample rate could not be matched
    }
    snd_pcm_uframes_t period = kPeriodFrames; // Desired period size in frames
    /*
     * snd_pcm_hw_params_set_period_size_near & set_buffer_size_near (ALSA):
     * Negotiates the period size (interrupt/transfer granularity) and total ring buffer size
     * (set here to 4 periods).
     */
    if ((err = snd_pcm_hw_params_set_period_size_near( // Negotiate period size near kPeriodFrames
             pcm, params, &period, nullptr)) < 0) { // Check for error
        return err; // Return failure code
    }
    snd_pcm_uframes_t buffer = period * 4; // Total hardware buffer size in frames
    if ((err = snd_pcm_hw_params_set_buffer_size_near( // Negotiate buffer size near 4 periods
             pcm, params, &buffer)) < 0) { // Check for error
        return err; // Return failure code
    }
    /*
     * snd_pcm_hw_params (ALSA):
     * Commits the selected hardware parameters to the PCM device and prepares the hardware.
     */
    /* API: snd_pcm_hw_params
     * Apply the requested ALSA hardware format and buffering. set_* calls fill the parameter structure first. near calls may negotiate a supported value.
     */
    if ((err = snd_pcm_hw_params(pcm, params)) < 0) {
        return err; // Apply hardware parameters
    }
    snd_pcm_sw_params_t* software; // Pointer to ALSA software parameter container
    /*
     * snd_pcm_sw_params_alloca / current / set_avail_min / set_stop_threshold /
     * set_start_threshold / snd_pcm_sw_params (ALSA):
     * Configures software ring-buffer thresholds: minimum available frames before waking I/O,
     * automatic stop threshold on xrun, and playback start threshold.
     */
    snd_pcm_sw_params_alloca(&software); // Allocate sw_params on stack
    if ((err = snd_pcm_sw_params_current(pcm, software)) < 0 || // Read current software params
        (err = snd_pcm_sw_params_set_avail_min(pcm, software, period)) < 0 || // Wake when at least 1 period is ready
        (err = snd_pcm_sw_params_set_stop_threshold( // Stop automatically at buffer boundary on xrun
             pcm, software, buffer)) < 0) { // Pass buffer size
        return err; // Return failure code
    }
    if (snd_pcm_stream(pcm) == SND_PCM_STREAM_PLAYBACK && // Apply start threshold only to playback streams
        (err = snd_pcm_sw_params_set_start_threshold( // Delay playback start until 1 period is buffered
             pcm, software, period)) < 0) { // Pass period size
        return err; // Return failure code
    }
    /* API: snd_pcm_sw_params
     * Apply ALSA software thresholds that control when a stream starts and when enough frames are available.
     */
    if ((err = snd_pcm_sw_params(pcm, software)) < 0) {
        return err; // Commit software parameters
    }
    /*
     * snd_pcm_prepare (ALSA):
     * Moves the PCM stream from SETUP/XRUN/DROPPED into PREPARED state so I/O can begin.
     */
    /* API: snd_pcm_prepare
     * Prepare an ALSA stream for transfers after configuration or recovery.
     */
    return snd_pcm_prepare(pcm); // Prepare PCM device for operation
}

bool AudioEngine::recover_pcm(snd_pcm_t* pcm, snd_pcm_sframes_t error) { // Attempt recovery from ALSA I/O errors
    if (error == -EPIPE || error == -ESTRPIPE) { // Handle buffer overrun/underrun (-EPIPE) or suspend (-ESTRPIPE)
        /*
         * snd_pcm_recover (ALSA):
         * Recovers a PCM stream from specific recoverable error states such as -EPIPE (xrun)
         * or -ESTRPIPE (suspended). Passing 1 silences internal ALSA error messages.
         */
        /* API: snd_pcm_recover
         * Ask ALSA to recover an xrun or suspended stream. Supply the stream and original negative error. Check the recovery result before continuing.
         */
        const int result = snd_pcm_recover(pcm, static_cast<int>(error), 1); // Recover stream state
        if (result < 0) { // Check if recovery failed
            /*
             * snd_strerror (ALSA):
             * Translates a negative ALSA error code into a human-readable string.
             */
            cerr << "ALSA recovery: " << snd_strerror(result) << '\n'; // Log recovery failure
            return false; // Recovery failed
        }
        return true; // Recovery succeeded
    }
    cerr << "ALSA PCM: " << snd_strerror(static_cast<int>(error)) << '\n'; // Log non-recovered ALSA error
    return false; // Indicate unhandled/unrecovered error
}

void AudioEngine::set_audio_thread_policy() { // Pin audio thread to CPU 1 and configure real-time FIFO scheduling
    cpu_set_t cpus; // CPU affinity bitmask
    /*
     * CPU_ZERO & CPU_SET (POSIX/glibc):
     * Clears a `cpu_set_t` bitmask and sets bit 1 so the thread can run only on CPU core 1.
     */
    CPU_ZERO(&cpus); // Clear CPU mask
    CPU_SET(1, &cpus); // Add CPU core 1 to the mask
    /*
     * pthread_setaffinity_np (POSIX/glibc):
     * Restricts the specified thread to execute only on the CPUs set in `cpus`.
     */
    const int affinity_result = // Apply CPU affinity to current thread
        /* API: pthread_setaffinity_np
         * Ask Linux to run this pthread on the CPUs in cpu_set_t. pthread_self identifies the current worker. Check the returned error number.
         */
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus); // Set affinity for self
    if (affinity_result != 0) { // Check if setting CPU affinity failed
        cerr << "Audio thread CPU affinity not set: " // Log warning to stderr
                  << strerror(affinity_result) << '\n'; // Print error description
    }
    sched_param priority{}; // Zero-initialized scheduling parameter structure
    priority.sched_priority = 80; // Set real-time priority level to 80
    /*
     * pthread_setschedparam (POSIX):
     * Sets the scheduling policy (SCHED_FIFO) and priority parameters for the given thread.
     */
    const int scheduling_result = // Attempt to enable SCHED_FIFO real-time scheduling
        /* API: pthread_setschedparam
         * Request the scheduling policy and sched_priority stored in sched_param. FIFO 80 is AudioDSP and FIFO 50 is FileIO. SCHED_OTHER requires priority 0.
         */
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority); // Apply policy and priority
    if (scheduling_result != 0) { // May fail if process lacks CAP_SYS_NICE or rlimit permissions
        cerr << "Audio thread SCHED_FIFO unavailable: " // Log notice to stderr
                  << strerror(scheduling_result) << '\n'; // Print error description
    }
}

void AudioEngine::post_result(WorkerResultCode code, uint32_t operation_id, // Enqueue a worker result/error to Main
                              uint8_t error) { // Error code (0 means no error)
    MainMessage message; // Message structure consumed by SystemController
    message.kind = MessageKind::WorkerResult;
    if (error != 0) {
        message.kind = MessageKind::WorkerError;
    }
    message.operation_id = operation_id; // Set matching operation identifier
    message.result_code = code; // Set worker result code
    message.error_code = error; // Set error code
    messages_.enqueue(move(message)); // Push message into thread-safe command queue
}

// Only AudioDSP executes these ALSA transitions, at an audio-block boundary.
bool AudioEngine::apply_request(Request request, // Apply a control request inside the audio worker
                               bool& playback_active) { // Reference to local playback state flag
    using phono::protocol::Error; // Bring protocol Error enum into local scope
    switch (request.kind) { // Dispatch based on request kind
        case RequestKind::SuspendCapture: { // Suspend microphone capture
            const int result = suspendCapture() ? 0 : -EIO; // Map suspend failure to -EIO
            if (result < 0) { // Check if suspending failed
                error_code_.store(static_cast<unsigned>(Error::MicLost)); // Record MicLost error
                post_result(WorkerResultCode::CaptureSuspended, // Post CaptureSuspended result
                            request.operation_id, // Include operation_id
                            static_cast<uint8_t>(Error::MicLost)); // Include MicLost error code
                return false; // Indicate failure
            }
            dsp_.reset(); // Clear DSP history and filter state
            post_result(WorkerResultCode::CaptureSuspended, // Notify successful capture suspension
                        request.operation_id); // Include operation_id
            return true; // Indicate success
        }
        case RequestKind::ResumeCapture: { // Resume microphone capture
            const int result = resumeCapture() ? 0 : -EIO; // Reopen/configure capture; map failure to -EIO
            if (result < 0) { // Check if reopening capture failed
                cerr << "Cannot reopen capture PCM '" // Log error prefix
                     << options_.capture_pcm << "': " << snd_strerror(result) // Log PCM name and ALSA error
                     << '\n'; // End line
                error_code_.store(static_cast<unsigned>(Error::MicLost)); // Record MicLost error
                post_result(WorkerResultCode::CaptureResumed, // Post CaptureResumed result
                            request.operation_id, // Include operation_id
                            static_cast<uint8_t>(Error::MicLost)); // Include MicLost error code
                return false; // Indicate failure
            }
            dsp_.reset(); // Reset DSP state after resuming capture
            post_result(WorkerResultCode::CaptureResumed, // Notify successful capture resume
                        request.operation_id); // Include operation_id
            return true; // Indicate success
        }
        case RequestKind::StartPlayback: // Switch worker into playback mode
            playback_active = true; // Enable playback mode flag
            dsp_.reset(); // Reset DSP state before playing audio
            post_result(WorkerResultCode::PlaybackStarted, // Notify that playback started
                        request.operation_id); // Include operation_id
            return true; // Indicate success
        case RequestKind::StopPlayback: // Exit playback mode
            playback_active = false; // Disable playback mode flag
            dsp_.reset(); // Reset DSP state when leaving playback
            post_result(WorkerResultCode::PlaybackStopped, // Notify that playback stopped
                        request.operation_id); // Include operation_id
            return true; // Indicate success
    }
    return false; // Fallback return for unhandled request kinds
}

// Open/configure capture and headphones inside the audio worker.
bool AudioEngine::initAlsa() { // Open and configure capture and playback PCM devices
    using phono::protocol::Error; // Bring protocol Error enum into local scope
    /*
     * snd_pcm_open (ALSA):
     * Opens a PCM device by name for either SND_PCM_STREAM_CAPTURE or SND_PCM_STREAM_PLAYBACK.
     * Mode 0 selects default blocking behavior.
     */
    /* API: snd_pcm_open
     * Open a named ALSA PCM stream. Supply a handle address, device name, capture/playback direction and flags. A negative return is an ALSA error.
     */
    int err = snd_pcm_open(&capture_handle_, options_.capture_pcm.c_str(), // Open capture PCM device
                           SND_PCM_STREAM_CAPTURE, 0); // Capture stream, blocking mode
    if (err < 0) { // Check if opening capture PCM failed
        cerr << "Cannot open capture PCM '" << options_.capture_pcm // Print device name
                  << "': " << snd_strerror(err) << '\n'; // Print ALSA error string
        error_code_.store(static_cast<unsigned>(Error::MicLost)); // Record MicLost error
        post_result(WorkerResultCode::AudioReady, 0, // Notify audio initialization failure
                    static_cast<uint8_t>(Error::MicLost)); // Set MicLost error code
        return false; // Initialization failed
    }
    if ((err = configure_pcm(capture_handle_)) < 0) { // Configure hardware and software parameters for capture
        cerr << "Cannot configure capture PCM: " << snd_strerror(err) << '\n'; // Log configuration error
        /*
         * snd_pcm_close (ALSA):
         * Closes an open PCM handle and frees resources associated with it.
         */
        /* API: snd_pcm_close
         * Release the ALSA stream handle after it is no longer in use.
         */
        snd_pcm_close(capture_handle_); // Close capture handle
        capture_handle_ = nullptr; // Clear pointer
        error_code_.store(static_cast<unsigned>(Error::MicLost)); // Record MicLost error
        post_result(WorkerResultCode::AudioReady, 0, // Notify audio initialization failure
                    static_cast<uint8_t>(Error::MicLost)); // Set MicLost error code
        return false; // Initialization failed
    }
    capture_pcm_.store(capture_handle_); // Publish capture handle atomically for stop()

    err = snd_pcm_open(&playback_handle_, options_.playback_pcm.c_str(), // Open playback PCM device
                       SND_PCM_STREAM_PLAYBACK, 0); // Playback stream, blocking mode
    if (err < 0 || (err = configure_pcm(playback_handle_)) < 0) { // Fail if open or configure fails
        cerr << "Cannot open/configure playback PCM '" // Log playback error prefix
                  << options_.playback_pcm << "': " << snd_strerror(err) // Print device name and ALSA error
                  << '\n'; // End line
        if (playback_handle_ != nullptr) snd_pcm_close(playback_handle_); // Close playback handle if opened
        playback_handle_ = nullptr; // Clear playback pointer
        capture_pcm_.store(nullptr); // Clear atomic capture pointer before closing
        snd_pcm_close(capture_handle_); // Close capture handle since audio setup is incomplete
        capture_handle_ = nullptr; // Clear capture pointer
        error_code_.store(static_cast<unsigned>(Error::OutputError)); // Record OutputError
        post_result(WorkerResultCode::AudioReady, 0, // Notify audio initialization failure
                    static_cast<uint8_t>(Error::OutputError)); // Set OutputError code
        return false; // Initialization failed
    }
    return true; // Both capture and playback devices are ready
}

int AudioEngine::readFrames(int16_t* buffer, int frames) { // Read interleaved PCM frames from capture
    /*
     * snd_pcm_readi (ALSA):
     * Reads `frames` interleaved samples from the capture PCM device into `buffer`.
     * Returns the number of frames read, or a negative ALSA error code.
     */
    /* API: snd_pcm_readi
     * Read interleaved PCM frames into the supplied buffer. Mono has one sample per frame. The returned frame count may be short or negative on error.
     */
    return static_cast<int>(snd_pcm_readi(capture_handle_, buffer, frames)); // Read frames and cast count to int
}

int AudioEngine::writeFrames(const int16_t* buffer, int frames) { // Write interleaved PCM frames to playback
    /*
     * snd_pcm_writei (ALSA):
     * Writes `frames` interleaved samples from `buffer` to the playback PCM device.
     * Returns the number of frames written, or a negative ALSA error code.
     */
    /* API: snd_pcm_writei
     * Write interleaved PCM frames to the output. Advance by the actual returned count. A negative result requires error handling or recovery.
     */
    return static_cast<int>(snd_pcm_writei(playback_handle_, buffer, frames)); // Write frames and cast count to int
}

bool AudioEngine::suspendCapture() { // Drop buffered capture frames and stop capture stream
    return capture_handle_ != nullptr && snd_pcm_drop(capture_handle_) >= 0; // Return true if dropped cleanly
}

// Reopening capture also recovers a device that was suspended or reconnected.
bool AudioEngine::resumeCapture() { // Close and reopen capture PCM device
    PthreadLock lock(capture_mutex_); // Serialize against stop() accessing capture_pcm_
    capture_pcm_.store(nullptr); // Clear atomic handle while reopening
    if (capture_handle_ != nullptr) { // Close existing capture handle if open
        const int closed = snd_pcm_close(capture_handle_); // Close old capture PCM handle
        capture_handle_ = nullptr; // Reset pointer
        if (closed < 0) {
            return false; // Return false if closing failed
        }
    }
    int result = snd_pcm_open(&capture_handle_, options_.capture_pcm.c_str(), // Reopen capture PCM device
                              SND_PCM_STREAM_CAPTURE, 0); // Capture stream, blocking mode
    if (result >= 0) result = configure_pcm(capture_handle_); // Reapply hw/sw PCM configuration
    if (result < 0 && capture_handle_ != nullptr) { // Clean up handle if configuration failed
        snd_pcm_close(capture_handle_); // Close partially opened handle
        capture_handle_ = nullptr; // Reset pointer
    }
    capture_pcm_.store(capture_handle_); // Store new handle (or nullptr on failure)
    return result >= 0; // Return true if reopen and configuration succeeded
}

// FLOWCHART: gain + limiter. Both microphone and WAV playback use this step.
// applied_gain is kept between blocks so slider changes do not jump abruptly.
void AudioEngine::apply_output_gain(float* filtered, int16_t* output,
                                    snd_pcm_sframes_t count, unsigned gain_code,
                                    float& applied_gain) {
    // Blend changes over 20 ms so moving the slider cannot jump the waveform.
    const float target_gain = phono::protocol::gain_multiplier(gain_code); // Convert gain code to float scale
    constexpr auto ramp_frames = kSampleRate / 50; // Number of frames in a 20 ms ramp (1/50 s)
    const auto ramp_count = min<snd_pcm_sframes_t>(count, ramp_frames); // Clamp ramp length to current block
    if (ramp_count > 0 && applied_gain != target_gain) { // Ramp gain if gain changed
        const float start_gain = applied_gain; // Save starting gain for interpolation
        for (snd_pcm_sframes_t i = 0; i < ramp_count; ++i) { // Interpolate gain across ramp samples
            const float progress = static_cast<float>(i + 1) / ramp_frames; // Normalized ramp progress
            filtered[i] *= start_gain + (target_gain - start_gain) * progress; // Scale sample by interpolated gain
        }
        applied_gain = start_gain + (target_gain - start_gain) * // Advance applied_gain by fraction ramped
            (static_cast<float>(ramp_count) / ramp_frames); // Fraction of full ramp completed
        if (ramp_count == ramp_frames) applied_gain = target_gain; // Snap to target_gain when ramp finishes
        dsp_.applyGainWithLimiter(filtered, output, // Limit ramped portion using gain = 1.0
                                  static_cast<int>(ramp_count), 1.0F); // First ramp_count samples
        dsp_.applyGainWithLimiter(filtered + ramp_count, // Apply target_gain and limit remainder
                                  output + ramp_count, // Offset output pointer by ramp_count
                                  static_cast<int>(count - ramp_count), target_gain); // Remaining frames
    } else { // Gain is already at target_gain
        dsp_.applyGainWithLimiter(filtered, output, // Apply target_gain and clamp to int16_t
                                  static_cast<int>(count), target_gain); // Entire frame block
    }
}

// FLOWCHART: writeFrames to headphones. ALSA may accept only part of a block.
void AudioEngine::write_output_block(const int16_t* output, snd_pcm_sframes_t count) {
    using phono::protocol::Error;
    snd_pcm_sframes_t written = 0; // Frames written so far to playback PCM
    while (written < count && !stop_requested_.load()) { // Loop until all frames are written
        const auto result = writeFrames(output + written, // Write starting at offset `written`
            static_cast<int>(count - written)); // Remaining frames to write
        if (result < 0) { // Handle playback write error
            if (!recover_pcm(playback_handle_, result)) { // Attempt PCM recovery
                error_code_.store(static_cast<unsigned>(Error::OutputError)); // Record OutputError
                post_result(WorkerResultCode::AudioReady, 0, // Notify audio output failure
                            static_cast<uint8_t>(Error::OutputError)); // OutputError code
                break; // Abort writing current block
            }
        } else { // Frames written successfully
            written += result; // Advance written frame count
        }
    }
}

// FLOWCHART: push recording samples; reject a full buffer without overwriting audio.
void AudioEngine::store_recording_block(const int16_t* output, snd_pcm_sframes_t count) {
    using phono::protocol::Error;
    if (!recording_buffer_.push(output, static_cast<size_t>(count)) && // Check if push failed
        !recording_buffer_.stopped()) { // Failure while not stopped indicates buffer overrun
        recording_.store(false); // Stop recording flag
        recording_buffer_.stop_producing(); // Signal consumer that no more samples will arrive
        recording_overruns_.fetch_add(1); // Increment overrun counter atomically
        error_code_.store(static_cast<unsigned>(Error::RecordOverrun)); // Record RecordOverrun error
        post_result(WorkerResultCode::RecordingClosed, 0, // Notify that recording closed due to overrun
                    static_cast<uint8_t>(Error::RecordOverrun)); // RecordOverrun error code
    }
}

// FLOWCHART: FFT -> DataQueue. This step must not wait for the GUI.
void AudioEngine::publish_visualization(const int16_t* output, snd_pcm_sframes_t count) {
    array<float, kFftBins> magnitudes{};
    // Headphone output and recording come first. FFT is visualization work.
    dsp_.computeFFT(output, magnitudes.data(), static_cast<int>(count)); // Update FFT window and compute bins
    if (dsp_.fftReady()) { // Check if a new FFT frame is ready
        Telemetry telemetry; // Telemetry packet for network thread
        telemetry.waveform = dsp_.waveform(); // Copy prepared waveform samples
        telemetry.spectrum = magnitudes; // Copy computed FFT magnitudes
        if (!telemetry_.try_enqueue(telemetry)) dropped_telemetry_.fetch_add(1); // Count drop if queue is full
    }
}

// FLOWCHART: th_AudioDSP parts 1-3, read in processing order.
void AudioEngine::run() { // Main processing loop for the audio worker thread
    using phono::protocol::Error; // Bring protocol Error enum into local scope
    set_audio_thread_policy(); // Set CPU affinity and scheduling policy
    if (!initAlsa()) {
        return; // Exit thread if ALSA initialization fails
    }
    ready_.store(true); // Mark audio engine as ready
    error_code_.store(0); // Clear initial error state
    post_result(WorkerResultCode::AudioReady, 0); // Inform SystemController that audio is ready

    array<int16_t, kPeriodFrames> input{}; // Input PCM frame buffer for one period
    array<int16_t, kPeriodFrames> output{}; // Output PCM frame buffer for one period
    array<float, kPeriodFrames> floating_input{}; // Float conversion buffer for input samples
    array<float, kPeriodFrames> filtered{}; // Bandpass-filtered float sample buffer
    float applied_gain = phono::protocol::gain_multiplier(gain_.load()); // Current smoothed gain multiplier
    bool capture_suspended = false; // Whether capture is currently suspended
    bool playback_active = false; // Whether playing from file instead of live capture
    bool playback_completion_sent = false; // Tracks whether PlaybackFinished was already posted
    uint32_t playback_operation = 0; // Operation ID of the active playback session
    while (!stop_requested_.load() && running.load()) { // Run until stop requested or global shutdown
        Request request{};
        bool has_request = false; // A plain request record copied from the mailbox
        { // Critical section to pop request from mailbox
            PthreadLock lock(request_mutex_); // Lock request_mutex_
            request = request_;
            has_request = has_request_; // Remember whether the mailbox contained a request
            has_request_ = false; // Clear mailbox slot
        } // Unlock request_mutex_
        if (has_request) { // Process request if one was present
            if (request.kind == RequestKind::SuspendCapture) { // Mark capture suspended
                capture_suspended = true; // Set suspended state
            } else if (request.kind == RequestKind::ResumeCapture) { // Mark capture active
                capture_suspended = false; // Clear suspended state
            } else if (request.kind == RequestKind::StartPlayback) { // Track playback operation ID
                playback_operation = request.operation_id; // Store operation ID
                playback_completion_sent = false; // Reset completion notification flag
            }
            apply_request(request, playback_active); // Execute state transition
        }

        snd_pcm_sframes_t count = 0; // Number of valid frames produced in this iteration
        unsigned gain_code = gain_.load(); // Snapshot current gain code
        if (playback_active) { // Playback path reading from playback_buffer_
            const auto samples = playback_buffer_.pop( // Pop samples from playback ring buffer
                output.data(), output.size(), chrono::milliseconds(10)); // Wait up to 10 ms for data
            count = static_cast<snd_pcm_sframes_t>(samples); // Convert sample count to PCM frames
            if (samples == 0) { // Handle empty pop / timeout
                if (playback_buffer_.stopped() && playback_buffer_.isEmpty()) { // Normal end of stream
                    if (!playback_completion_sent) { // Post completion only once
                        /*
                         * snd_pcm_drain (ALSA):
                         * Blocks until all pending playback frames in the hardware buffer have finished playing.
                         */
                        /* API: snd_pcm_drain
                         * Wait for queued playback frames to finish. Used at normal playback EOF so the last samples can be heard.
                         */
                        int output_result = snd_pcm_drain(playback_handle_);
                        if (output_result >= 0) {
                            /* API: snd_pcm_prepare
                             * drain() finishes the WAV and leaves the output in SETUP.
                             * Prepare it again before live audio writes its next block.
                             * Otherwise snd_pcm_writei returns -EBADFD after normal EOF.
                             */
                            output_result = snd_pcm_prepare(playback_handle_);
                        }
                        if (output_result < 0) { // Check both drain and preparation
                            error_code_.store( // Record OutputError
                                static_cast<unsigned>(Error::OutputError)); // OutputError enum value
                            post_result(WorkerResultCode::PlaybackFinished, // Post PlaybackFinished with error
                                        playback_operation, // Playback operation ID
                                        static_cast<uint8_t>(Error::OutputError)); // OutputError code
                        } else { // Drain succeeded
                            post_result(WorkerResultCode::PlaybackFinished, // Post normal PlaybackFinished
                                        playback_operation); // Playback operation ID
                        }
                        playback_completion_sent = true; // Mark completion sent
                        playback_active = false; // Exit playback mode
                    }
                    continue; // Proceed to next loop iteration
                }
                output.fill(0); // Fill silence if buffer is not cleanly finished
                count = static_cast<snd_pcm_sframes_t>(output.size()); // Output one full period of silence
                if (!playback_completion_sent) { // Post completion with ReadError if not yet sent
                    post_result(WorkerResultCode::PlaybackFinished, // Post PlaybackFinished
                                playback_operation, // Playback operation ID
                                static_cast<uint8_t>(Error::ReadError)); // Indicate read/buffer error
                    playback_completion_sent = true; // Mark completion sent
                    playback_active = false; // Exit playback mode
                }
            }
            for (size_t i = 0; i < static_cast<size_t>(count); ++i) { // Copy playback samples into float buffer
                filtered[i] = static_cast<float>(output[i]); // Convert int16_t to float
            }
        } else if (!capture_suspended) { // Live capture path when capture is active
            while (count < static_cast<snd_pcm_sframes_t>(input.size()) && // Accumulate a full period of frames
                   !stop_requested_.load() && !capture_suspended) { // Stop early if requested or suspended
                const auto received = readFrames(input.data() + count, // Read into remaining space of input
                    static_cast<int>(input.size() - count)); // Request remaining frame count
                if (received < 0) { // Handle capture error
                    if (!recover_pcm(capture_handle_, received)) { // Try recovering capture PCM
                        error_code_.store(static_cast<unsigned>(Error::MicLost)); // Mark microphone as lost
                        post_result(WorkerResultCode::AudioReady, 0, // Notify fatal audio error
                                    static_cast<uint8_t>(Error::MicLost)); // MicLost error code
                        stop_requested_.store(true); // Request thread termination
                        break; // Break out of capture read loop
                    }
                } else { // Frames read successfully
                    count += received; // Advance frame counter
                }
            }
            if (stop_requested_.load()) break; // Exit main loop if stop requested
            if (capture_suspended || count == 0) continue; // Skip processing if suspended or no frames

            unsigned mode; // Local copy of active filter mode
            { // Synchronized read of shared mode and gain
                PthreadLock lock(configuration_mutex_); // Lock configuration_mutex_
                mode = mode_.load(); // Load filter mode
                gain_code = gain_.load(); // Load gain code
            } // Unlock configuration_mutex_
            for (size_t i = 0; i < static_cast<size_t>(count); ++i) { // Convert captured PCM samples to float
                floating_input[i] = static_cast<float>(input[i]); // Store float sample
            }
            dsp_.applyBandpass(floating_input.data(), filtered.data(), // Apply mode-specific bandpass filter
                              static_cast<int>(count), static_cast<FilterMode>(mode)); // Pass sample count and mode
        } else { // Capture is suspended and playback is inactive
            const timespec pause{0, 1000000L}; // Sleep duration of 1 ms (1,000,000 ns)
            nanosleep(&pause, nullptr); // Yield CPU briefly while idle
            continue; // Continue to next loop iteration
        }

        // The same named steps are used by the AudioDSP flowchart.
        apply_output_gain(filtered.data(), output.data(), count, gain_code, applied_gain);
        write_output_block(output.data(), count);
        if (!playback_active && !capture_suspended && recording_.load()) {
            store_recording_block(output.data(), count);
        }
        publish_visualization(output.data(), count);
    }

    ready_.store(false); // Mark audio engine as no longer ready
    recording_.store(false); // Ensure recording flag is false on exit
    playback_buffer_.stop_producing(); // Stop playback buffer production
    { // Lock capture_mutex_ while closing capture PCM
        PthreadLock lock(capture_mutex_); // Acquire capture_mutex_
        capture_pcm_.store(nullptr); // Clear atomic capture pointer
        if (capture_handle_ != nullptr) snd_pcm_close(capture_handle_); // Close capture PCM if open
    } // Release capture_mutex_
    if (playback_handle_ != nullptr) { // Close playback PCM if open
        (void)snd_pcm_drain(playback_handle_); // Drain remaining playback frames before closing
        snd_pcm_close(playback_handle_); // Close playback PCM handle
        playback_handle_ = nullptr; // Clear local/reference playback pointer
    }
    capture_handle_ = nullptr; // Clear local/reference capture pointer
}
}  // namespace phono_app // Close phono_app namespace
