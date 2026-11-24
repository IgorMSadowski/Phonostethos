/**
 * AudioEngine.h - implements th_AudioDSP: live capture, filtered/gained output, stored playback and FFT publication.
 *
 * CONNECTIONS: AudioEngine.h declares the class; AudioEngine.cpp implements it. SystemController owns this object and passes DspProcessor, CommandQueue, DataQueue and both CircularBuffers by reference. Main posts mailbox requests; AudioDSP returns results through CommandQueue.
 *
 * FUNCTIONS:
 * - initAlsa(): open and configure microphone capture and headphone output for mono PCM16 at 8 kHz.
 * - readFrames(): read capture frames with ALSA and return the actual count or negative error.
 * - writeFrames(): write headphone frames with ALSA, accounting for partial transfers.
 * - suspendCapture(): drop pending microphone capture before stored playback begins.
 * - resumeCapture(): reopen and configure microphone capture after stored playback finishes.
 * - ready(): read the atomic audio-initialization readiness flag.
 * - start(): create this worker pthread once and retain its handle for joining.
 * - stop(): request teardown and join workers before destroying their shared resources.
 * - request(): put one control record into the worker mailbox if its slot is free.
 * - apply_output_gain(): ramp gain changes, then call the PCM16 gain/limiter operation.
 * - write_output_block(): send processed samples to the headphones and report unrecoverable output failure.
 * - configure_pcm(): negotiate ALSA format, sample rate, period and buffer parameters.
 * - recover_pcm(): recover an ALSA xrun/suspend error before processing more frames.
 * - set_audio_thread_policy(): request CPU 1 affinity and SCHED_FIFO priority 80.
 * - run(): execute this class's thread loop in the order shown in its flowchart.
 * - post_result(): construct a MainMessage and enqueue completion or error through CommandQueue.
 * - publish_visualization(): compute a new FFT and offer its waveform/spectrum to DataQueue without waiting.
 * - store_recording_block(): push processed samples into the recording buffer and report an overrun.
 * - take_request(): copy a mailbox request locally and free its slot under the mailbox mutex.
 * - apply_request(): apply an AudioDSP control transition after releasing the mailbox mutex.
 * - threadEntry(): recover the object pointer passed to pthread_create and call its worker loop.
 */

// API DOCUMENTATION FOR THIS FILE:
// ALSA PCM: stream setup, capture, output and recovery:
// https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html
// POSIX threads: creation, joining, mutexes and condition variables:
// https://man7.org/linux/man-pages/man7/pthreads.7.html
// pthread mutex lock, try-lock and unlock:
// https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
// Linux scheduling policies and real-time priorities:
// https://man7.org/linux/man-pages/man7/sched.7.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include "phonostethos/CircularBuffer.h" // PCM16 ring buffer used for recording and playback audio streams
#include "phonostethos/CommandQueue.h" // Thread-safe queue for sending worker events/errors to Main
#include "phonostethos/DataQueue.h" // Thread-safe queue for sending FFT/waveform telemetry to Network
#include "phonostethos/DspProcessor.h" // Digital signal processing (bandpass filter, gain limiter, FFT)

#include <alsa/asoundlib.h> // ALSA PCM audio interface types and functions (snd_pcm_t, etc.)

#include <atomic> // Atomic types (std::atomic) for atomic shared-state flags and counters

#include "phonostethos/ThreadSupport.h" // POSIX thread/mutex helpers and project audio constants

using namespace std; // Bring standard library symbols into scope

namespace phono_app { // Open the main PhonoStethos application namespace
// ALSA owner and th_AudioDSP execution context; uses Main-owned DSP and buffers.
class AudioEngine { // Manages real-time ALSA capture/playback and coordinates DSP/telemetry on a dedicated thread
public: // The class operations callable by other code.
    enum class RequestKind { // Give the following named choices distinct typed values.
        SUSPEND_CAPTURE, // Pause the microphone while FileIO prepares a WAV.
        RESUME_CAPTURE,  // Return to live microphone capture after playback.
        START_PLAYBACK,  // Start popping samples from playback_buffer_.
        STOP_PLAYBACK,   // Stop using WAV samples before capture is resumed.
    }; // End this block, declaration or argument list.
    struct Request { // Single control request dispatched from SystemController to AudioEngine
        RequestKind kind; // Type of state transition requested
        uint32_t operation_id; // Correlation ID used to match worker replies with a pending operation
    }; // End this block, declaration or argument list.

    AudioEngine(const Options& options, DspProcessor& dsp, atomic<unsigned>& mode, // Call AudioEngine with the arguments shown here.
                atomic<unsigned>& gain, atomic<bool>& recording, // Declare gain for this operation.
                atomic<unsigned>& error_code, CommandQueue& messages, // Declare error code for this operation.
                DataQueue& telemetry, pthread_mutex_t& configuration_mutex, // Declare telemetry for this operation.
                CircularBuffer& recording_buffer, CircularBuffer& playback_buffer, // Declare recording buffer for this operation.
                atomic<uint32_t>& dropped_telemetry, // Declare dropped telemetry for this operation.
                atomic<uint32_t>& recording_overruns); // Declare recording overruns for this operation.
    ~AudioEngine(); // Complete the call above with these remaining arguments.

    // REPORT 3.4.2: ALSA operations, executed only by th_AudioDSP.
    bool initAlsa(); // Open and configure microphone capture and headphone output for mono PCM16 at 8 kHz.
    int readFrames(int16_t* buffer, int frames); // Read capture frames with ALSA and return the actual count or negative error.
    int writeFrames(const int16_t* buffer, int frames); // Write headphone frames with ALSA, accounting for partial transfers.
    bool suspendCapture(); // Drop pending microphone capture before stored playback begins.
    bool resumeCapture(); // Reopen and configure microphone capture after stored playback finishes.

