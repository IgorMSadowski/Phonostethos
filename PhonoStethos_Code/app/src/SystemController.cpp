/**
 * SystemController.cpp - implements th_Main: validation, worker coordination, authoritative state and LED masks.
 *
 * CONNECTIONS: SystemController.h declares the coordinator and owned objects; SystemController.cpp implements it. main.cpp starts it. It owns both sample buffers, all three managers and the DSP/LED objects, and handles worker results without blocking on worker I/O.
 *
 * FUNCTIONS:
 * - ~SystemController(): release this object's resources after its users have stopped.
 * - SystemController(): connect the owned resources/references used by this class.
 * - compare_file_names(): sort archive entries by basename for a stable display order.
 * - initSystem(): prepare shared resources, create the three worker pthreads and check audio readiness.
 * - run(): execute this class's thread loop in the order shown in its flowchart.
 * - processCommand(): validate a decoded GUI command and dispatch its named system operation.
 * - handleWorkerResult(): match a worker result to the pending operation before committing system state.
 * - setMode(): publish the selected filter mode under the configuration mutex.
 * - setGain(): publish a validated gain code; AudioDSP converts it to a multiplier.
 * - startRecording(): ask FileIO to open a new WAV and keep recording disabled until OPEN_READY.
 * - stopRecording(): close the producer gate and let FileIO drain and finalize the recording.
 * - startPlayback(): resolve a file ID and coordinate capture suspension and WAV prefill.
 * - stopPlayback(): coordinate playback stop, file closure and return to microphone capture.
 * - deleteRecording(): resolve the selected file ID and request deletion in FileIO.
 * - listFiles(): request an archive scan or queue the requested page of an existing scan.
 * - ejectStorage(): synchronize and unmount storage in FileIO, or request that operation from Main.
 * - initUdevMonitor(): subscribe to Linux block-device events and obtain the monitor descriptor.
 * - checkUsbEvents(): poll udev without blocking and retain storage requests for FileIO.
 * - broadcastStatus(): queue capability and State snapshots for Network rather than writing a socket.
 * - generateFilename(): build a mode/date/time/counter WAV name without overwriting an existing file.
 * - stop(): request teardown and join workers before destroying their shared resources.
 * - begin_operation(): allocate a correlation ID and remember the accepted command in Main.
 * - request_file(): attempt delivery to FileIO's one-slot control mailbox.
 * - queue_file_request(): deliver a FileIO request or retain it in Main for a later retry.
 * - request_audio(): attempt delivery to AudioDSP's one-slot control mailbox.
 * - dispatch_deferred_operation_request(): retry Main's retained operation when the FileIO mailbox becomes free.
 * - dispatch_deferred_mount(): retry Main's retained storage-device request when FileIO can accept it.
 * - publish_status(): derive the storage/button capability packet from current authoritative state.
 * - publish_state(): serialize transport, mode, gain, storage, session and diagnostic counters.
 * - publish_command_result(): queue a final or pending acknowledgement for the GUI command.
 * - update_leds(): build and write the complete mode-plus-recording mask from Main's acknowledged state.
 * - handle_archive_result(): update the stable connection-scoped file-ID registry from FileIO's scan.
 * - queue_archive_page(): prepare a bounded page of IDs and names for the GUI archive.
 * - set_error(): publish the current system error code atomically.
 * - set_main_thread_policy(): request CPU 0 affinity and SCHED_OTHER priority 0 with nice 0.
 * - processMessage(): route MainMessage by kind to command handling or worker-result handling.
 * - handle_notification(): handle unsolicited worker/connection events outside an active GUI transition.
 * - handle_operation_error(): fail the matching transition and coordinate any required cleanup.
 * - handle_recording_result(): advance recording open/close only after the matching FileIO result.
 * - handle_playback_result(): advance playback preparation/stop through worker acknowledgements.
 * - complete_operation(): publish the final command result and clear Main's pending context.
 * - start_playback_cleanup(): stop the consumer before FileIO closes/discards playback data.
 * - fail_start_playback(): undo partial playback preparation and report the failure.
 */

#include "phonostethos/SystemController.h" // Include the SystemController class/types used here; implements th_Main: validation, worker coordination, authoritative state and LED masks.

#include "phonostethos/protocol.h" // Include the protocol class/types used here; defines and validates the client/server binary protocol.

#include <libudev.h> // Include Linux device enumeration and hotplug event monitoring.

#include <algorithm> // Include min/max, copying, sorting and other standard algorithms used in this file.
#include <cerrno> // Include errno and named system-call error codes.
#include <cstdio> // Include C-style file/console formatting used by this file.
#include <ctime> // Include calendar time for recording filenames.
#include <iostream> // Include console diagnostics and test results.
#include <poll.h> // Include bounded descriptor-readiness checks for sockets and udev.
#include <pthread.h> // Include POSIX thread creation, mutexes and condition-variable APIs.
#include <sched.h> // Include CPU affinity masks and Linux scheduling policies/priorities.
#include <sys/resource.h> // Include Linux nice-value control, distinct from real-time scheduler priority.
#include <cstring> // Include byte copying/comparison and readable system-error strings.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
// sort() needs a comparison function. This keeps the callback explicit and named.
static bool compare_file_names(const phono::protocol::FileEntry& first, // Pass this next argument/value to the call or initializer above.
                               const phono::protocol::FileEntry& second) { // Declare second for this operation.
    return first.basename < second.basename; // Return first.basename < second.basename to the caller.
} // End this block, declaration or argument list.

// WIRING: pass the SAME queue/buffer objects by reference to all their users.
// writer_ owns its FileIO mailbox; audio_ owns its AudioDSP mailbox;
// network_ owns ControlReplyQueue. See the communication map in the class header.
SystemController::SystemController(const Options& options) // Construct Main's objects and connect the shared queue/buffer references below.
    : options_(options), // Supply the next constructor initializer or argument.
      led_driver_(options.led_device), // Call led_driver_ with the arguments shown here.
      writer_(options, command_queue_, recording_buffer_, playback_buffer_), // Call writer_ with the arguments shown here.
      audio_(options, dsp_processor_, mode_, gain_, recording_, error_code_, // Call audio_ with the arguments shown here.
             command_queue_, data_queue_, configuration_mutex_, // Protect the mode/gain pair while Main publishes and AudioDSP copies it.
             recording_buffer_, playback_buffer_, dropped_telemetry_, // Count visualization updates skipped when the channel is busy or disconnected.
             recording_overruns_), // Pass this next argument/value to the call or initializer above.
      network_(options, command_queue_, data_queue_) { // Call network_ with the arguments shown here.
    initializeMutex(configuration_mutex_); // Initialize one pthread mutex and reject initialization failure.
} // End this function or local scope.

SystemController::~SystemController() { // Stop/join all workers before releasing the configuration mutex.
    stop(); // Request teardown and join workers before destroying their shared resources.
    pthread_mutex_destroy(&configuration_mutex_); // Release this initialized mutex after its users have stopped.
} // End this function or local scope.

