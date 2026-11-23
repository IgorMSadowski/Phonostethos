#pragma once

#include "phonostethos/CircularBuffer.h" // PCM16 ring buffer used for recording and playback audio streams
#include "phonostethos/CommandQueue.h" // Thread-safe queue for sending worker events/errors to Main
#include "phonostethos/DataQueue.h" // Thread-safe queue for sending FFT/waveform telemetry to Network
#include "phonostethos/DspProcessor.h" // Digital signal processing (bandpass filter, gain limiter, FFT)

#include <alsa/asoundlib.h> // ALSA PCM audio interface types and functions (snd_pcm_t, etc.)

#include <atomic> // Atomic types (std::atomic) for lock-free state flags and counters

#include "phonostethos/ThreadSupport.h" // POSIX thread/mutex helpers and project audio constants

using namespace std; // Bring standard library symbols into scope

namespace phono_app { // Open the main PhonoStethos application namespace
// ALSA owner and th_AudioDSP execution context; uses Main-owned DSP and buffers.
class AudioEngine { // Manages real-time ALSA capture/playback and coordinates DSP/telemetry on a dedicated thread
public:
    // Main posts one of these records; AudioDSP takes it at a block boundary.
    enum class RequestKind {
        SuspendCapture, // Pause the microphone while FileIO prepares a WAV.
        ResumeCapture,  // Return to live microphone capture after playback.
        StartPlayback,  // Start popping samples from playback_buffer_.
        StopPlayback,   // Stop using WAV samples before capture is resumed.
    };
    struct Request { // Single control request dispatched from SystemController to AudioEngine
        RequestKind kind; // Type of state transition requested
        uint32_t operation_id; // Correlation ID used to match worker replies with a pending operation
    };

    AudioEngine(const Options& options, DspProcessor& dsp, atomic<unsigned>& mode, // Runtime options, DSP instance, and filter mode
                atomic<unsigned>& gain, atomic<bool>& recording, // Gain code and active recording state
                atomic<unsigned>& error_code, CommandQueue& messages, // Error code and control queue to Main
                DataQueue& telemetry, // Telemetry queue to Network thread
                pthread_mutex_t& configuration_mutex, // Mutex guarding shared mode/gain configuration
                CircularBuffer& recording_buffer, // Producer buffer for WAV recording
                CircularBuffer& playback_buffer, // Consumer buffer for WAV playback
                atomic<uint32_t>& dropped_telemetry, // Counter for dropped telemetry frames
                atomic<uint32_t>& recording_overruns); // Counter for recording ring-buffer overruns
    ~AudioEngine(); // Stops worker thread and destroys owned mutexes

    // These PCM operations are called by th_AudioDSP, never by Main.
    bool initAlsa(); // Opens and configures capture and playback PCM devices
    int readFrames(int16_t* buffer, int frames); // Reads interleaved PCM frames from the capture device
    int writeFrames(const int16_t* buffer, int frames); // Writes interleaved PCM frames to the playback device
    bool suspendCapture(); // Drops pending capture frames and stops the capture stream
    bool resumeCapture(); // Closes, reopens, and reconfigures the capture PCM device

    bool request(Request request); // Enqueues a single control request into the mailbox if free
    bool ready() const; // Returns true once ALSA initialization succeeds and the worker is ready
    bool start(); // Spawns the audio worker pthread
    void stop(); // Signals shutdown, unblocks capture if needed, and joins the worker thread

private:
    // Named processing steps correspond to the AudioDSP flowchart boxes.
    void apply_output_gain(float* filtered, int16_t* output,
                           snd_pcm_sframes_t count, unsigned gain_code,
                           float& applied_gain);
    void write_output_block(const int16_t* output, snd_pcm_sframes_t count);
    void store_recording_block(const int16_t* output, snd_pcm_sframes_t count);
    void publish_visualization(const int16_t* output, snd_pcm_sframes_t count);
    static int configure_pcm(snd_pcm_t* pcm); // Applies hardware/software PCM parameters and prepares the stream
    static bool recover_pcm(snd_pcm_t* pcm, snd_pcm_sframes_t error); // Recovers PCM stream from xrun/suspend errors
    void set_audio_thread_policy(); // Pins worker to CPU 1 and attempts SCHED_FIFO real-time priority 80
    void run(); // Main real-time audio processing loop executed by the worker thread
    void post_result(WorkerResultCode code, uint32_t operation_id, // Sends a worker status/error message to CommandQueue
                     uint8_t error = 0); // Optional error code (0 indicates success)
    bool apply_request(Request request, // Executes a control request at an audio-block boundary
                       bool& playback_active); // Tracks whether playback mode is active in the loop

    const Options& options_; // PCM device names and runtime options
    atomic<unsigned>& mode_; // Selected DSP filter mode
    atomic<unsigned>& gain_; // Selected output gain code
    atomic<bool>& recording_; // True while live samples should be pushed to recording_buffer_
    atomic<unsigned>& error_code_; // Shared error code reported to status/state telemetry
    CommandQueue& messages_; // Queue delivering worker results/errors to SystemController
    DataQueue& telemetry_; // Queue delivering waveform and spectrum frames to NetworkManager
    pthread_mutex_t& configuration_mutex_; // Mutex synchronizing reads/writes of shared configuration
    CircularBuffer& recording_buffer_; // Ring buffer feeding FileIoManager recording
    CircularBuffer& playback_buffer_; // Ring buffer fed by FileIoManager playback
    atomic<uint32_t>& dropped_telemetry_; // Tracks telemetry packets dropped when queue is full
    atomic<uint32_t>& recording_overruns_; // Tracks recording pushes that failed due to full buffer
    DspProcessor& dsp_; // DSP processor owned by Main, invoked by the audio worker
    atomic<bool> ready_{false}; // Indicates whether ALSA setup succeeded and audio loop is active
    atomic<bool> stop_requested_{false}; // Signals worker and loops to terminate
    pthread_mutex_t request_mutex_{}; // Protects access to the single-slot request_ mailbox
    Request request_{};
    bool has_request_ = false; // Pending control request waiting to be handled by run()
    snd_pcm_t* capture_handle_ = nullptr; // ALSA capture PCM handle owned by the worker thread
    snd_pcm_t* playback_handle_ = nullptr; // ALSA playback PCM handle owned by the worker thread
    atomic<snd_pcm_t*> capture_pcm_{nullptr}; // Atomic pointer allowing stop() to call snd_pcm_drop safely
    pthread_mutex_t capture_mutex_{}; // Synchronizes stop() with resumeCapture()/shutdown on capture PCM
    static void* threadEntry(void* context); // Static pthread start routine forwarding to AudioEngine::run()
    pthread_t worker_{}; // POSIX thread handle for the audio worker
    bool worker_started_ = false; // Tracks whether worker_ has been created and needs joining
};
}  // namespace phono_app // Close phono_app namespace