    // Lifecycle and mailbox API: Main starts the worker and posts a Request.
    bool ready() const; // Read the atomic audio-initialization readiness flag.
    bool start(); // Create this worker pthread once and retain its handle for joining.
    void stop(); // Request teardown and join workers before destroying their shared resources.
    bool request(Request request); // Put one control record into the worker mailbox if its slot is free.

private: // Internal helpers and state used by this class.
    // Worker loop follows the thread flowchart.
    void run(); // Main real-time audio processing loop executed by the worker thread

    const Options& options_; // PCM device names and runtime options
    atomic<unsigned>& mode_; // Selected filter code; Main writes it and AudioDSP reads a configuration snapshot.
    atomic<unsigned>& gain_; // Selected gain CODE, not necessarily the numeric multiplier; convert 101/102/103 before DSP.
    atomic<bool>& recording_; // Shared recording-enabled flag; the buffer gate is checked under its own mutex too.
    atomic<unsigned>& error_code_; // Current protocol error value sent in server status/state.
    // QUEUE REFERENCES: these refer to the same objects owned by SystemController.
    // A reference (&) shares an existing object; it does not create another queue.
    // RESULT QUEUE: build a MainMessage and send it through the reference below.
    CommandQueue& messages_; // AudioDSP enqueues results/errors; Main dequeues them.
    // VISUALIZATION QUEUE: FFT publishing uses this single latest-snapshot channel.
    DataQueue& telemetry_;   // AudioDSP publishes waveform/FFT; Network takes it.
    pthread_mutex_t& configuration_mutex_; // Protect the mode/gain pair while Main publishes and AudioDSP copies it.
    // AUDIO BUFFERS: these references point to the two Main-owned sample buffers.
    CircularBuffer& recording_buffer_; // AudioDSP pushes; FileIO pops recording samples.
    CircularBuffer& playback_buffer_; // Ring buffer fed by FileIoManager playback
    atomic<uint32_t>& dropped_telemetry_; // Count visualization updates skipped when the channel is busy or disconnected.
    atomic<uint32_t>& recording_overruns_; // Count failed recording-buffer pushes; those failures must remain visible.
    DspProcessor& dsp_; // DSP processor owned by Main, invoked by the audio worker
    atomic<bool> ready_{false}; // Indicates whether ALSA setup succeeded and audio loop is active
    atomic<bool> stop_requested_{false}; // Signals worker and loops to terminate
    // ================= MAILBOX: Main -> AudioDSP =================
    // Storage is ONE Request struct + an occupied flag, protected by a mutex.
    // SEND: Main calls request(): lock, reject if occupied, copy, set flag, unlock.
    // TAKE: take_request() locks, copies the record locally, clears the flag, unlocks.
    // DO:   apply_request() executes that local copy outside the mailbox lock.
    // AudioDSP checks this slot at a block boundary or on its idle-loop pass;
    // this mailbox has no separate condition variable or waiting request queue.
    pthread_mutex_t request_mutex_{}; // Protect the AudioDSP request struct together with its occupied flag.
    Request request_{};              // The one shared control-message slot.
    bool has_request_ = false;       // true = unprocessed request; false = free.
    // ============================================================
    snd_pcm_t* capture_handle_ = nullptr; // ALSA capture PCM handle owned by the worker thread
    snd_pcm_t* playback_handle_ = nullptr; // ALSA playback PCM handle owned by the worker thread
    atomic<snd_pcm_t*> capture_pcm_{nullptr}; // Atomic pointer allowing stop() to call snd_pcm_drop safely
    pthread_mutex_t capture_mutex_{}; // Synchronizes stop() with resumeCapture()/shutdown on capture PCM
    pthread_t worker_{}; // POSIX pthread handle created by start and later passed to pthread_join.
    bool worker_started_ = false; // Track whether a pthread was actually created and must be joined.
    // ==================== PRIVATE HELPER METHODS ====================
    void apply_output_gain(float* filtered, int16_t* output, // Pass this next argument/value to the call or initializer above.
                           snd_pcm_sframes_t count, unsigned gain_code, // Declare count for this operation.
                           float& applied_gain); // Declare applied gain for this operation.
    void write_output_block(const int16_t* output, snd_pcm_sframes_t count); // Send processed samples to the headphones and report unrecoverable output failure.
    static int configure_pcm(snd_pcm_t* pcm); // Applies hardware/software PCM parameters and prepares the stream
    static bool recover_pcm(snd_pcm_t* pcm, snd_pcm_sframes_t error); // Recovers PCM stream from xrun/suspend errors
    void set_audio_thread_policy(); // Pins worker to CPU 1 and attempts SCHED_FIFO real-time priority 80
    void post_result(WorkerResultCode code, uint32_t operation_id, uint8_t error = 0); // Construct a MainMessage and enqueue completion or error through CommandQueue.
    void publish_visualization(const int16_t* output, snd_pcm_sframes_t count); // Compute a new FFT and offer its waveform/spectrum to DataQueue without waiting.
    void store_recording_block(const int16_t* output, snd_pcm_sframes_t count); // Push processed samples into the recording buffer and report an overrun.
    bool take_request(Request& request); // Called by run(): true means a record was taken.
    bool apply_request(Request request, bool& playback_active); // Execute the taken record.
    static void* threadEntry(void* context); // Static pthread start routine forwarding to AudioEngine::run()

}; // End this block, declaration or argument list.
}  // namespace phono_app // Close phono_app namespace