// Prepare shared resources first; only then create the three worker pthreads.
// FLOWCHART: Boot -> prepare devices/buffers/FFTW -> start worker threads.
bool SystemController::initSystem() { // Prepare shared resources, create the three worker pthreads and check audio readiness.
    if (initialized_) { // Test the condition initialized_.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    set_main_thread_policy(); // Request CPU 0 affinity and SCHED_OTHER priority 0 with nice 0.
    if (!initUdevMonitor()) { // Test the condition !initUdevMonitor().
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    if (!led_driver_.openDriver()) { // Test the condition !led_driver_.openDriver().
        led_error_.store(true); // Update the shared led error value atomically.
        cerr << "LED status outputs are unavailable; continuing without LEDs\n"; // Print the diagnostic or test result for the operator.
    } // End this block, declaration or argument list.
    update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
    // FFTW planning belongs to startup: no worker may execute an unfinished plan.
    if (!dsp_processor_.initialize()) { // Test the condition !dsp_processor_.initialize().
        cerr << "Cannot prepare the FFTW plan\n"; // Print the diagnostic or test result for the operator.
        set_error(static_cast<uint8_t>(phono::protocol::Error::DspError)); // Publish the current system error code atomically.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    if (!audio_.start() || !writer_.start()) { // Test the condition !audio_.start() || !writer_.start().
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5); // Initialize deadline to chrono::steady_clock::now() + chrono::seconds(5).
    while (!audio_init_complete_.load() && // Repeat this block while its condition remains true; recheck shared state on each pass.
           chrono::steady_clock::now() < deadline) { // Finish the parameters/condition and enter the block below.
        MainMessage message; // Declare message for this operation.
        if (command_queue_.dequeue(message, chrono::milliseconds(10))) { // Test the condition command_queue_.dequeue(message, chrono::milliseconds(10)).
            processMessage(message); // Route MainMessage by kind to command handling or worker-result handling.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    if (!audio_ready_.load()) { // Test the condition !audio_ready_.load().
        cerr << "Audio initialization did not complete before timeout\n"; // Print the diagnostic or test result for the operator.
        set_error(static_cast<uint8_t>(phono::protocol::Error::MicLost)); // Publish the current system error code atomically.
    } // End this block, declaration or argument list.
    network_.start(); // Call network_.start with the arguments shown here.
    initialized_ = true; // Set initialized to true.
    next_storage_check_ = chrono::steady_clock::now(); // Set next storage check to chrono::steady_clock::now().
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

// Main coordinates operations. The queue lock is released before any handler runs.
// FLOWCHART: th_Main -> dequeue MainMessage -> dispatch -> check USB events.
void SystemController::run() { // Execute this class's thread loop in the order shown in its flowchart.
    if (!initialized_ && !initSystem()) { // Test the condition !initialized_ && !initSystem().
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    while (running.load() && !stopping_) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        // QUEUE RECEIVE: Main takes one command/result from command_queue_.
        // dequeue() waits on the queue condition if empty and unlocks on return.
        MainMessage message; // Declare message for this operation.
        if (command_queue_.dequeue(message, chrono::milliseconds(10))) { // Test the condition command_queue_.dequeue(message, chrono::milliseconds(10)).
            processMessage(message); // Route MainMessage by kind to command handling or worker-result handling.
        } // End this block, declaration or argument list.
        checkUsbEvents(); // Poll udev without blocking and retain storage requests for FileIO.
        dispatch_deferred_operation_request(); // Retry Main's retained operation when the FileIO mailbox becomes free.
        dispatch_deferred_mount(); // Retry Main's retained storage-device request when FileIO can accept it.
        const auto now = chrono::steady_clock::now(); // Initialize now to chrono::steady_clock::now().
        if (now >= next_storage_check_ && !has_deferred_storage_ && // Test the condition now >= next_storage_check_ && !has_deferred_storage_ &&.
            !has_pending_operation_) { // Finish the parameters/condition and enter the block below.
            FileIoManager::Request request; // Declare request for this operation.
            request.kind = FileIoManager::RequestKind::REFRESH_STORAGE; // Set request.kind to FileIoManager::RequestKind::REFRESH_STORAGE.
            writer_.request(request); // Attempt to copy this control request into the worker mailbox; execution happens in that worker.
            next_storage_check_ = now + chrono::milliseconds(250); // Set next storage check to now + chrono::milliseconds(250).
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    stop(); // Request teardown and join workers before destroying their shared resources.
} // End this function or local scope.

// Reject invalid/conflicting requests before calling the named diagram operation.
// FLOWCHART: thMain_PROCESS -> cmd.cmd_type decision and its command boxes.
void SystemController::processCommand( // Begin this function/widget call; its arguments follow below.
    const phono::protocol::CommandPacket& request) { // Declare request for this operation.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = request.cmd_type; // Initialize command to request.cmd_type.
    if (!valid_command_value(request)) { // Test the condition !valid_command_value(request).
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::InvalidArgument)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (has_pending_operation_) { // Test the condition has_pending_operation_.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.

    switch (command) { // Choose one branch from the supplied command/state value.
        case CommandType::SET_MODE: // Handle the CommandType::SET_MODE choice in this switch.
            setMode(static_cast<FilterMode>(request.value)); // Publish the selected filter mode under the configuration mutex.
            return; // Leave this function; any local scoped locks are released.
        case CommandType::SET_GAIN: // Handle the CommandType::SET_GAIN choice in this switch.
            setGain(static_cast<int>(request.value)); // Publish a validated gain code; AudioDSP converts it to a multiplier.
            return; // Leave this function; any local scoped locks are released.
        case CommandType::START_REC: // Handle the CommandType::START_REC choice in this switch.
            startRecording(); // Ask FileIO to open a new WAV and keep recording disabled until OPEN_READY.
            return; // Leave this function; any local scoped locks are released.
        case CommandType::STOP_REC: // Handle the CommandType::STOP_REC choice in this switch.
            stopRecording(); // Close the producer gate and let FileIO drain and finalize the recording.
            return; // Leave this function; any local scoped locks are released.
        case CommandType::PLAY_AUDIO: // Handle the CommandType::PLAY_AUDIO choice in this switch.
            startPlayback(request.value); // Resolve a file ID and coordinate capture suspension and WAV prefill.
            return; // Leave this function; any local scoped locks are released.
        case CommandType::STOP_PLAYBACK: // Handle the CommandType::STOP_PLAYBACK choice in this switch.
            stopPlayback(); // Coordinate playback stop, file closure and return to microphone capture.
            return; // Leave this function; any local scoped locks are released.
        case CommandType::EJECT_USB: // Handle the CommandType::EJECT_USB choice in this switch.
            ejectStorage(); // Synchronize and unmount storage in FileIO, or request that operation from Main.
            return; // Leave this function; any local scoped locks are released.
        case CommandType::GET_ARCHIVE: // Handle the CommandType::GET_ARCHIVE choice in this switch.
            listFiles(request.value); // Request an archive scan or queue the requested page of an existing scan.
            return; // Leave this function; any local scoped locks are released.
        case CommandType::DELETE_RECORDING: // Handle the CommandType::DELETE_RECORDING choice in this switch.
            deleteRecording(request.value); // Resolve the selected file ID and request deletion in FileIO.
            return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// Each result advances one pending transition. operation_id rejects replies from older work.

// FLOWCHART: thMain_PROCESS -> WORKER_RESULT / WORKER_ERROR.
// First handle general notifications, then match the current operation ID.
void SystemController::handleWorkerResult(const MainMessage& message) { // Match a worker result to the pending operation before committing system state.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    if (handle_notification(message)) { // Test the condition handle_notification(message).
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const bool playback_completion_matches_pending = // Start initializing this local value from the expression on the next line.
        message.result_code == WorkerResultCode::PLAYBACK_FINISHED && // Continue checking the file/state comparison; no value is assigned here.
        has_pending_operation_ && // Continue this calculation/condition with the next line.
        (pending_.kind == OperationKind::StopPlayback || // Continue this calculation/condition with the next line.
         (pending_.kind == OperationKind::EjectStorage && // Continue this calculation/condition with the next line.
          pending_.argument == // Continue checking the file/state comparison; no value is assigned here.
              static_cast<uint32_t>(Transport::Playback))); // Complete the call above with these remaining arguments.
    if (!has_pending_operation_ || // Test the condition !has_pending_operation_ ||.
        (pending_.operation_id != message.operation_id && // Continue this calculation/condition with the next line.
         !playback_completion_matches_pending)) { // Finish the parameters/condition and enter the block below.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (message.error_code != 0) { // Test the condition message.error_code != 0.
        handle_operation_error(message); // Fail the matching transition and coordinate any required cleanup.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.

    switch (message.result_code) { // Choose one branch from the supplied command/state value.
        case WorkerResultCode::OPEN_READY: // Handle the WorkerResultCode::OPEN_READY choice in this switch.
        case WorkerResultCode::RECORD_CLOSED: // Handle the WorkerResultCode::RECORD_CLOSED choice in this switch.
            handle_recording_result(message); // Advance recording open/close only after the matching FileIO result.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::PLAYBACK_READY: // Handle the WorkerResultCode::PLAYBACK_READY choice in this switch.
        case WorkerResultCode::CAPTURE_SUSPENDED: // Handle the WorkerResultCode::CAPTURE_SUSPENDED choice in this switch.
        case WorkerResultCode::PLAYBACK_STARTED: // Handle the WorkerResultCode::PLAYBACK_STARTED choice in this switch.
        case WorkerResultCode::PLAYBACK_CLOSED: // Handle the WorkerResultCode::PLAYBACK_CLOSED choice in this switch.
        case WorkerResultCode::PLAYBACK_STOPPED: // Handle the WorkerResultCode::PLAYBACK_STOPPED choice in this switch.
        case WorkerResultCode::PLAYBACK_FINISHED: // Handle the WorkerResultCode::PLAYBACK_FINISHED choice in this switch.
        case WorkerResultCode::CAPTURE_RESUMED: // Handle the WorkerResultCode::CAPTURE_RESUMED choice in this switch.
            handle_playback_result(message); // Advance playback preparation/stop through worker acknowledgements.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::ARCHIVE_READY: // Handle the WorkerResultCode::ARCHIVE_READY choice in this switch.
            if (pending_.kind == OperationKind::ListArchive) { // Test the condition pending_.kind == OperationKind::ListArchive.
                handle_archive_result(message); // Update the stable connection-scoped file-ID registry from FileIO's scan.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::RECORDING_DELETED: // Handle the WorkerResultCode::RECORDING_DELETED choice in this switch.
            if (pending_.kind != OperationKind::DeleteRecording) { // Test the condition pending_.kind != OperationKind::DeleteRecording.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            complete_operation(message.error_code == 0, message.error_code); // Publish the final command result and clear Main's pending context.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::STORAGE_EJECTED: // Handle the WorkerResultCode::STORAGE_EJECTED choice in this switch.
            if (pending_.kind != OperationKind::EjectStorage) { // Test the condition pending_.kind != OperationKind::EjectStorage.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (message.error_code == 0) storage_flags_ = 0; // Test the condition message.error_code == 0. If true, set storage flags to 0.
            transport_.store(Transport::Live); // Update the shared transport value atomically.
            complete_operation(message.error_code == 0, message.error_code); // Publish the final command result and clear Main's pending context.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::AUDIO_READY: // Handle the WorkerResultCode::AUDIO_READY choice in this switch.
        case WorkerResultCode::FILEIO_READY: // Handle the WorkerResultCode::FILEIO_READY choice in this switch.
        case WorkerResultCode::STORAGE_CHANGED: // Handle the WorkerResultCode::STORAGE_CHANGED choice in this switch.
        case WorkerResultCode::NETWORK_STARTED: // Handle the WorkerResultCode::NETWORK_STARTED choice in this switch.
        case WorkerResultCode::NETWORK_FAILED: // Handle the WorkerResultCode::NETWORK_FAILED choice in this switch.
        case WorkerResultCode::CLIENT_CONNECTED: // Handle the WorkerResultCode::CLIENT_CONNECTED choice in this switch.
        case WorkerResultCode::CLIENT_DISCONNECTED: // Handle the WorkerResultCode::CLIENT_DISCONNECTED choice in this switch.
            return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
} // End this function or local scope.

void SystemController::setMode(FilterMode mode) { // Publish the selected filter mode under the configuration mutex.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::SET_MODE; // Initialize command to CommandType::SET_MODE.
    { // Begin this local scope; scoped locks release when it ends.
        PthreadLock lock(configuration_mutex_); // Lock this shared object until the local scope ends, including an early return.
        mode_.store(static_cast<unsigned>(mode)); // Update the shared mode value atomically.
    } // End this block, declaration or argument list.
    update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
    broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
    publish_command_result(command, Result::Completed, 0); // Queue a final or pending acknowledgement for the GUI command.
} // End this function or local scope.

void SystemController::setGain(int gain) { // Publish a validated gain code; AudioDSP converts it to a multiplier.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::SET_GAIN; // Initialize command to CommandType::SET_GAIN.
    { // Begin this local scope; scoped locks release when it ends.
        PthreadLock lock(configuration_mutex_); // Lock this shared object until the local scope ends, including an early return.
        gain_.store(static_cast<unsigned>(gain)); // Update the shared gain value atomically.
    } // End this block, declaration or argument list.
    broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
    publish_command_result(command, Result::Completed, 0); // Queue a final or pending acknowledgement for the GUI command.
} // End this function or local scope.

// Ask FileIO to open the WAV. Recording starts only after OPEN_READY arrives.
void SystemController::startRecording() { // Ask FileIO to open a new WAV and keep recording disabled until OPEN_READY.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::START_REC; // Initialize command to CommandType::START_REC.
    const auto required = // Start initializing this local value from the expression on the next line.
        StorageMounted | StorageWritable | StorageSpaceOk; // Complete this expression or declaration.
    if (!audio_ready_.load()) { // Test the condition !audio_ready_.load().
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::MicLost)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (transport_.load() != Transport::Live) { // Test the condition transport_.load() != Transport::Live.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if ((storage_flags_ & required) != required) { // Test the condition (storage_flags_ & required) != required.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::StorageLost)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto mode = // Start initializing this local value from the expression on the next line.
        static_cast<FilterMode>(mode_.load()); // Complete the call above with these remaining arguments.
    const auto path = options_.storage / generateFilename(mode); // Initialize path to options_.storage / generateFilename(mode).
    const auto session_id = next_session_id_ - 1; // Initialize session id to next_session_id_ - 1.
    const auto operation_id = // Start initializing this local value from the expression on the next line.
        begin_operation(OperationKind::StartRecording, command, // Call begin_operation with the arguments shown here.
                        session_id); // Complete the call above with these remaining arguments.
    FileIoManager::Request file_request; // Declare file request for this operation.
    file_request.kind = FileIoManager::RequestKind::OPEN_RECORD; // Set file request.kind to FileIoManager::RequestKind::OPEN_RECORD.
    file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
    file_request.path = path; // Set file request.path to path.
    // MAILBOX SEND: the struct is ready; request_file -> writer_.request copies
    // it into FileIO's mailbox. true means accepted, not that the WAV is open.
    // FileIO later reports OPEN_READY through CommandQueue with this ID.
    if (!request_file(file_request)) { // Test the condition !request_file(file_request).
        has_pending_operation_ = false; // Mark pending operation as empty.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    publish_command_result(command, Result::AcceptedPending, 0); // Queue a final or pending acknowledgement for the GUI command.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

// Close the producer gate now. FileIO drains accepted samples and closes the WAV later.
void SystemController::stopRecording() { // Close the producer gate and let FileIO drain and finalize the recording.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::STOP_REC; // Initialize command to CommandType::STOP_REC.
    if (transport_.load() != Transport::Recording) { // Test the condition transport_.load() != Transport::Recording.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto operation_id = // Start initializing this local value from the expression on the next line.
        begin_operation(OperationKind::StopRecording, command, 0); // Allocate a correlation ID and remember the accepted command in Main.
    transport_.store(Transport::Stopping); // Update the shared transport value atomically.
    recording_.store(false); // Update the shared recording value atomically.
    recording_buffer_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    FileIoManager::Request file_request; // Declare file request for this operation.
    file_request.kind = FileIoManager::RequestKind::CLOSE_RECORD; // Set file request.kind to FileIoManager::RequestKind::CLOSE_RECORD.
    file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
    if (!queue_file_request(file_request)) { // Test the condition !queue_file_request(file_request).
        set_error(static_cast<uint8_t>(Error::Busy)); // Publish the current system error code atomically.
        complete_operation(false, static_cast<uint8_t>(Error::Busy)); // Publish the final command result and clear Main's pending context.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
    publish_command_result(command, Result::AcceptedPending, 0); // Queue a final or pending acknowledgement for the GUI command.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

// Prepare the file and suspend microphone capture; both workers must be ready.
void SystemController::startPlayback(uint32_t file_id) { // Resolve a file ID and coordinate capture suspension and WAV prefill.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::PLAY_AUDIO; // Initialize command to CommandType::PLAY_AUDIO.
    const auto found = archive_by_id_.find(file_id); // Initialize found to archive_by_id_.find(file_id).
    if (found == archive_by_id_.end()) { // Test the condition found == archive_by_id_.end().
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::StaleFileId)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto required = // Start initializing this local value from the expression on the next line.
        StorageMounted | StorageReadable | StorageSpaceOk; // Complete this expression or declaration.
    if (!audio_ready_.load() || transport_.load() != Transport::Live || // Test the condition !audio_ready_.load() || transport_.load() != Transport::Live ||.
        (storage_flags_ & required) != required || error_code_.load() != 0) { // Read the shared atomic value into this local snapshot.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::StorageLost)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto operation_id = // Start initializing this local value from the expression on the next line.
        begin_operation(OperationKind::StartPlayback, command, // Call begin_operation with the arguments shown here.
                        file_id); // Complete the call above with these remaining arguments.
    transport_.store(Transport::Stopping); // Update the shared transport value atomically.
    FileIoManager::Request file_request; // Declare file request for this operation.
    file_request.kind = FileIoManager::RequestKind::PREPARE_PLAYBACK; // Set file request.kind to FileIoManager::RequestKind::PREPARE_PLAYBACK.
    file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
    file_request.identity = found->second; // Set file request.identity to found->second.
    if (!request_file(file_request)) { // Test the condition !request_file(file_request).
        transport_.store(Transport::Live); // Update the shared transport value atomically.
        has_pending_operation_ = false; // Mark pending operation as empty.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (!request_audio({AudioEngine::RequestKind::SUSPEND_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::SUSPEND_CAPTURE,.
                        operation_id})) { // Finish the parameters/condition and enter the block below.
        fail_start_playback(static_cast<uint8_t>(Error::Busy)); // Undo partial playback preparation and report the failure.
        publish_command_result(command, Result::AcceptedPending, 0); // Queue a final or pending acknowledgement for the GUI command.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
    publish_command_result(command, Result::AcceptedPending, 0); // Queue a final or pending acknowledgement for the GUI command.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

// Request both worker transitions; LIVE is published only after their acknowledgements.
void SystemController::stopPlayback() { // Coordinate playback stop, file closure and return to microphone capture.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::STOP_PLAYBACK; // Initialize command to CommandType::STOP_PLAYBACK.
    if (transport_.load() != Transport::Playback) { // Test the condition transport_.load() != Transport::Playback.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto operation_id = // Start initializing this local value from the expression on the next line.
        begin_operation(OperationKind::StopPlayback, command, 0); // Allocate a correlation ID and remember the accepted command in Main.
    transport_.store(Transport::Stopping); // Update the shared transport value atomically.
    FileIoManager::Request file_request; // Declare file request for this operation.
    file_request.kind = FileIoManager::RequestKind::STOP_PLAYBACK; // Set file request.kind to FileIoManager::RequestKind::STOP_PLAYBACK.
    file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
    if (!queue_file_request(file_request)) { // Test the condition !queue_file_request(file_request).
        transport_.store(Transport::Playback); // Update the shared transport value atomically.
        has_pending_operation_ = false; // Mark pending operation as empty.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (!request_audio({AudioEngine::RequestKind::STOP_PLAYBACK, // Test the condition !request_audio({AudioEngine::RequestKind::STOP_PLAYBACK,.
                        operation_id})) { // Finish the parameters/condition and enter the block below.
        pending_.error_code = static_cast<uint8_t>(Error::Busy); // Set pending .error code to static_cast<uint8_t>(Error::Busy).
    } // End this block, declaration or argument list.
    broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
    publish_command_result(command, Result::AcceptedPending, 0); // Queue a final or pending acknowledgement for the GUI command.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

void SystemController::deleteRecording(uint32_t file_id) { // Resolve the selected file ID and request deletion in FileIO.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::DELETE_RECORDING; // Initialize command to CommandType::DELETE_RECORDING.
    const auto found = archive_by_id_.find(file_id); // Initialize found to archive_by_id_.find(file_id).
    if (found == archive_by_id_.end()) { // Test the condition found == archive_by_id_.end().
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::StaleFileId)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (transport_.load() != Transport::Live || // Test the condition transport_.load() != Transport::Live ||.
        (storage_flags_ & (StorageMounted | StorageWritable)) != // Continue the expression or parameter list from the preceding line.
            (StorageMounted | StorageWritable)) { // Finish the parameters/condition and enter the block below.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::StorageLost)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto operation_id = // Start initializing this local value from the expression on the next line.
        begin_operation(OperationKind::DeleteRecording, command, // Call begin_operation with the arguments shown here.
                        file_id); // Complete the call above with these remaining arguments.
    FileIoManager::Request file_request; // Declare file request for this operation.
    file_request.kind = FileIoManager::RequestKind::DELETE; // Set file request.kind to FileIoManager::RequestKind::DELETE.
    file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
    file_request.identity = found->second; // Set file request.identity to found->second.
    if (!request_file(file_request)) { // Test the condition !request_file(file_request).
        has_pending_operation_ = false; // Mark pending operation as empty.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    publish_command_result(command, Result::AcceptedPending, 0); // Queue a final or pending acknowledgement for the GUI command.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

void SystemController::listFiles(uint32_t cursor) { // Request an archive scan or queue the requested page of an existing scan.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::GET_ARCHIVE; // Initialize command to CommandType::GET_ARCHIVE.
    const auto operation_id = // Start initializing this local value from the expression on the next line.
        begin_operation(OperationKind::ListArchive, command, // Call begin_operation with the arguments shown here.
                        cursor); // Complete the call above with these remaining arguments.
    FileIoManager::Request file_request; // Declare file request for this operation.
    file_request.kind = FileIoManager::RequestKind::LIST; // Set file request.kind to FileIoManager::RequestKind::LIST.
    file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
    if (!request_file(file_request)) { // Test the condition !request_file(file_request).
        has_pending_operation_ = false; // Mark pending operation as empty.
        publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                               static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    publish_command_result(command, Result::AcceptedPending, 0); // Queue a final or pending acknowledgement for the GUI command.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

// Finish active storage work before FileIO checks syncfs() and unmount.
void SystemController::ejectStorage() { // Synchronize and unmount storage in FileIO, or request that operation from Main.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const auto command = CommandType::EJECT_USB; // Initialize command to CommandType::EJECT_USB.
    const auto previous_transport = transport_.load(); // Read the shared atomic value into this local snapshot.
    const auto operation_id = // Start initializing this local value from the expression on the next line.
        begin_operation(OperationKind::EjectStorage, command, 0); // Allocate a correlation ID and remember the accepted command in Main.
    pending_.argument = static_cast<uint32_t>(previous_transport); // Set pending .argument to static_cast<uint32_t>(previous_transport).
    transport_.store(Transport::Ejecting); // Update the shared transport value atomically.
    if (previous_transport == Transport::Recording) { // Test the condition previous_transport == Transport::Recording.
        recording_.store(false); // Update the shared recording value atomically.
        recording_buffer_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
        FileIoManager::Request file_request; // Declare file request for this operation.
        file_request.kind = FileIoManager::RequestKind::CLOSE_RECORD; // Set file request.kind to FileIoManager::RequestKind::CLOSE_RECORD.
        file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
        if (!queue_file_request(file_request)) { // Test the condition !queue_file_request(file_request).
            transport_.store(Transport::Recording); // Update the shared transport value atomically.
            has_pending_operation_ = false; // Mark pending operation as empty.
            publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                                   static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
    } else if (previous_transport == Transport::Playback) { // Follow the alternative path when the previous condition is false.
        pending_.file_done = false; // Set pending .file done to false.
        pending_.audio_done = false; // Set pending .audio done to false.
        FileIoManager::Request file_request; // Declare file request for this operation.
        file_request.kind = FileIoManager::RequestKind::STOP_PLAYBACK; // Set file request.kind to FileIoManager::RequestKind::STOP_PLAYBACK.
        file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
        if (!queue_file_request(file_request)) { // Test the condition !queue_file_request(file_request).
            transport_.store(Transport::Playback); // Update the shared transport value atomically.
            has_pending_operation_ = false; // Mark pending operation as empty.
            publish_command_result(command, Result::Rejected, // Call publish_command_result with the arguments shown here.
                                   static_cast<uint32_t>(Error::Busy)); // Complete the call above with these remaining arguments.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        const bool audio_requested = request_audio( // Initialize audio requested to request_audio(.
            {AudioEngine::RequestKind::STOP_PLAYBACK, operation_id}); // Complete the call above with these remaining arguments.
        if (!audio_requested) { // Test the condition !audio_requested.
            pending_.error_code = static_cast<uint8_t>(Error::Busy); // Set pending .error code to static_cast<uint8_t>(Error::Busy).
        } // End this block, declaration or argument list.
        pending_.commit_requested = false; // Set pending .commit requested to false.
    } else { // Follow the alternative path when the previous condition is false.
        pending_.stage = WorkflowStage::EjectingStorage; // Set pending .stage to WorkflowStage::EjectingStorage.
        FileIoManager::Request file_request; // Declare file request for this operation.
        file_request.kind = FileIoManager::RequestKind::EJECT; // Set file request.kind to FileIoManager::RequestKind::EJECT.
        file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
        if (!queue_file_request(file_request)) { // Test the condition !queue_file_request(file_request).
            transport_.store(Transport::Live); // Update the shared transport value atomically.
            complete_operation(false, // Call complete_operation with the arguments shown here.
                               static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
    publish_command_result(command, Result::AcceptedPending, 0); // Queue a final or pending acknowledgement for the GUI command.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

bool SystemController::initUdevMonitor() { // Subscribe to Linux block-device events and obtain the monitor descriptor.
    /* API: udev_new
     * Create the userspace udev context. NULL means creation failed. Release the context with udev_unref after the monitor is no longer used.
     */
    udev_context_ = udev_new(); // Set udev context to udev_new().
    if (udev_context_ == nullptr) { // Test the condition udev_context_ == nullptr.
        cerr << "Cannot initialize udev context\n"; // Print the diagnostic or test result for the operator.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    /* API: udev_monitor_new_from_netlink
     * Subscribe to udev device notifications using the existing context. Add a subsystem filter and enable receiving before polling its descriptor.
     */
    udev_monitor_ = udev_monitor_new_from_netlink(udev_context_, "udev"); // Set udev monitor to udev_monitor_new_from_netlink(udev_context_, "udev").
    if (udev_monitor_ == nullptr || // Test the condition udev_monitor_ == nullptr ||.
        /* API: udev_monitor_filter_add_match_subsystem_devtype
         * Filter monitor events to the required subsystem/device type before receiving. A negative return indicates failure.
         */
        udev_monitor_filter_add_match_subsystem_devtype( // Call udev_monitor_filter_add_match_subsystem_devtype with the arguments shown here.
            udev_monitor_, "block", nullptr) < 0 || // Continue this calculation/condition with the next line.
        /* API: udev_monitor_enable_receiving
         * Enable delivery to the configured monitor. Check the return before asking for its descriptor.
         */
        udev_monitor_enable_receiving(udev_monitor_) < 0) { // Call udev_monitor_enable_receiving with the arguments shown here.
        cerr << "Cannot initialize udev block-device monitor\n"; // Print the diagnostic or test result for the operator.
        if (udev_monitor_ != nullptr) { // Test the condition udev_monitor_ != nullptr.
            /* API: udev_monitor_unref
             * Release the monitor during teardown, after Main has stopped checking its descriptor.
             */
            udev_monitor_unref(udev_monitor_); // Call udev_monitor_unref with the arguments shown here.
            udev_monitor_ = nullptr; // Set udev monitor to nullptr.
        } // End this block, declaration or argument list.
        /* API: udev_unref
         * Release the libudev context after its monitor is released.
         */
        udev_unref(udev_context_); // Call udev_unref with the arguments shown here.
        udev_context_ = nullptr; // Set udev context to nullptr.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    /* API: udev_monitor_get_fd
     * Get the monitor's file descriptor for poll(). Poll with zero timeout in Main so USB checking does not block command dispatch.
     */
    udev_fd_ = udev_monitor_get_fd(udev_monitor_); // Set udev fd to udev_monitor_get_fd(udev_monitor_).
    if (udev_fd_ < 0) { // Test the condition udev_fd_ < 0.
        cerr << "Cannot get udev monitor descriptor\n"; // Print the diagnostic or test result for the operator.
        udev_monitor_unref(udev_monitor_); // Call udev_monitor_unref with the arguments shown here.
        udev_monitor_ = nullptr; // Set udev monitor to nullptr.
        udev_unref(udev_context_); // Call udev_unref with the arguments shown here.
        udev_context_ = nullptr; // Set udev context to nullptr.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

// Poll udev without blocking Main; mount/unmount/file work stays in FileIO.
void SystemController::checkUsbEvents() { // Poll udev without blocking and retain storage requests for FileIO.
    if (udev_fd_ < 0) { // Test the condition udev_fd_ < 0.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    pollfd event{udev_fd_, POLLIN, 0}; // Declare event for this operation.
    /* API: ::poll
     * Wait for descriptor readiness for a bounded number of milliseconds. Positive means events, zero means timeout, -1 means error.
     */
    const int ready = ::poll(&event, 1, 0); // Initialize ready to ::poll(&event, 1, 0).
    if (ready < 0) { // Test the condition ready < 0.
        if (errno != EINTR) { // Test the condition errno != EINTR.
            cerr << "udev monitor poll: " << strerror(errno) << '\n'; // Print the diagnostic or test result for the operator.
        } // End this block, declaration or argument list.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (ready == 0 || !(event.revents & POLLIN)) { // Test the condition ready == 0 || !(event.revents & POLLIN).
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.

    while (true) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        errno = 0; // Set errno to 0.
        /* API: udev_monitor_receive_device
         * Read one available device event from the udev monitor. Inspect its action, devnode and properties, then release it with udev_device_unref.
         */
        udev_device* device = udev_monitor_receive_device(udev_monitor_); // Initialize device to udev_monitor_receive_device(udev_monitor_).
        if (device == nullptr) { // Test the condition device == nullptr.
            if (errno != 0 && errno != EAGAIN && errno != EWOULDBLOCK && // Test the condition errno != 0 && errno != EAGAIN && errno != EWOULDBLOCK &&.
                errno != EINTR) { // Finish the parameters/condition and enter the block below.
                cerr << "Cannot read udev event: " << strerror(errno) << '\n'; // Print the diagnostic or test result for the operator.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        const auto* usb_parent = // Start initializing this local value from the expression on the next line.
            /* API: udev_device_get_parent_with_subsystem_devtype
             * Find a matching parent device, used here to distinguish USB storage. The returned parent is borrowed; the current device owns the reference relationship.
             */
            udev_device_get_parent_with_subsystem_devtype( // Call udev_device_get_parent_with_subsystem_devtype with the arguments shown here.
                device, "usb", "usb_device"); // Complete the call above with these remaining arguments.
        /* API: udev_device_get_action
         * Read the event action string such as add/remove/change. The borrowed string may be nullptr and remains tied to this udev device.
         */
        const char* action = udev_device_get_action(device); // Initialize action to udev_device_get_action(device).
        /* API: udev_device_get_devnode
         * Read the block-device path, for example /dev/sda1. Check nullptr before building the FileIO request.
         */
        const char* device_node = udev_device_get_devnode(device); // Initialize device node to udev_device_get_devnode(device).
        const char* filesystem_type = // Start initializing this local value from the expression on the next line.
            /* API: udev_device_get_property_value
             * Read a named property such as ID_FS_TYPE. The returned string is borrowed; copy it into the mailbox request before unref.
             */
            udev_device_get_property_value(device, "ID_FS_TYPE"); // Call udev_device_get_property_value with the arguments shown here.
        if (usb_parent != nullptr && action != nullptr) { // Test the condition usb_parent != nullptr && action != nullptr.
            FileIoManager::Request request; // Declare request for this operation.
            request.path = device_node != nullptr ? device_node : ""; // Set request.path to device_node != nullptr ? device_node : "".
            if (strcmp(action, "remove") == 0 && device_node != nullptr) { // Test the condition strcmp(action, "remove") == 0 && device_node != nullptr.
                request.kind = FileIoManager::RequestKind::DEVICE_REMOVED; // Set request.kind to FileIoManager::RequestKind::DEVICE_REMOVED.
                deferred_storage_request_ = request; // Set deferred storage request to request.
                has_deferred_storage_ = true; // Mark deferred storage as available.
            } else if ((strcmp(action, "add") == 0 || // Follow the alternative path when the previous condition is false.
                        strcmp(action, "change") == 0) && // Call strcmp with the arguments shown here.
                       device_node != nullptr && filesystem_type != nullptr) { // Finish the parameters/condition and enter the block below.
                request.kind = FileIoManager::RequestKind::MOUNT; // Set request.kind to FileIoManager::RequestKind::MOUNT.
                request.filesystem_type = filesystem_type; // Set request.filesystem type to filesystem_type.
                deferred_storage_request_ = request; // Set deferred storage request to request.
                has_deferred_storage_ = true; // Mark deferred storage as available.
            } else { // Follow the alternative path when the previous condition is false.
                request.kind = FileIoManager::RequestKind::REFRESH_STORAGE; // Set request.kind to FileIoManager::RequestKind::REFRESH_STORAGE.
                deferred_storage_request_ = request; // Set deferred storage request to request.
                has_deferred_storage_ = true; // Mark deferred storage as available.
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
        /* API: udev_device_unref
         * Release the event-device reference after copying the required fields. Do not keep borrowed strings after this point.
         */
        udev_device_unref(device); // Call udev_device_unref with the arguments shown here.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// These methods enqueue replies. Only th_Network writes to the TCP socket.
void SystemController::broadcastStatus() { // Queue capability and State snapshots for Network rather than writing a socket.
    publish_status(); // Derive the storage/button capability packet from current authoritative state.
    publish_state(); // Serialize transport, mode, gain, storage, session and diagnostic counters.
} // End this function or local scope.

string SystemController::generateFilename(FilterMode mode) { // Build a mode/date/time/counter WAV name without overwriting an existing file.
    const auto now = // Start initializing this local value from the expression on the next line.
        chrono::system_clock::to_time_t(chrono::system_clock::now()); // Complete the call above with these remaining arguments.
    tm local{}; // Declare local for this operation.
    /* API: localtime_r
     * Convert a calendar timestamp into caller-owned local-time fields for the filename. This form does not share a static tm buffer.
     */
    localtime_r(&now, &local); // Call localtime_r with the arguments shown here.
    char timestamp[32]{}; // Declare timestamp for this operation.
    /* API: strftime
     * Format the date/time into the supplied bounded character buffer using the given format string.
     */
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local); // Call strftime with the arguments shown here.
    const char* modes[] = {"GENERAL", "HEART", "LUNG"}; // Initialize modes to {"GENERAL", "HEART", "LUNG"}.
    char session[16]{}; // Declare session for this operation.
    /* API: snprintf
     * Format text into the supplied bounded buffer, including the filename counter. The size limit prevents writing beyond the array.
     */
    snprintf(session, sizeof(session), "%06u", next_session_id_++); // Format the session counter into this bounded character array.
    return string(modes[min(static_cast<unsigned>(mode), 2U)]) + "_" + // Return string(modes[min(static_cast<unsigned>(mode), 2U)]) + "_" + to the caller.
           timestamp + "_" + session + ".wav"; // Complete this expression or declaration.
} // End this function or local scope.

// Developer/service stop: wake workers and join them before shared objects are destroyed.
void SystemController::stop() { // Request teardown and join workers before destroying their shared resources.
    if (stopping_) { // Test the condition stopping_.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    stopping_ = true; // Set stopping to true.
    network_.stop(); // Call network_.stop with the arguments shown here.
    recording_.store(false); // Update the shared recording value atomically.
    recording_buffer_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    playback_buffer_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    audio_.stop(); // Call audio_.stop with the arguments shown here.
    writer_.stop(); // Call writer_.stop with the arguments shown here.
    if (udev_monitor_ != nullptr) { // Test the condition udev_monitor_ != nullptr.
        udev_monitor_unref(udev_monitor_); // Call udev_monitor_unref with the arguments shown here.
        udev_monitor_ = nullptr; // Set udev monitor to nullptr.
    } // End this block, declaration or argument list.
    if (udev_context_ != nullptr) { // Test the condition udev_context_ != nullptr.
        udev_unref(udev_context_); // Call udev_unref with the arguments shown here.
        udev_context_ = nullptr; // Set udev context to nullptr.
    } // End this block, declaration or argument list.
    udev_fd_ = -1; // Set udev fd to -1.
    led_driver_.closeDriver(); // Call led_driver_.closeDriver with the arguments shown here.
    command_queue_.close(); // Call command_queue_.close with the arguments shown here.
} // End this function or local scope.

uint32_t SystemController::begin_operation( // Begin this function/widget call; its arguments follow below.
    OperationKind kind, phono::protocol::CommandType command, uint32_t argument) { // Declare kind for this operation.
    auto operation_id = next_operation_id_++; // Initialize operation id to next_operation_id_++.
    if (operation_id == 0) operation_id = next_operation_id_++; // Test the condition operation_id == 0. If true, set operation id to next_operation_id_++.
    pending_ = PendingOperation{}; // Reset flags and stage to their defaults.
    pending_.kind = kind; // Set pending .kind to kind.
    pending_.command = command; // Set pending .command to command.
    pending_.operation_id = operation_id; // Set pending .operation id to operation_id.
    pending_.argument = argument; // Set pending .argument to argument.
    has_pending_operation_ = true; // Mark pending operation as available.
    return operation_id; // Return operation_id to the caller.
} // End this function or local scope.

// CONTROL: Main -> FileIO mailbox (one request record, not a socket call).
bool SystemController::request_file(FileIoManager::Request request) { // Attempt delivery to FileIO's one-slot control mailbox.
    if (writer_.request(request)) { // Test the condition writer_.request(request).
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    cerr << "FileIO request rejected because its mailbox is occupied\n"; // Print the diagnostic or test result for the operator.
    return false; // Report failure or no available item to the caller.
} // End this function or local scope.

bool SystemController::queue_file_request(FileIoManager::Request request) { // Deliver a FileIO request or retain it in Main for a later retry.
    if (writer_.request(request)) { // Test the condition writer_.request(request).
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    if (has_deferred_operation_) { // Test the condition has_deferred_operation_.
        cerr << "A deferred FileIO operation is already pending\n"; // Print the diagnostic or test result for the operator.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    // Main keeps this locally until FileIO's shared mailbox has a free slot.
    // It has not been delivered yet; dispatch_deferred_operation_request retries.
    deferred_operation_request_ = request; // Set deferred operation request to request.
    has_deferred_operation_ = true; // Mark deferred operation as available.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

// CONTROL: Main -> AudioDSP mailbox; AudioDSP executes the requested ALSA change.
bool SystemController::request_audio(AudioEngine::Request request) { // Attempt delivery to AudioDSP's one-slot control mailbox.
    if (audio_.request(request)) { // Test the condition audio_.request(request).
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    cerr << "Audio request rejected because its mailbox is occupied\n"; // Print the diagnostic or test result for the operator.
    return false; // Report failure or no available item to the caller.
} // End this function or local scope.

void SystemController::dispatch_deferred_operation_request() { // Retry Main's retained operation when the FileIO mailbox becomes free.
    if (!has_deferred_operation_) { // Test the condition !has_deferred_operation_.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    // RETRY DELIVERY: clear Main's local retry flag only after mailbox acceptance.
    if (writer_.request(deferred_operation_request_)) { // Test the condition writer_.request(deferred_operation_request_).
        has_deferred_operation_ = false; // Mark deferred operation as empty.
    } // End this block, declaration or argument list.
} // End this function or local scope.

void SystemController::dispatch_deferred_mount() { // Retry Main's retained storage-device request when FileIO can accept it.
    if (!has_deferred_storage_) { // Test the condition !has_deferred_storage_.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (writer_.request(deferred_storage_request_)) { // Test the condition writer_.request(deferred_storage_request_).
        has_deferred_storage_ = false; // Mark deferred storage as empty.
    } // End this block, declaration or argument list.
} // End this function or local scope.

void SystemController::publish_status() { // Derive the storage/button capability packet from current authoritative state.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    const bool healthy = error_code_.load() == 0; // Read the shared atomic value into this local snapshot.
    const bool mounted = (storage_flags_ & StorageMounted) != 0; // Initialize mounted to (storage_flags_ & StorageMounted) != 0.
    StatusPacket status; // Declare status for this operation.
    status.storage_mounted = mounted; // Set status.storage mounted to mounted.
    status.allow_record = mounted && audio_ready_.load() && healthy && // Read the shared atomic value into this local snapshot.
        (storage_flags_ & StorageWritable) != 0 && // Continue this calculation/condition with the next line.
        (storage_flags_ & StorageSpaceOk) != 0; // Complete this expression or declaration.
    status.allow_playback = mounted && audio_ready_.load() && healthy && // Read the shared atomic value into this local snapshot.
        (storage_flags_ & StorageReadable) != 0 && // Continue this calculation/condition with the next line.
        (storage_flags_ & StorageSpaceOk) != 0; // Complete this expression or declaration.
    status.error_code = static_cast<uint8_t>(error_code_.load()); // Read the shared atomic value into this local snapshot.
    if (status.error_code == 0 && led_error_.load()) { // Test the condition status.error_code == 0 && led_error_.load().
        status.error_code = static_cast<uint8_t>(Error::LedDriverError); // Set status.error code to static_cast<uint8_t>(Error::LedDriverError).
    } // End this block, declaration or argument list.
    network_.sendStatus(status); // Call network_.sendStatus with the arguments shown here.
} // End this function or local scope.

void SystemController::publish_state() { // Serialize transport, mode, gain, storage, session and diagnostic counters.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    unsigned mode; // Declare mode for this operation.
    unsigned gain; // Declare gain for this operation.
    { // Begin this local scope; scoped locks release when it ends.
        PthreadLock lock(configuration_mutex_); // Lock this shared object until the local scope ends, including an early return.
        mode = mode_.load(); // Read the shared atomic value into this local snapshot.
        gain = gain_.load(); // Read the shared atomic value into this local snapshot.
    } // End this block, declaration or argument list.
    State state; // Declare state for this operation.
    state.transport = transport_.load(); // Read the shared atomic value into this local snapshot.
    state.filter = static_cast<uint8_t>(mode); // Set state.filter to static_cast<uint8_t>(mode).
    state.gain = static_cast<uint8_t>(gain); // Set state.gain to static_cast<uint8_t>(gain).
    state.storage_flags = storage_flags_; // Set state.storage flags to storage_flags_.
    state.error_code = static_cast<uint8_t>(error_code_.load()); // Read the shared atomic value into this local snapshot.
    if (state.error_code == 0 && led_error_.load()) { // Test the condition state.error_code == 0 && led_error_.load().
        state.error_code = static_cast<uint8_t>(Error::LedDriverError); // Set state.error code to static_cast<uint8_t>(Error::LedDriverError).
    } // End this block, declaration or argument list.
    state.active_session = active_session_.load(); // Read the shared atomic value into this local snapshot.
    state.dropped_telemetry = dropped_telemetry_.load(); // Read the shared atomic value into this local snapshot.
    state.recording_overruns = recording_overruns_.load(); // Read the shared atomic value into this local snapshot.
    network_.queue_control(make_state_packet(state)); // Call network_.queue_control with the arguments shown here.
} // End this function or local scope.

void SystemController::publish_command_result( // Begin this function/widget call; its arguments follow below.
    phono::protocol::CommandType command, phono::protocol::Result result, // Declare command for this operation.
    uint32_t detail) { // Declare detail for this operation.
    network_.queue_control(phono::protocol::make_command_result_packet( // Call network_.queue_control with the arguments shown here.
        {command, result, detail})); // Complete the call above with these remaining arguments.
} // End this function or local scope.

void SystemController::update_leds() { // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
    // Write the complete target mask once; never erase the selected mode on STOP.
    uint8_t mask = 0; // Initialize mask to 0.
    if (recording_.load()) mask = 0x08; // Test the condition recording_.load(). If true, set mask to 0x08.
    const uint8_t mode_masks[] = {0x04, 0x01, 0x02}; // Initialize mode masks to {0x04, 0x01, 0x02}.
    mask |= mode_masks[min(mode_.load(), 2U)]; // Read the shared atomic value into this local snapshot.
    const bool ok = led_driver_.writeByte(mask); // Initialize ok to led_driver_.writeByte(mask).
    led_error_.store(!ok); // Update the shared led error value atomically.
} // End this function or local scope.

// Keep stable connection-scoped IDs for unchanged files, using filesystem identity.
void SystemController::handle_archive_result(const MainMessage& message) { // Update the stable connection-scoped file-ID registry from FileIO's scan.
    archive_entries_.clear(); // Discard the previous contents/state of this object.
    for (const auto& identity : message.archive) { // Visit the specified items/indexes in order.
        auto known = archive_id_by_name_.find(identity.basename); // Initialize known to archive_id_by_name_.find(identity.basename).
        bool unchanged = false; // Initialize unchanged to false.
        if (known != archive_id_by_name_.end()) { // Test the condition known != archive_id_by_name_.end().
            const auto old = archive_by_id_.find(known->second); // Initialize old to archive_by_id_.find(known->second).
            unchanged = // Continue the expression or parameter list from the preceding line.
                old != archive_by_id_.end() && // Continue this calculation/condition with the next line.
                old->second.device == identity.device && // Continue checking the file/state comparison; no value is assigned here.
                old->second.inode == identity.inode && // Continue checking the file/state comparison; no value is assigned here.
                old->second.size == identity.size && // Continue checking the file/state comparison; no value is assigned here.
                old->second.modified.tv_sec == identity.modified.tv_sec && // Continue checking the file/state comparison; no value is assigned here.
                old->second.modified.tv_nsec == identity.modified.tv_nsec; // Continue checking the file/state comparison; no value is assigned here.
        } // End this block, declaration or argument list.
        uint32_t id = 0; // Initialize id to 0.
        if (unchanged) { // Test the condition unchanged.
            id = known->second; // Set id to known->second.
            archive_by_id_[id] = identity; // Set archive by id [id] to identity.
        } else { // Follow the alternative path when the previous condition is false.
            if (next_file_id_ == 0) { // Test the condition next_file_id_ == 0.
                complete_operation(false, static_cast<uint8_t>( // Call complete_operation with the arguments shown here.
                                             phono::protocol::Error::StaleFileId)); // Complete the call above with these remaining arguments.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            id = next_file_id_++; // Set id to next_file_id_++.
            archive_id_by_name_[identity.basename] = id; // Set archive id by name [identity.basename] to id.
            archive_by_id_[id] = identity; // Set archive by id [id] to identity.
        } // End this block, declaration or argument list.
        if (identity.basename.size() <= // Test the condition identity.basename.size() <=.
                phono::protocol::kMaxBasenameBytes && // Continue this calculation/condition with the next line.
            identity.basename.find_first_of("\t\r\n/\\") == string::npos) { // Finish the parameters/condition and enter the block below.
            archive_entries_.push_back({id, identity.basename}); // Append the owned item at the back of this container.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    // Standard library sort calls compare_file_names for each comparison.
    sort(archive_entries_.begin(), archive_entries_.end(), compare_file_names); // Call sort with the arguments shown here.
    queue_archive_page(pending_.argument); // Prepare a bounded page of IDs and names for the GUI archive.
} // End this function or local scope.

void SystemController::queue_archive_page(uint32_t cursor) { // Prepare a bounded page of IDs and names for the GUI archive.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    size_t start = 0; // Initialize start to 0.
    if (cursor != 0) { // Test the condition cursor != 0.
        bool found = false; // Initialize found to false.
        for (size_t i = 0; i < archive_entries_.size(); i++) { // Visit the specified items/indexes in order.
            if (archive_entries_[i].id == cursor) { // Test the condition archive_entries_[i].id == cursor.
                start = i + 1; // Set start to i + 1.
                found = true; // Set found to true.
                break; // Leave the current loop or switch branch.
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
        if (!found) { // Test the condition !found.
            complete_operation(false, static_cast<uint8_t>(Error::StaleFileId)); // Publish the final command result and clear Main's pending context.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    vector<FileEntry> page; // Declare page for this operation.
    size_t payload_size = string("next_cursor=4294967295\n").size(); // Initialize payload size to string("next_cursor=4294967295\n").size().
    for (size_t i = start; // Visit the specified items/indexes in order.
         i < archive_entries_.size() && page.size() < kMaxFileListEntries; // Complete this expression or declaration.
         ++i) { // Advance this index/counter for the next item.
        const auto& entry = archive_entries_[i]; // Initialize entry to archive_entries_[i].
        const auto entry_size = // Start initializing this local value from the expression on the next line.
            to_string(entry.id).size() + 1 + entry.basename.size() + 1; // Call to_string with the arguments shown here.
        if (payload_size + entry_size > kMaxFileListPayloadBytes) break; // Test the condition payload_size + entry_size > kMaxFileListPayloadBytes. If true, leave the current loop or switch branch.
        page.push_back(entry); // Append the owned item at the back of this container.
        payload_size += entry_size; // Update payload size using += entry_size.
    } // End this block, declaration or argument list.
    const bool more = start + page.size() < archive_entries_.size(); // Initialize more to start + page.size() < archive_entries_.size().
    uint32_t next_cursor = 0; // Initialize next cursor to 0.
    if (more && !page.empty()) next_cursor = page.back().id; // Test the condition more && !page.empty(). If true, set next cursor to page.back().id.
    try { // Catch failures from the operations in this block.
        network_.sendFileList(next_cursor, page); // Call network_.sendFileList with the arguments shown here.
        complete_operation(true); // Publish the final command result and clear Main's pending context.
    } catch (const exception& error) { // Handle the stated exception instead of continuing with an invalid result.
        cerr << "Cannot serialize archive page: " << error.what() << '\n'; // Print the diagnostic or test result for the operator.
        complete_operation(false, static_cast<uint8_t>(Error::ProtocolError)); // Publish the final command result and clear Main's pending context.
    } // End this block, declaration or argument list.
} // End this function or local scope.

void SystemController::set_error(uint8_t error_code) { // Publish the current system error code atomically.
    error_code_.store(error_code); // Update the shared error code value atomically.
} // End this function or local scope.

void SystemController::set_main_thread_policy() { // Request CPU 0 affinity and SCHED_OTHER priority 0 with nice 0.
    // Main and Network are peers: SCHED_OTHER priority 0, nice 0.
    sched_param normal_priority{}; // Declare normal priority for this operation.
    /* API: pthread_setschedparam
     * Request the scheduling policy and sched_priority stored in sched_param. FIFO 80 is AudioDSP and FIFO 50 is FileIO. SCHED_OTHER requires priority 0.
     */
    /* API: pthread_self
     * Return the calling pthread handle, used when setting this worker's affinity or scheduling policy.
     */
    const int policy_error = pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal_priority); // Initialize policy error to pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal_priority).
    if (policy_error != 0) cerr << "Cannot set normal scheduling: " << strerror(policy_error) << '\n'; // Test the condition policy_error != 0. If true, print the diagnostic or test result for the operator.
    /* API: setpriority
     * Set the calling Linux thread's nice value with PRIO_PROCESS and ID 0. Nice 0 is normal SCHED_OTHER weight; it is distinct from FIFO priority 50/80. Returns 0 or -1 with errno.
     */
    if (setpriority(PRIO_PROCESS, 0, 0) != 0) cerr << "Cannot set nice 0: " << strerror(errno) << '\n'; // Test the condition setpriority(PRIO_PROCESS, 0, 0) != 0. If true, print the diagnostic or test result for the operator.

    cpu_set_t cpus; // Declare cpus for this operation.
    CPU_ZERO(&cpus); // Call CPU_ZERO with the arguments shown here.
    CPU_SET(0, &cpus); // Call CPU_SET with the arguments shown here.
    const int result = // Start initializing this local value from the expression on the next line.
        /* API: pthread_setaffinity_np
         * Ask Linux to run this pthread on the CPUs in cpu_set_t. pthread_self identifies the current worker. Check the returned error number.
         */
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus); // Call pthread_setaffinity_np with the arguments shown here.
    if (result != 0) { // Test the condition result != 0.
        cerr << "Main thread CPU affinity not set: " << strerror(result) // Print the diagnostic or test result for the operator.
                  << '\n'; // Append this part of the diagnostic output.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// The same queue carries GUI commands and worker replies; kind tells them apart.
void SystemController::processMessage(const MainMessage& message) { // Route MainMessage by kind to command handling or worker-result handling.
    if (message.kind == MessageKind::GUI_COMMAND) { // Test the condition message.kind == MessageKind::GUI_COMMAND.
        processCommand(message.command); // Validate a decoded GUI command and dispatch its named system operation.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (message.kind == MessageKind::WORKER_ERROR) { // Test the condition message.kind == MessageKind::WORKER_ERROR.
        set_error(message.error_code); // Publish the current system error code atomically.
    } // End this block, declaration or argument list.
    handleWorkerResult(message); // Match a worker result to the pending operation before committing system state.
} // End this function or local scope.

// Returns true when a notification was handled without a pending GUI operation.
bool SystemController::handle_notification(const MainMessage& message) { // Handle unsolicited worker/connection events outside an active GUI transition.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    if (message.result_code == WorkerResultCode::NETWORK_FAILED) { // Test the condition message.result_code == WorkerResultCode::NETWORK_FAILED.
        if (message.error_code != 0) set_error(message.error_code); // Test the condition message.error_code != 0. If true, publish the current system error code atomically.
        running.store(false); // Update the shared running value atomically.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    if (message.result_code == WorkerResultCode::NETWORK_STARTED) { // Test the condition message.result_code == WorkerResultCode::NETWORK_STARTED.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    if (message.result_code == WorkerResultCode::CLIENT_CONNECTED) { // Test the condition message.result_code == WorkerResultCode::CLIENT_CONNECTED.
        archive_by_id_.clear(); // Discard the previous contents/state of this object.
        archive_id_by_name_.clear(); // Discard the previous contents/state of this object.
        archive_entries_.clear(); // Discard the previous contents/state of this object.
        next_file_id_ = 1; // Set next file id to 1.
        broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    if (message.result_code == WorkerResultCode::CLIENT_DISCONNECTED) { // Test the condition message.result_code == WorkerResultCode::CLIENT_DISCONNECTED.
        archive_by_id_.clear(); // Discard the previous contents/state of this object.
        archive_id_by_name_.clear(); // Discard the previous contents/state of this object.
        archive_entries_.clear(); // Discard the previous contents/state of this object.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    if (message.result_code == WorkerResultCode::AUDIO_READY) { // Test the condition message.result_code == WorkerResultCode::AUDIO_READY.
        audio_ready_.store(message.kind == MessageKind::WORKER_RESULT); // Update the shared audio ready value atomically.
        audio_init_complete_.store(true); // Update the shared audio init complete value atomically.
        if (message.error_code == 0) error_code_.store(0); // Test the condition message.error_code == 0. If true, publish the new shared error code value atomically.
        if (message.error_code != 0) { // Test the condition message.error_code != 0.
            transport_.store(phono::protocol::Transport::Error); // Update the shared transport value atomically.
        } // End this block, declaration or argument list.
        broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    if (message.result_code == WorkerResultCode::FILEIO_READY || // Test the condition message.result_code == WorkerResultCode::FILEIO_READY ||.
        message.result_code == WorkerResultCode::STORAGE_CHANGED) { // Continue checking the file/state comparison; no value is assigned here.
        const auto changed = storage_flags_ != message.storage_flags; // Initialize changed to storage_flags_ != message.storage_flags.
        storage_flags_ = message.storage_flags; // Set storage flags to message.storage_flags.
        const auto storage_usable = // Start initializing this local value from the expression on the next line.
            (storage_flags_ & (StorageMounted | StorageSpaceOk)) == // Continue the expression or parameter list from the preceding line.
                (StorageMounted | StorageSpaceOk) && // Continue this calculation/condition with the next line.
            (storage_flags_ & (StorageReadable | StorageWritable)) != 0; // Complete this expression or declaration.
        const bool recovered = // Start initializing this local value from the expression on the next line.
            message.kind == MessageKind::WORKER_RESULT && storage_usable && // Continue checking the file/state comparison; no value is assigned here.
            error_code_.load() == // Read the shared atomic value into this local snapshot.
                static_cast<unsigned>(Error::StorageLost); // Complete the call above with these remaining arguments.
        if (recovered) { // Test the condition recovered.
            error_code_.store(0); // Update the shared error code value atomically.
            if (transport_.load() == Transport::Error) { // Test the condition transport_.load() == Transport::Error.
                transport_.store(Transport::Live); // Update the shared transport value atomically.
            } // End this block, declaration or argument list.
            update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
        } // End this block, declaration or argument list.
        if (changed || message.result_code == WorkerResultCode::FILEIO_READY || // Test the condition changed || message.result_code == WorkerResultCode::FILEIO_READY ||.
            message.kind == MessageKind::WORKER_ERROR || recovered) { // Continue checking the file/state comparison; no value is assigned here.
            broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
        } // End this block, declaration or argument list.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    if (message.result_code == WorkerResultCode::PLAYBACK_FINISHED && // Test the condition message.result_code == WorkerResultCode::PLAYBACK_FINISHED &&.
        !has_pending_operation_) { // Finish the parameters/condition and enter the block below.
        start_playback_cleanup(message.operation_id, false, message.error_code); // Stop the consumer before FileIO closes/discards playback data.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    if (message.result_code == WorkerResultCode::RECORD_CLOSED && // Test the condition message.result_code == WorkerResultCode::RECORD_CLOSED &&.
        !has_pending_operation_ && // Continue this calculation/condition with the next line.
        transport_.load() == Transport::Recording) { // Read the shared atomic value into this local snapshot.
        recording_.store(false); // Update the shared recording value atomically.
        active_session_.store(0); // Update the shared active session value atomically.
        if (message.operation_id == 0) { // Test the condition message.operation_id == 0.
            FileIoManager::Request close_request; // Declare close request for this operation.
            close_request.kind = FileIoManager::RequestKind::CLOSE_RECORD; // Set close request.kind to FileIoManager::RequestKind::CLOSE_RECORD.
            request_file(close_request); // Attempt delivery to FileIO's one-slot control mailbox.
        } // End this block, declaration or argument list.
        transport_.store(message.error_code == 0 ? Transport::Live // Publish the new shared transport value atomically.
                                                  : Transport::Error); // Supply the next constructor initializer or argument.
        update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
        if (message.error_code != 0) set_error(message.error_code); // Test the condition message.error_code != 0. If true, publish the current system error code atomically.
        broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    return false; // Report failure or no available item to the caller.
} // End this function or local scope.

// Failed operations still close files and restore capture where necessary.
// Keeping this separate makes the normal success path easier to follow.
void SystemController::handle_operation_error(const MainMessage& message) { // Fail the matching transition and coordinate any required cleanup.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    pending_.error_code = message.error_code; // Set pending .error code to message.error_code.
    if (pending_.kind == OperationKind::StartPlayback) { // Test the condition pending_.kind == OperationKind::StartPlayback.
        if (message.result_code == WorkerResultCode::CAPTURE_RESUMED) { // Test the condition message.result_code == WorkerResultCode::CAPTURE_RESUMED.
            pending_.audio_done = true; // Set pending .audio done to true.
            transport_.store(phono::protocol::Transport::Error); // Update the shared transport value atomically.
            if (pending_.file_done) { // Test the condition pending_.file_done.
                complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        if (message.result_code == WorkerResultCode::PLAYBACK_CLOSED) { // Test the condition message.result_code == WorkerResultCode::PLAYBACK_CLOSED.
            pending_.file_done = true; // Set pending .file done to true.
            transport_.store(Transport::Error); // Update the shared transport value atomically.
            if (pending_.audio_done) { // Test the condition pending_.audio_done.
                complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        fail_start_playback(message.error_code); // Undo partial playback preparation and report the failure.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (pending_.kind == OperationKind::StartRecording || // Test the condition pending_.kind == OperationKind::StartRecording ||.
        pending_.kind == OperationKind::StopRecording || // Continue checking the file/state comparison; no value is assigned here.
        pending_.kind == OperationKind::ListArchive || // Continue checking the file/state comparison; no value is assigned here.
        pending_.kind == OperationKind::DeleteRecording || // Continue checking the file/state comparison; no value is assigned here.
        (pending_.kind == OperationKind::EjectStorage && // Continue this calculation/condition with the next line.
         pending_.argument != // Continue the expression or parameter list from the preceding line.
             static_cast<uint32_t>(Transport::Playback))) { // Finish the parameters/condition and enter the block below.
        if (pending_.kind == OperationKind::StartRecording || // Test the condition pending_.kind == OperationKind::StartRecording ||.
            pending_.kind == OperationKind::StopRecording || // Continue checking the file/state comparison; no value is assigned here.
            pending_.kind == OperationKind::EjectStorage) { // Continue checking the file/state comparison; no value is assigned here.
            recording_.store(false); // Update the shared recording value atomically.
            active_session_.store(0); // Update the shared active session value atomically.
            transport_.store(Transport::Live); // Update the shared transport value atomically.
            update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
        } // End this block, declaration or argument list.
        complete_operation(false, message.error_code); // Publish the final command result and clear Main's pending context.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (pending_.kind == OperationKind::EjectStorage && // Test the condition pending_.kind == OperationKind::EjectStorage &&.
        pending_.argument == // Continue checking the file/state comparison; no value is assigned here.
            static_cast<uint32_t>(Transport::Playback)) { // Finish the parameters/condition and enter the block below.
        if (message.result_code == WorkerResultCode::PLAYBACK_CLOSED) { // Test the condition message.result_code == WorkerResultCode::PLAYBACK_CLOSED.
            pending_.file_done = true; // Set pending .file done to true.
        } else if (message.result_code == WorkerResultCode::PLAYBACK_STOPPED || // Follow the alternative path when the previous condition is false.
                   message.result_code == // Continue checking the file/state comparison; no value is assigned here.
                       WorkerResultCode::PLAYBACK_FINISHED) { // Finish the parameters/condition and enter the block below.
            pending_.audio_done = true; // Set pending .audio done to true.
        } else if (message.result_code == WorkerResultCode::CAPTURE_RESUMED) { // Follow the alternative path when the previous condition is false.
            pending_.audio_done = true; // Set pending .audio done to true.
        } else { // Follow the alternative path when the previous condition is false.
            transport_.store(Transport::Error); // Update the shared transport value atomically.
            complete_operation(false, message.error_code); // Publish the final command result and clear Main's pending context.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        if (pending_.stage == WorkflowStage::WaitingForWorkers && pending_.file_done && // Test the condition pending_.stage == WorkflowStage::WaitingForWorkers && pending_.file_done &&.
            pending_.audio_done) { // Finish the parameters/condition and enter the block below.
            pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
            if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                                pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                transport_.store(Transport::Error); // Update the shared transport value atomically.
                complete_operation(false, // Call complete_operation with the arguments shown here.
                                   static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
            } // End this block, declaration or argument list.
        } else if (pending_.stage == WorkflowStage::ResumingCapture && pending_.file_done && // Follow the alternative path when the previous condition is false.
                   pending_.audio_done) { // Finish the parameters/condition and enter the block below.
            transport_.store(Transport::Live); // Update the shared transport value atomically.
            complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
        } // End this block, declaration or argument list.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (pending_.kind == OperationKind::StopPlayback) { // Test the condition pending_.kind == OperationKind::StopPlayback.
        if (pending_.stage == WorkflowStage::ResumingCapture) { // Test the condition pending_.stage == WorkflowStage::ResumingCapture.
            if (message.result_code == WorkerResultCode::PLAYBACK_CLOSED) { // Test the condition message.result_code == WorkerResultCode::PLAYBACK_CLOSED.
                pending_.file_done = true; // Set pending .file done to true.
            } else if (message.result_code == // Follow the alternative path when the previous condition is false.
                       WorkerResultCode::CAPTURE_RESUMED) { // Finish the parameters/condition and enter the block below.
                pending_.audio_done = true; // Set pending .audio done to true.
            } // End this block, declaration or argument list.
            if (pending_.file_done && pending_.audio_done) { // Test the condition pending_.file_done && pending_.audio_done.
                transport_.store( // Publish the new shared transport value atomically.
                    Transport::Error); // Complete the call above with these remaining arguments.
                complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        if (message.result_code == WorkerResultCode::PLAYBACK_CLOSED) { // Test the condition message.result_code == WorkerResultCode::PLAYBACK_CLOSED.
            pending_.file_done = true; // Set pending .file done to true.
        } else if (message.result_code == WorkerResultCode::PLAYBACK_STOPPED || // Follow the alternative path when the previous condition is false.
                   message.result_code == // Continue checking the file/state comparison; no value is assigned here.
                       WorkerResultCode::PLAYBACK_FINISHED) { // Finish the parameters/condition and enter the block below.
            pending_.audio_done = true; // Set pending .audio done to true.
        } else { // Follow the alternative path when the previous condition is false.
            transport_.store(Transport::Error); // Update the shared transport value atomically.
            complete_operation(false, message.error_code); // Publish the final command result and clear Main's pending context.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        if (pending_.file_done && pending_.audio_done) { // Test the condition pending_.file_done && pending_.audio_done.
            pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
            pending_.commit_requested = true; // Set pending .commit requested to true.
            if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                                pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                transport_.store(Transport::Error); // Update the shared transport value atomically.
                complete_operation(false, // Call complete_operation with the arguments shown here.
                                   static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// FLOWCHART: FileIO opened/closed the WAV -> Main enables/stops recording.
void SystemController::handle_recording_result(const MainMessage& message) { // Advance recording open/close only after the matching FileIO result.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    switch (message.result_code) { // Choose one branch from the supplied command/state value.
        case WorkerResultCode::OPEN_READY: // Handle the WorkerResultCode::OPEN_READY choice in this switch.
            if (pending_.kind != OperationKind::StartRecording) { // Test the condition pending_.kind != OperationKind::StartRecording.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (!recording_buffer_.begin_session()) { // Test the condition !recording_buffer_.begin_session().
                pending_.error_code = static_cast<uint8_t>(Error::Busy); // Set pending .error code to static_cast<uint8_t>(Error::Busy).
                FileIoManager::Request close_request; // Declare close request for this operation.
                close_request.kind = FileIoManager::RequestKind::CLOSE_RECORD; // Set close request.kind to FileIoManager::RequestKind::CLOSE_RECORD.
                close_request.operation_id = pending_.operation_id; // Set close request.operation id to pending_.operation_id.
                if (!request_file(close_request)) { // Test the condition !request_file(close_request).
                    transport_.store(Transport::Error); // Update the shared transport value atomically.
                    complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            active_session_.store(pending_.argument); // Update the shared active session value atomically.
            recording_.store(true); // Update the shared recording value atomically.
            transport_.store(Transport::Recording); // Update the shared transport value atomically.
            update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
            complete_operation(true); // Publish the final command result and clear Main's pending context.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::RECORD_CLOSED: // Handle the WorkerResultCode::RECORD_CLOSED choice in this switch.
            if (pending_.kind == OperationKind::StartRecording) { // Test the condition pending_.kind == OperationKind::StartRecording.
                recording_.store(false); // Update the shared recording value atomically.
                active_session_.store(0); // Update the shared active session value atomically.
                transport_.store(Transport::Live); // Update the shared transport value atomically.
                update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
                complete_operation(false, // Call complete_operation with the arguments shown here.
                                   pending_.error_code == 0 // Continue checking the file/state comparison; no value is assigned here.
                                       ? static_cast<uint8_t>(Error::Busy) // Convert to the explicitly stated numeric type for this API or record.
                                       : pending_.error_code); // Supply the next constructor initializer or argument.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.kind == OperationKind::EjectStorage) { // Test the condition pending_.kind == OperationKind::EjectStorage.
                if (message.error_code != 0) { // Test the condition message.error_code != 0.
                    transport_.store(Transport::Live); // Update the shared transport value atomically.
                    complete_operation(false, message.error_code); // Publish the final command result and clear Main's pending context.
                    return; // Leave this function; any local scoped locks are released.
                } // End this block, declaration or argument list.
                recording_.store(false); // Update the shared recording value atomically.
                active_session_.store(0); // Update the shared active session value atomically.
                FileIoManager::Request eject_request; // Declare eject request for this operation.
                eject_request.kind = FileIoManager::RequestKind::EJECT; // Set eject request.kind to FileIoManager::RequestKind::EJECT.
                eject_request.operation_id = pending_.operation_id; // Set eject request.operation id to pending_.operation_id.
                pending_.stage = WorkflowStage::EjectingStorage; // Set pending .stage to WorkflowStage::EjectingStorage.
                if (!request_file(eject_request)) { // Test the condition !request_file(eject_request).
                    transport_.store(Transport::Live); // Update the shared transport value atomically.
                    complete_operation(false, static_cast<uint8_t>(Error::Busy)); // Publish the final command result and clear Main's pending context.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.kind != OperationKind::StopRecording) { // Test the condition pending_.kind != OperationKind::StopRecording.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            recording_.store(false); // Update the shared recording value atomically.
            active_session_.store(0); // Update the shared active session value atomically.
            transport_.store(Transport::Live); // Update the shared transport value atomically.
            update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
            complete_operation(message.error_code == 0, message.error_code); // Publish the final command result and clear Main's pending context.
            return; // Leave this function; any local scoped locks are released.
        default: // Handle values not matched by an earlier case.
            return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// FileIO and AudioDSP reply independently. Wait for both before the next stage.
void SystemController::handle_playback_result(const MainMessage& message) { // Advance playback preparation/stop through worker acknowledgements.
    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.
    switch (message.result_code) { // Choose one branch from the supplied command/state value.
        case WorkerResultCode::PLAYBACK_READY: // Handle the WorkerResultCode::PLAYBACK_READY choice in this switch.
        case WorkerResultCode::CAPTURE_SUSPENDED: // Handle the WorkerResultCode::CAPTURE_SUSPENDED choice in this switch.
            if (pending_.kind != OperationKind::StartPlayback || // Test the condition pending_.kind != OperationKind::StartPlayback ||.
                pending_.error_code != 0) return; // Complete this expression or declaration.
            if (message.result_code == WorkerResultCode::PLAYBACK_READY) { // Test the condition message.result_code == WorkerResultCode::PLAYBACK_READY.
                pending_.file_done = true; // Set pending .file done to true.
            } else { // Follow the alternative path when the previous condition is false.
                pending_.audio_done = true; // Set pending .audio done to true.
            } // End this block, declaration or argument list.
            // Both workers must be ready before AudioDSP can start playback.
            if (pending_.file_done && pending_.audio_done && // Test the condition pending_.file_done && pending_.audio_done &&.
                !pending_.commit_requested) { // Finish the parameters/condition and enter the block below.
                pending_.commit_requested = true; // Set pending .commit requested to true.
                if (!request_audio({AudioEngine::RequestKind::START_PLAYBACK, // Test the condition !request_audio({AudioEngine::RequestKind::START_PLAYBACK,.
                                    pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                    fail_start_playback(static_cast<uint8_t>(Error::Busy)); // Undo partial playback preparation and report the failure.
                } // End this block, declaration or argument list.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::PLAYBACK_STARTED: // Handle the WorkerResultCode::PLAYBACK_STARTED choice in this switch.
            if (pending_.kind != OperationKind::StartPlayback) { // Test the condition pending_.kind != OperationKind::StartPlayback.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            transport_.store(Transport::Playback); // Update the shared transport value atomically.
            complete_operation(true); // Publish the final command result and clear Main's pending context.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::PLAYBACK_CLOSED: // Handle the WorkerResultCode::PLAYBACK_CLOSED choice in this switch.
            if (pending_.kind == OperationKind::StartPlayback && // Test the condition pending_.kind == OperationKind::StartPlayback &&.
                pending_.error_code != 0) { // Finish the parameters/condition and enter the block below.
                pending_.file_done = true; // Set pending .file done to true.
                if (pending_.audio_done) { // Test the condition pending_.audio_done.
                    if (transport_.load() != Transport::Error) { // Test the condition transport_.load() != Transport::Error.
                        transport_.store(Transport::Live); // Update the shared transport value atomically.
                    } // End this block, declaration or argument list.
                    complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.kind == OperationKind::EjectStorage) { // Test the condition pending_.kind == OperationKind::EjectStorage.
                if (pending_.stage != WorkflowStage::WaitingForWorkers) { // Test the condition pending_.stage != WorkflowStage::WaitingForWorkers.
                    return; // Leave this function; any local scoped locks are released.
                } // End this block, declaration or argument list.
                pending_.file_done = true; // Set pending .file done to true.
                if (pending_.audio_done) { // Test the condition pending_.audio_done.
                    pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
                    if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                                        pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                        transport_.store(Transport::Live); // Update the shared transport value atomically.
                        complete_operation(false, // Call complete_operation with the arguments shown here.
                                           static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
                    } // End this block, declaration or argument list.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.kind != OperationKind::StopPlayback) { // Test the condition pending_.kind != OperationKind::StopPlayback.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            pending_.file_done = true; // Set pending .file done to true.
            if (pending_.stage == WorkflowStage::ResumingCapture) { // Test the condition pending_.stage == WorkflowStage::ResumingCapture.
                if (pending_.audio_done) { // Test the condition pending_.audio_done.
                    transport_.store(pending_.error_code == 0 // Publish the new shared transport value atomically.
                                         ? Transport::Live // Continue the expression or parameter list from the preceding line.
                                         : Transport::Error); // Supply the next constructor initializer or argument.
                    complete_operation(pending_.error_code == 0, // Call complete_operation with the arguments shown here.
                                       pending_.error_code); // Complete the call above with these remaining arguments.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.audio_done && !pending_.commit_requested) { // Test the condition pending_.audio_done && !pending_.commit_requested.
                pending_.commit_requested = true; // Set pending .commit requested to true.
                pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
                if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                                    pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                    transport_.store(Transport::Error); // Update the shared transport value atomically.
                    complete_operation(false, // Call complete_operation with the arguments shown here.
                                       static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
                } // End this block, declaration or argument list.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::PLAYBACK_STOPPED: // Handle the WorkerResultCode::PLAYBACK_STOPPED choice in this switch.
            if (pending_.kind == OperationKind::EjectStorage) { // Test the condition pending_.kind == OperationKind::EjectStorage.
                if (pending_.stage != WorkflowStage::WaitingForWorkers) { // Test the condition pending_.stage != WorkflowStage::WaitingForWorkers.
                    return; // Leave this function; any local scoped locks are released.
                } // End this block, declaration or argument list.
                pending_.audio_done = true; // Set pending .audio done to true.
                if (pending_.file_done) { // Test the condition pending_.file_done.
                    pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
                    if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                                        pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                        transport_.store(Transport::Live); // Update the shared transport value atomically.
                        complete_operation(false, // Call complete_operation with the arguments shown here.
                                           static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
                    } // End this block, declaration or argument list.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.kind != OperationKind::StopPlayback) { // Test the condition pending_.kind != OperationKind::StopPlayback.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.stage != WorkflowStage::WaitingForWorkers) { // Test the condition pending_.stage != WorkflowStage::WaitingForWorkers.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            pending_.audio_done = true; // Set pending .audio done to true.
            if (pending_.file_done && !pending_.commit_requested) { // Test the condition pending_.file_done && !pending_.commit_requested.
                pending_.commit_requested = true; // Set pending .commit requested to true.
                pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
                if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                                    pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                    transport_.store(Transport::Error); // Update the shared transport value atomically.
                    complete_operation(false, // Call complete_operation with the arguments shown here.
                                       static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
                } // End this block, declaration or argument list.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::PLAYBACK_FINISHED: // Handle the WorkerResultCode::PLAYBACK_FINISHED choice in this switch.
            if (pending_.kind == OperationKind::EjectStorage && // Test the condition pending_.kind == OperationKind::EjectStorage &&.
                pending_.argument == // Continue checking the file/state comparison; no value is assigned here.
                    static_cast<uint32_t>(Transport::Playback) && // Continue this calculation/condition with the next line.
                pending_.stage == WorkflowStage::WaitingForWorkers) { // Continue checking the file/state comparison; no value is assigned here.
                pending_.audio_done = true; // Set pending .audio done to true.
                if (pending_.file_done) { // Test the condition pending_.file_done.
                    pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
                    if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                                        pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                        transport_.store(Transport::Error); // Update the shared transport value atomically.
                        complete_operation(false, // Call complete_operation with the arguments shown here.
                                           static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
                    } // End this block, declaration or argument list.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.kind == OperationKind::StopPlayback && // Test the condition pending_.kind == OperationKind::StopPlayback &&.
                pending_.stage == WorkflowStage::WaitingForWorkers) { // Continue checking the file/state comparison; no value is assigned here.
                pending_.audio_done = true; // Set pending .audio done to true.
                if (pending_.file_done && !pending_.commit_requested) { // Test the condition pending_.file_done && !pending_.commit_requested.
                    pending_.commit_requested = true; // Set pending .commit requested to true.
                    pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
                    if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                                        pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
                        transport_.store(Transport::Error); // Update the shared transport value atomically.
                        complete_operation(false, // Call complete_operation with the arguments shown here.
                                           static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
                    } // End this block, declaration or argument list.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        case WorkerResultCode::CAPTURE_RESUMED: // Handle the WorkerResultCode::CAPTURE_RESUMED choice in this switch.
            if (pending_.kind == OperationKind::StartPlayback && // Test the condition pending_.kind == OperationKind::StartPlayback &&.
                pending_.error_code != 0) { // Finish the parameters/condition and enter the block below.
                pending_.audio_done = true; // Set pending .audio done to true.
                if (pending_.file_done) { // Test the condition pending_.file_done.
                    if (transport_.load() != Transport::Error) { // Test the condition transport_.load() != Transport::Error.
                        transport_.store(Transport::Live); // Update the shared transport value atomically.
                    } // End this block, declaration or argument list.
                    complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.kind == OperationKind::EjectStorage) { // Test the condition pending_.kind == OperationKind::EjectStorage.
                if (pending_.error_code != 0) { // Test the condition pending_.error_code != 0.
                    transport_.store(Transport::Live); // Update the shared transport value atomically.
                    complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
                    return; // Leave this function; any local scoped locks are released.
                } // End this block, declaration or argument list.
                FileIoManager::Request eject_request; // Declare eject request for this operation.
                eject_request.kind = FileIoManager::RequestKind::EJECT; // Set eject request.kind to FileIoManager::RequestKind::EJECT.
                eject_request.operation_id = pending_.operation_id; // Set eject request.operation id to pending_.operation_id.
                pending_.stage = WorkflowStage::EjectingStorage; // Set pending .stage to WorkflowStage::EjectingStorage.
                if (!request_file(eject_request)) { // Test the condition !request_file(eject_request).
                    transport_.store(Transport::Live); // Update the shared transport value atomically.
                    complete_operation(false, static_cast<uint8_t>(Error::Busy)); // Publish the final command result and clear Main's pending context.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            if (pending_.kind == OperationKind::StopPlayback) { // Test the condition pending_.kind == OperationKind::StopPlayback.
                if (pending_.stage != WorkflowStage::ResumingCapture) { // Test the condition pending_.stage != WorkflowStage::ResumingCapture.
                    return; // Leave this function; any local scoped locks are released.
                } // End this block, declaration or argument list.
                pending_.audio_done = true; // Set pending .audio done to true.
                if (pending_.file_done) { // Test the condition pending_.file_done.
                    transport_.store(pending_.error_code == 0 // Publish the new shared transport value atomically.
                                         ? Transport::Live // Continue the expression or parameter list from the preceding line.
                                         : Transport::Error); // Supply the next constructor initializer or argument.
                    complete_operation(pending_.error_code == 0, // Call complete_operation with the arguments shown here.
                                       pending_.error_code); // Complete the call above with these remaining arguments.
                } // End this block, declaration or argument list.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            return; // Leave this function; any local scoped locks are released.
        default: // Handle values not matched by an earlier case.
            return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// Clear the pending request and send its final result, capabilities and state to the GUI.
void SystemController::complete_operation(bool success, uint8_t error_code) { // Publish the final command result and clear Main's pending context.
    if (!has_pending_operation_) { // Test the condition !has_pending_operation_.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto operation = pending_; // Initialize operation to pending_.
    has_pending_operation_ = false; // Mark pending operation as empty.
    if (!success && error_code != 0) set_error(error_code); // Test the condition !success && error_code != 0. If true, publish the current system error code atomically.
    if (operation.report_result) { // Test the condition operation.report_result.
        if (success) { // Test the condition success.
            publish_command_result(operation.command, // Call publish_command_result with the arguments shown here.
                                   phono::protocol::Result::Completed, 0); // Complete the call above with these remaining arguments.
        } else { // Follow the alternative path when the previous condition is false.
            publish_command_result(operation.command, // Call publish_command_result with the arguments shown here.
                                   phono::protocol::Result::Failed, error_code); // Declare error code for this operation.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
} // End this function or local scope.

// EOF uses the same asynchronous close/resume coordination as an explicit stop.
void SystemController::start_playback_cleanup(uint32_t operation_id, // Pass this next argument/value to the call or initializer above.
                                              bool report_result, // Declare report result for this operation.
                                              uint8_t error_code) { // Declare error code for this operation.
    if (has_pending_operation_) { // Test the condition has_pending_operation_.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    pending_ = PendingOperation{}; // Set pending to PendingOperation{}.
    pending_.kind = OperationKind::StopPlayback; // Set pending .kind to OperationKind::StopPlayback.
    pending_.command = phono::protocol::CommandType::STOP_PLAYBACK; // Set pending .command to phono::protocol::CommandType::STOP_PLAYBACK.
    pending_.operation_id = operation_id; // Set pending .operation id to operation_id.
    pending_.error_code = error_code; // Set pending .error code to error_code.
    pending_.report_result = report_result; // Set pending .report result to report_result.
    pending_.commit_requested = true; // Set pending .commit requested to true.
    pending_.stage = WorkflowStage::ResumingCapture; // Set pending .stage to WorkflowStage::ResumingCapture.
    has_pending_operation_ = true; // Mark pending operation as available.
    transport_.store(phono::protocol::Transport::Stopping); // Update the shared transport value atomically.
    FileIoManager::Request file_request; // Declare file request for this operation.
    file_request.kind = FileIoManager::RequestKind::STOP_PLAYBACK; // Set file request.kind to FileIoManager::RequestKind::STOP_PLAYBACK.
    file_request.operation_id = operation_id; // Set file request.operation id to operation_id.
    if (!request_file(file_request)) { // Test the condition !request_file(file_request).
        pending_.file_done = true; // Set pending .file done to true.
        pending_.error_code = static_cast<uint8_t>( // Set pending .error code to static_cast<uint8_t>(.
            phono::protocol::Error::Busy); // Complete the call above with these remaining arguments.
        transport_.store(phono::protocol::Transport::Error); // Update the shared transport value atomically.
    } // End this block, declaration or argument list.
    if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                        operation_id})) { // Finish the parameters/condition and enter the block below.
        pending_.audio_done = true; // Set pending .audio done to true.
        pending_.error_code = static_cast<uint8_t>( // Set pending .error code to static_cast<uint8_t>(.
            phono::protocol::Error::Busy); // Complete the call above with these remaining arguments.
        transport_.store(phono::protocol::Transport::Error); // Update the shared transport value atomically.
    } // End this block, declaration or argument list.
    if (pending_.file_done && pending_.audio_done) { // Test the condition pending_.file_done && pending_.audio_done.
        complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
    } // End this block, declaration or argument list.
} // End this function or local scope.

void SystemController::fail_start_playback(uint8_t error_code) { // Undo partial playback preparation and report the failure.
    if (!has_pending_operation_ || // Test the condition !has_pending_operation_ ||.
        pending_.kind != OperationKind::StartPlayback) { // Finish the parameters/condition and enter the block below.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    pending_.error_code = error_code; // Set pending .error code to error_code.
    pending_.commit_requested = true; // Set pending .commit requested to true.
    pending_.file_done = false; // Set pending .file done to false.
    pending_.audio_done = false; // Set pending .audio done to false.

    FileIoManager::Request stop_request; // Declare stop request for this operation.
    stop_request.kind = FileIoManager::RequestKind::STOP_PLAYBACK; // Set stop request.kind to FileIoManager::RequestKind::STOP_PLAYBACK.
    stop_request.operation_id = pending_.operation_id; // Set stop request.operation id to pending_.operation_id.
    if (!request_file(stop_request)) { // Test the condition !request_file(stop_request).
        pending_.file_done = true; // Set pending .file done to true.
        transport_.store(phono::protocol::Transport::Error); // Update the shared transport value atomically.
    } // End this block, declaration or argument list.
    if (!request_audio({AudioEngine::RequestKind::RESUME_CAPTURE, // Test the condition !request_audio({AudioEngine::RequestKind::RESUME_CAPTURE,.
                        pending_.operation_id})) { // Finish the parameters/condition and enter the block below.
        pending_.audio_done = true; // Set pending .audio done to true.
        transport_.store(phono::protocol::Transport::Error); // Update the shared transport value atomically.
    } // End this block, declaration or argument list.
    if (pending_.file_done && pending_.audio_done) { // Test the condition pending_.file_done && pending_.audio_done.
        complete_operation(false, pending_.error_code); // Publish the final command result and clear Main's pending context.
    } // End this block, declaration or argument list.
} // End this function or local scope.


}  // namespace phono_app
